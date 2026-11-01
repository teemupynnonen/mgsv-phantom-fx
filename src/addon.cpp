// ReShade add-on (PhantomFX.addon64): NVIDIA DLSS or AMD FSR before the game's grading, in place of
// its FXAA, as DLAA or upscaling from a lower render size (render_scale.hpp, ui_redirect.hpp), and
// the rest of Phantom FX's features (features/, one folder each), plugged into the game's passes
// they share (passes/). It lets the game start under ReShade (device_check.hpp), holds the settings
// and hands every change to the modules (core.hpp), draws the settings window (menu.hpp), and has
// an optional pipeline recorder (frame_log.hpp), the GPU costs of its passes (performance.hpp) and
// weather and time control (weather.hpp) for development.
// NGX receives ReShade's wrapped context, so other add-ons that follow DLSS evaluations see it like
// a game's own DLSS.
#include "character.hpp"
#include "constant_capture.hpp"
#include "core.hpp"
#include "d3d12_helpers.hpp"
#include "dev/dev.hpp"
#include "dev_tools.hpp"
#include "device_check.hpp"
#include "features/frame_generation/fsr_frame_generation.hpp"
#include "features/frame_generation/presenter.hpp"
#include "features/frame_rate/frame_rate.hpp"
#include "features/hdr/hdr.hpp"
#include "features/lamp_contact_shadows/lamp_contact_shadows.hpp"
#include "features/depth_of_field/depth_of_field.hpp"
#include "features/motion_blur/motion_blur.hpp"
#include "features/path_tracing/forward_surfaces.hpp"
#include "features/path_tracing/glass.hpp"
#include "features/path_tracing/path_tracing.hpp"
#include "features/path_tracing/sky_capture.hpp"
#include "features/photo_mode/photo_mode.hpp"
#include "features/ray_traced_lights/local_lights.hpp"
#include "features/sea/sea.hpp"
#include "features/sun_contact_shadows/sun_contact_shadows.hpp"
#include "features/upscaling/fxaa.hpp"
#include "features/upscaling/upscaler.hpp"
#include "features/volumetric_fog/atmosphere.hpp"
#include "frame_log.hpp"
#include "fxui/fxui.hpp"
#include "game_dither.hpp"
#include "game_loop.hpp"
#include "game_shaders.hpp"
#include "log.hpp"
#include "menu.hpp"
#include "modules.hpp"
#include "passes/forward_water_pass.hpp"
#include "passes/indirect_light_pass.hpp"
#include "passes/local_light_pass.hpp"
#include "passes/ray_tracing/engine_scene.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "passes/sun_light_pass.hpp"
#include "performance.hpp"
#include "remaster/remaster.hpp"
#include "render_scale.hpp"
#include "sun_movement.hpp"
#include "weather.hpp"

#include <Windows.h>
#include <Psapi.h>
#include <MinHook.h>
#include <d3d11.h>
// Before reshade.hpp, so register_addon also connects the ImGui calls of the overlay to ReShade's.
#include <imgui.h>
#include <reshade.hpp>
#include <array>
#include <filesystem>
#include <string>

extern "C" __declspec(dllexport) const char* NAME = "Phantom FX";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "NVIDIA DLSS, AMD FSR, frame generation, HDR and better shadows and lighting for Metal Gear Solid V: The Phantom Pain (Steam version 1.0.15.4).";

namespace
{
using namespace phantom_fx;
using namespace reshade::api;

HMODULE self{};
command_list* immediateList{};
bool overlayOpen{};
// ReShade's runtime, which draws its effects and overlay on the back buffer in the colour space it is told.
effect_runtime* effects{};

ID3D11DeviceContext* native(command_list* list)
{
    return reinterpret_cast<ID3D11DeviceContext*>(list->get_native());
}

// Whether `list` is a device context rather than a command list.
bool isContext(command_list* list)
{
    ID3D11DeviceContext* context{};
    if (FAILED(reinterpret_cast<IUnknown*>(list->get_native())->QueryInterface(IID_PPV_ARGS(&context)))) return false;

    context->Release();
    return true;
}

// The settings, log and frame records live beside the add-on, as ReShade keeps its own.
std::filesystem::path dataDirectory()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        const auto length = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return std::filesystem::current_path();
        if (length < path.size()) return std::filesystem::path(path.substr(0, length)).parent_path();

        path.resize(path.size() * 2);
    }
}

void logGameBuild()
{
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE)
    {
        log("The executable has no PE header.");
        return;
    }

    log("Game executable: TimeDateStamp " + hex(nt->FileHeader.TimeDateStamp) + ", SizeOfImage " + hex(nt->OptionalHeader.SizeOfImage)
        + (isSupportedGameBuild() ? " (supported)." : " (not supported)."));
}

// The command queue ReShade reports is a base inside its D3D11DeviceContext proxy; find the proxy
// itself and prove it by asking for the native context it wraps.
ID3D11DeviceContext* wrapperOf(command_queue* queue, ID3D11DeviceContext* context)
{
    const auto address = reinterpret_cast<std::uintptr_t>(queue);
    HMODULE reshade{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            *reinterpret_cast<LPCWSTR*>(address), &reshade))
        return nullptr;

    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), reshade, &info, sizeof(info))) return nullptr;

    const auto begin = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll), end = begin + info.SizeOfImage;
    for (std::uintptr_t offset = 8; offset <= 32; offset += 8)
    {
        const auto candidate = address - offset;
        const auto vtable = *reinterpret_cast<const std::uintptr_t*>(candidate);
        if (vtable < begin || vtable >= end) continue;

        IUnknown* unwrapped{};
        if (FAILED(reinterpret_cast<IUnknown*>(candidate)->QueryInterface(UnwrappedObject, reinterpret_cast<void**>(&unwrapped)))) continue;

        const bool match = unwrapped == context;
        unwrapped->Release();
        if (match) return reinterpret_cast<ID3D11DeviceContext*>(candidate);
    }

    return nullptr;
}

void onInitCommandQueue(command_queue* queue)
{
    auto* context = reinterpret_cast<ID3D11DeviceContext*>(queue->get_native());
    if (queue->get_device()->get_api() != device_api::d3d11 || !context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || immediateList)
        return;

    immediateList = queue->get_immediate_command_list();
    auto* wrapper = wrapperOf(queue, context);
    auto* device = reinterpret_cast<ID3D11Device*>(queue->get_device()->get_native());
    hdr::attach(device);
    upscaler::attach(device, context, wrapper);
    modules::attach(device, context);
    // Last: a first launch's choice of upscaler reconfigures the modules, which then have the device.
    core::attach(device);
    performance::attach(device, context, dataDirectory() / "PhantomFX-performance.txt");
    log("Immediate context " + hex(reinterpret_cast<std::uintptr_t>(context)) + ", ReShade wrapper "
        + hex(reinterpret_cast<std::uintptr_t>(wrapper)) + ".");
}

// The performance measurement first: one running gives the effects back.
void detachEffects()
{
    performance::detach();
    core::detach();
    upscaler::detach();
    modules::detach();
    hdr::detach();
    game_shaders::forget();
}

void onDestroyCommandQueue(command_queue* queue)
{
    if (queue->get_immediate_command_list() != immediateList) return;

    detachEffects();
    immediateList = nullptr;
}

void onInitPipeline(device* device, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipeline)
{
    if (device->get_api() != device_api::d3d11) return;

    for (uint32_t i = 0; i < count; ++i)
    {
        const auto* shader = static_cast<const shader_desc*>(subobjects[i].data);
        if (!shader) continue;

        if (subobjects[i].type == pipeline_subobject_type::pixel_shader)
        {
            // Shaders made from patched code are known by the game's own.
            const auto [hdrOriginal, hdrSize] = hdr::originalOf(shader->code, shader->code_size);
            const auto [code, size] = game_dither::originalOf(hdrOriginal, hdrSize);
            game_shaders::pixelShaderCreated(code, size, reinterpret_cast<IUnknown*>(pipeline.handle));
        }
        else if (subobjects[i].type == pipeline_subobject_type::vertex_shader)
            game_shaders::vertexShaderCreated(shader->code, shader->code_size, reinterpret_cast<IUnknown*>(pipeline.handle));
    }
}

// With the UI hidden, the 2D pass's draws into the scene are skipped.
bool hiddenUi(command_list* list)
{
    return photo_mode::uiHidden() && ui_redirect::drawing2D(native(list)) && ui_redirect::drawingIntoScene(native(list));
}

// A draw meets the modules in three steps: the notes take what they need from it and never draw it;
// while upscaling, the grading is redirected to the output size; then the owners, the first of which
// that draws it in the game's place, or skips it, takes it from the game.
bool onDraw(command_list* list, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t firstInstance)
{
    const ui_redirect::DrawCall call{vertices, instances, first, firstInstance, 0, false};
    if (hiddenUi(list)) return true;

    auto* context = native(list);
    game_shaders::DrawnShader shader(context);

    // 1. The notes. HDR's: the resolve's curve, the depth of field's float copy. A draw through an sRGB
    // view of the scene (the resolve) writes linear colour, which is encoded once drawn, by whoever
    // draws it.
    const bool encode = hdr::beforeDraw(context, call, shader);
    ray_tracing::note(context, call, shader);
    atmosphere::note(context, call, shader);
    indirect_light_pass::note(context, call, shader);
    path_tracing::note(context, call, shader);
    forward_surfaces::note(context, shader);
    sea::note(context, call, shader);

    // 2. While upscaling, the game's grading draws at the output size, whoever draws it.
    if (upscaler::beforePostDraw(context, call, shader))
    {
        if (!hdr::draw(context, call, shader)) context->DrawInstanced(vertices, instances, first, firstInstance);
        upscaler::afterPostDraw(context);

        return true;
    }

    // 3. The owners. Each takes game passes of its own (the upscaler also the 2D pass), so their order
    // does not matter.
    const bool drawn = atmosphere::draw(context, call, shader) || indirect_light_pass::draw(context, call, shader)
        || sun_light_pass::draw(context, call, shader) || path_tracing::draw(context, call, shader) || sea::draw(context, call, shader)
        || hdr::draw(context, call, shader) || depth_of_field::draw(context, call, shader) || upscaler::draw(context, call, shader);

    // The game's sky, then again for the rays; the physical sky's own draw does the same (atmosphere.cpp).
    if (!drawn && sky_capture::isWanted())
        if (const auto* row = shader.row(); row && row->pass == game_shaders::GamePass::Sky)
        {
            context->DrawInstanced(vertices, instances, first, firstInstance);
            sky_capture::capture(context, call);

            return true;
        }

    if (!encode) return drawn;

    if (!drawn) context->DrawInstanced(vertices, instances, first, firstInstance);
    hdr::encode(context);

    return true;
}

bool onDrawIndexed(command_list* list, uint32_t indices, uint32_t instances, uint32_t first, int32_t offset, uint32_t firstInstance)
{
    const ui_redirect::DrawCall call{indices, instances, first, firstInstance, offset, true};
    auto* context = native(list);
    game_shaders::DrawnShader shader(context);
    ray_tracing::note(context, call, shader);
    sea::note(context, call, shader);
    // The one order among the owners that matters: a point or spot light's draws have two, and the
    // ray-traced lights come first, so the local light pass never draws a light the rays shadow.
    if (hiddenUi(list) || local_lights::draw(context, call, shader) || local_light_pass::draw(context, call, shader) || sea::draw(context, call, shader)
        || forward_water_pass::draw(context, call, shader) || glass::draw(context, call, shader) || upscaler::draw(context, call, shader))
        return true;

    // The sky's dome and clouds, then again for the rays.
    if (!sky_capture::isWanted()) return false;

    const auto* row = shader.row();
    if (!row || row->pass != game_shaders::GamePass::SkyDome) return false;

    context->DrawIndexedInstanced(indices, instances, first, offset, firstInstance);
    sky_capture::capture(context, call);

    return true;
}

bool onClearRenderTarget(command_list* list, resource_view view, const float color[4], uint32_t rects, const rect*)
{
    auto* target = reinterpret_cast<ID3D11RenderTargetView*>(view.handle);
    return !rects && (hdr::clearTarget(native(list), target, color) || upscaler::clearTarget(native(list), target, color));
}

bool onClearDepthStencil(command_list* list, resource_view view, const float* depth, const uint8_t* stencil, uint32_t rects, const rect*)
{
    const UINT flags = (depth ? D3D11_CLEAR_DEPTH : 0) | (stencil ? D3D11_CLEAR_STENCIL : 0);
    if (!rects)
        upscaler::depthStencilCleared(native(list), reinterpret_cast<ID3D11DepthStencilView*>(view.handle), flags, depth ? *depth : 0,
            stencil ? *stencil : 0);

    return false;
}

void onBindRenderTargets(command_list* list, uint32_t count, const resource_view* targets, resource_view depth)
{
    auto* const views = reinterpret_cast<ID3D11RenderTargetView* const*>(targets);
    auto* const depthView = reinterpret_cast<ID3D11DepthStencilView*>(depth.handle);
    sea::targetsBound(native(list), count, views, depthView);
    indirect_light_pass::targetsBound(count, views, depthView);
}

void onBindPipeline(command_list* list, pipeline_stage stages, pipeline pipeline)
{
    if ((stages & pipeline_stage::vertex_shader) != pipeline_stage::vertex_shader) return;

    const auto kind = game_shaders::vertexShaderOf(reinterpret_cast<IUnknown*>(pipeline.handle));
    ui_redirect::vertexShaderBound(native(list), kind == game_shaders::VertexShader::Draw2D);
    ray_tracing::vertexShaderBound(native(list), kind);
}

// ReShade reports bound samplers and views after binding them; rebind biased samplers, and HDR's
// decoded views, on the native context.
void onPushDescriptors(command_list* list, shader_stage stages, pipeline_layout, uint32_t, const descriptor_table_update& update)
{
    if ((stages & shader_stage::pixel) != shader_stage::pixel) return;

    if (update.type == descriptor_type::shader_resource_view && update.count <= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
    {
        std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> replaced;
        const auto* views = static_cast<ID3D11ShaderResourceView* const*>(update.descriptors);
        if (hdr::replaceViews(update.count, views, replaced.data())) native(list)->PSSetShaderResources(update.binding, update.count, replaced.data());

        return;
    }

    if (update.type != descriptor_type::sampler || update.count > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) return;

    std::array<ID3D11SamplerState*, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> biased{};
    const auto* samplers = static_cast<ID3D11SamplerState* const*>(update.descriptors);
    if (upscaler::biasSamplers(native(list), update.count, samplers, biased.data()))
        native(list)->PSSetSamplers(update.binding, update.count, biased.data());
}

void showColorSpace()
{
    if (effects && hdr::active())
        effects->set_color_space(hdr::output() == hdr::Output::Hdr10 ? color_space::hdr10_st2084 : color_space::srgb);
}

void onInitSwapchain(swapchain* chain, bool)
{
    const auto desc = chain->get_device()->get_resource_desc(chain->get_current_back_buffer());
    upscaler::setOutputSize(desc.texture.width, desc.texture.height);
    // ReShade does not pass on a colour space change once an add-on changed the back buffer format,
    // so HDR sets it on the native swap chain.
    hdr::swapChainReady(reinterpret_cast<IDXGISwapChain*>(chain->get_native()));
    showColorSpace();
}

void onInitEffectRuntime(effect_runtime* created)
{
    effects = created;
    showColorSpace();
}

void onDestroyEffectRuntime(effect_runtime* destroyed)
{
    if (effects == destroyed) effects = nullptr;
}

// ---- The float scene's targets and HDR's swap chain, registered only while the scene is float ----

bool onCreateSwapchain(device_api api, swapchain_desc& desc, void*)
{
    auto bufferFormat = static_cast<DXGI_FORMAT>(desc.back_buffer.texture.format);
    auto swapEffect = static_cast<DXGI_SWAP_EFFECT>(desc.present_mode);
    UINT buffers = desc.back_buffer_count;
    if (api != device_api::d3d11 || !hdr::upgradeSwapChain(bufferFormat, swapEffect, buffers)) return false;

    desc.back_buffer.texture.format = static_cast<format>(bufferFormat);
    desc.present_mode = swapEffect;
    desc.back_buffer_count = buffers;

    return true;
}

bool onCreateResource(device* device, resource_desc& desc, subresource_data*, resource_usage)
{
    if (device->get_api() != device_api::d3d11 || desc.type != resource_type::texture_2d) return false;

    auto textureFormat = static_cast<DXGI_FORMAT>(desc.texture.format);
    const bool renderTarget = (desc.usage & resource_usage::render_target) == resource_usage::render_target;
    if (atmosphere::upgradeTexture(desc.texture.width, desc.texture.height, textureFormat, renderTarget))
    {
        desc.usage |= resource_usage::unordered_access;
        return true;
    }

    bool unorderedAccess{};
    if (!hdr::isSceneFloat() || !hdr::upgradeTexture(textureFormat, unorderedAccess)) return false;

    desc.texture.format = static_cast<format>(textureFormat);
    if (unorderedAccess) desc.usage |= resource_usage::unordered_access;

    return true;
}

void onInitResource(device*, const resource_desc&, const subresource_data*, resource_usage, resource created)
{
    atmosphere::textureCreated(reinterpret_cast<ID3D11Resource*>(created.handle));
    if (hdr::isSceneFloat()) hdr::textureCreated(reinterpret_cast<ID3D11Resource*>(created.handle));
}

void onDestroyResource(device*, resource destroyed)
{
    atmosphere::resourceDestroyed(reinterpret_cast<ID3D11Resource*>(destroyed.handle));
    if (hdr::isSceneFloat()) hdr::resourceDestroyed(reinterpret_cast<ID3D11Resource*>(destroyed.handle));
}

bool onCreateResourceView(device* device, resource viewed, resource_usage, resource_view_desc& desc)
{
    if (device->get_api() != device_api::d3d11) return false;

    auto viewFormat = static_cast<DXGI_FORMAT>(desc.format);
    if (!hdr::upgradeView(reinterpret_cast<ID3D11Resource*>(viewed.handle), viewFormat)) return false;

    desc.format = static_cast<format>(viewFormat);

    return true;
}

void onInitResourceView(device*, resource, resource_usage, const resource_view_desc&, resource_view created)
{
    hdr::viewCreated(reinterpret_cast<ID3D11View*>(created.handle));
}

void onDestroyResourceView(device*, resource_view destroyed)
{
    hdr::viewDestroyed(reinterpret_cast<ID3D11View*>(destroyed.handle));
}

bool onCreatePipeline(device* device, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects)
{
    if (device->get_api() != device_api::d3d11) return false;

    bool changed{};
    for (uint32_t i = 0; i < count; ++i)
    {
        if (subobjects[i].type != pipeline_subobject_type::pixel_shader || !subobjects[i].data) continue;

        auto* shader = static_cast<shader_desc*>(subobjects[i].data);
        // The dither first: HDR's copy of the code it patches keeps the undithered code's checksum,
        // by which game_dither knows it.
        if (const auto* undithered = game_dither::undithered(reinterpret_cast<ID3D11Device*>(device->get_native()), shader->code, shader->code_size))
        {
            shader->code = undithered->data();
            shader->code_size = undithered->size();
            changed = true;
        }

        if (const auto* patched = hdr::patchedShader(shader->code, shader->code_size))
        {
            shader->code = patched->data();
            shader->code_size = patched->size();
            changed = true;
        }
    }

    return changed;
}

bool onCopyResource(command_list* list, resource source, resource destination)
{
    return hdr::copy(native(list), reinterpret_cast<ID3D11Resource*>(source.handle), reinterpret_cast<ID3D11Resource*>(destination.handle));
}

void onMapBuffer(device*, resource buffer, uint64_t, uint64_t, map_access access, void** data)
{
    if (access == map_access::read_only || !data) return;

    upscaler::bufferMapped(reinterpret_cast<ID3D11Resource*>(buffer.handle), *data);
    constant_capture::bufferMapped(reinterpret_cast<ID3D11Resource*>(buffer.handle), *data);
}

// The upscaler adds the jitter to the scene constants first; the constant capture copies them after it,
// so the contact shadows and the ray tracing read them as jittered.
void onUnmapBuffer(device*, resource buffer)
{
    upscaler::bufferUnmapped(reinterpret_cast<ID3D11Resource*>(buffer.handle));
    constant_capture::bufferUnmapped(reinterpret_cast<ID3D11Resource*>(buffer.handle));
}

// Called before the immediate context executes a command list, after FinishCommandList with the new
// command list and the deferred context it came from, and before a deferred context executes a command
// list inside it.
void onExecuteSecondary(command_list* list, command_list* secondary)
{
    if (list == immediateList)
    {
        // The frame's timing starts before DLSS, which runs here.
        performance::executing(secondary->get_native());
        upscaler::executing(native(list), secondary->get_native());
        // Before the kick: the ray-traced scene's plane and the guides take the sea when its list ran.
        sea::executing(native(list), secondary->get_native());
        ray_tracing::executing(native(list), secondary->get_native());
        // After the global illumination's wait: the point and spot lights read their shadows.
        local_lights::executing(native(list), secondary->get_native());
        forward_surfaces::executing(native(list), secondary->get_native());
        return;
    }

    // The game is not known to do it: the list's notes join the context's (command_list_notes.hpp).
    if (isContext(list)) logOnce("A deferred context runs a command list inside it; the list's notes join the context's.");

    performance::recorded(list->get_native(), secondary->get_native());
    upscaler::recorded(list->get_native(), secondary->get_native());
    ray_tracing::recorded(list->get_native(), secondary->get_native());
    local_lights::recorded(list->get_native(), secondary->get_native());
    sea::recorded(list->get_native(), secondary->get_native());
    forward_surfaces::recorded(list->get_native(), secondary->get_native());
    ui_redirect::contextFinished(native(secondary));
    sun_contact_shadows::contextFinished(native(secondary));
    lamp_contact_shadows::contextFinished(native(secondary));
}

void onDestroyCommandList(command_list* list)
{
    upscaler::forget(list->get_native());
    performance::forget(list->get_native());
    ray_tracing::forget(list->get_native());
    local_lights::forget(list->get_native());
    sea::forget(list->get_native());
    forward_surfaces::forget(list->get_native());
}

// With frame generation the game's swap chain is the presenter's stand-in, whose Present follows.
void onPresent(command_queue* queue, swapchain* swapChain, const rect*, const rect*, uint32_t, const rect*)
{
    if (queue->get_immediate_command_list() != immediateList) return;

    if constexpr (DevTools)
        dev::onPresent(native(queue->get_immediate_command_list()), reinterpret_cast<ID3D11Resource*>(swapChain->get_current_back_buffer().handle));

    const bool generating = presenter::active();
    upscaler::setFrameGeneration(generating);
    motion_blur::setFrameGeneration(presenter::presentedPerFrame() > 1);

    if (generating)
    {
        const auto mode = core::currentSettings().frameGeneration.mode;
        frame_generation::select(upscaler::multiplier(mode), mode == upscaler::FrameGenerationMode::Dynamic);
        fsr_frame_generation::select(mode == upscaler::FrameGenerationMode::Fsr);
        presenter::submit(upscaler::frameGenerationInputs());
    }

    core::present();
    upscaler::present();
    modules::present();
    performance::present();
}

void onOverlay(effect_runtime*)
{
    menu::draw();
}

// Every frame, with ReShade's overlay open or closed.
void onReshadeOverlay(effect_runtime*)
{
    performance::drawOverlay();
    fxui::drawToast();
}

// ReShade's overlay takes the keyboard while it is open.
bool onOpenOverlay(effect_runtime*, bool open, input_source)
{
    overlayOpen = open;

    return false;
}

void onReshadePresent(effect_runtime* runtime)
{
    if (runtime->is_key_pressed(VK_F1)) core::toggleEnhancements();
    if (runtime->is_key_pressed(VK_F2)) photo_mode::toggle();
    if (runtime->is_key_pressed(VK_F3)) photo_mode::toggleUi();
    if (runtime->is_key_pressed(VK_F4)) photo_mode::toggleBlur();
    if constexpr (DevTools)
    {
        if (runtime->is_key_pressed(VK_F9)) dev::onCaseKey();
    }

    const auto down = [runtime](uint32_t key) { return !overlayOpen && runtime->is_key_down(key); };
    photo_mode::setControls({down('W'), down('S'), down('A'), down('D'), down(VK_SPACE), down(VK_CONTROL), down(VK_SHIFT)});
}
}

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon, HMODULE reshadeModule)
{
    if (!reshade::register_addon(addon, reshadeModule)) return false;

    self = addon;
    const auto directory = dataDirectory();
    openLog(directory / "PhantomFX.log");
    log(DiagnosticLog ? "Phantom FX " PHANTOM_FX_VERSION ", with the diagnostic log." : "Phantom FX " PHANTOM_FX_VERSION ".");
    logGameBuild();
    log(passDeviceCheck());

    // The engine hooks share one MinHook instance; a hook that cannot be made logs MinHook's reason.
    MH_Initialize();
    log(motion_blur::hook());
    log(fxaa::hook());
    log(frame_rate::hook());
    log(render_scale::hook());
    log(photo_mode::hook());
    log(game_loop::hook());
    log(engine_scene::hook());
    log(remaster::hook());
    log(character::allowFobCharacters());
    log(sun_movement::makeSmooth());
    if (isSupportedGameBuild() && !weather::supported()) log("The weather manager has unexpected code; the weather menu is unavailable.");

    frame_log::install(directory);
    if constexpr (DevTools) dev::install(directory);
    upscaler::initialize(directory);
    core::initialize(directory, isSupportedGameBuild());
    // The game makes its swap chain and targets next, so the float scene and HDR are decided now.
    if (isSupportedGameBuild()) hdr::enable(core::currentSettings().hdr.on ? hdr::Mode::HdrOutput : hdr::Mode::FloatScene);

    // Frame generation takes over the swap chain the game creates next, so it starts with the game.
    if (const auto mode = core::currentSettings().frameGeneration.mode; isSupportedGameBuild() && mode != upscaler::FrameGenerationMode::Off)
    {
        // The stand-in swap chain and the hooks point into this module; keep it loaded until exit.
        HMODULE pinned{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&AddonInit), &pinned);
        presenter::install(directory, mode == upscaler::FrameGenerationMode::Fsr ? presenter::Route::Fsr : presenter::Route::Streamline);
    }

    reshade::register_event<reshade::addon_event::init_command_queue>(onInitCommandQueue);
    reshade::register_event<reshade::addon_event::destroy_command_queue>(onDestroyCommandQueue);
    // The dither and HDR patches know this build's shaders only.
    if (isSupportedGameBuild()) reshade::register_event<reshade::addon_event::create_pipeline>(onCreatePipeline);

    reshade::register_event<reshade::addon_event::init_pipeline>(onInitPipeline);
    reshade::register_event<reshade::addon_event::draw>(onDraw);
    reshade::register_event<reshade::addon_event::draw_indexed>(onDrawIndexed);
    reshade::register_event<reshade::addon_event::clear_render_target_view>(onClearRenderTarget);
    reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(onClearDepthStencil);
    reshade::register_event<reshade::addon_event::push_descriptors>(onPushDescriptors);
    reshade::register_event<reshade::addon_event::bind_pipeline>(onBindPipeline);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(onBindRenderTargets);
    reshade::register_event<reshade::addon_event::init_swapchain>(onInitSwapchain);
    reshade::register_event<reshade::addon_event::map_buffer_region>(onMapBuffer);
    reshade::register_event<reshade::addon_event::unmap_buffer_region>(onUnmapBuffer);
    reshade::register_event<reshade::addon_event::execute_secondary_command_list>(onExecuteSecondary);
    reshade::register_event<reshade::addon_event::destroy_command_list>(onDestroyCommandList);
    reshade::register_event<reshade::addon_event::present>(onPresent);
    reshade::register_event<reshade::addon_event::reshade_open_overlay>(onOpenOverlay);
    reshade::register_event<reshade::addon_event::reshade_present>(onReshadePresent);
    reshade::register_event<reshade::addon_event::reshade_overlay>(onReshadeOverlay);
    reshade::register_event<reshade::addon_event::init_effect_runtime>(onInitEffectRuntime);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(onDestroyEffectRuntime);
    reshade::register_overlay("Phantom FX", onOverlay);

    // The game's fog texture is made larger as it is created (features/volumetric_fog/atmosphere.hpp),
    // and HDR's targets change format (hdr.hpp).
    if (isSupportedGameBuild())
    {
        reshade::register_event<reshade::addon_event::create_resource>(onCreateResource);
        reshade::register_event<reshade::addon_event::init_resource>(onInitResource);
        reshade::register_event<reshade::addon_event::destroy_resource>(onDestroyResource);
    }

    // Every view and copy passes these, so they are only there while the scene is float.
    if (hdr::isSceneFloat())
    {
        reshade::register_event<reshade::addon_event::create_swapchain>(onCreateSwapchain);
        reshade::register_event<reshade::addon_event::create_resource_view>(onCreateResourceView);
        reshade::register_event<reshade::addon_event::init_resource_view>(onInitResourceView);
        reshade::register_event<reshade::addon_event::destroy_resource_view>(onDestroyResourceView);
        reshade::register_event<reshade::addon_event::copy_resource>(onCopyResource);
    }

    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon, HMODULE reshadeModule)
{
    // ReShade unloads add-ons when its last device goes; the next load reports the game's device again.
    detachEffects();
    photo_mode::leave();
    // Removes every engine hook, so the game keeps running without the add-on.
    MH_Uninitialize();
    immediateList = nullptr;
    effects = nullptr;
    reshade::unregister_overlay("Phantom FX", onOverlay);
    frame_log::uninstall();
    reshade::unregister_addon(addon, reshadeModule);
}
