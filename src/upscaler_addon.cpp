// ReShade add-on (PhantomFX.addon64): NVIDIA DLSS or AMD FSR before the game's grading, in place of
// its FXAA (upscaler.hpp, fsr_upscaler.hpp), as DLAA or upscaling from a lower render size
// (render_scale.hpp, ui_redirect.hpp). It lets the game start under ReShade (device_check.hpp), trades
// the game's motion blur for its velocity passes (motion_blur.hpp), keeps the FXAA pass with post
// processing down (fxaa.hpp), can lift its 60 fps cap (frame_rate.hpp), adds DLSS Frame Generation
// with Reflex or FSR frame generation (presenter.hpp, game_loop.hpp, fsr_frame_generation.hpp), can
// give the sun soft shadows and every light Bend Studio's screen-space shadows (shadows.hpp), can
// replace the game's ambient occlusion with GTVBAO and add GT-VBGI's screen-space indirect lighting
// (indirect_light.hpp), or replace the game's indirect light with ray-traced global illumination
// (global_illumination.hpp), adds volumetric fog with light shafts (atmosphere.hpp), has a photo mode
// (photo_mode.hpp), can output HDR10 (hdr.hpp) and has an optional pipeline recorder (frame_log.hpp),
// the GPU costs of its passes (performance.hpp) and weather and time control (weather.hpp) for
// development.
// NGX receives ReShade's wrapped context, so other add-ons that follow DLSS evaluations see it like
// a game's own DLSS.
#include "atmosphere.hpp"
#include "character.hpp"
#include "d3d12_helpers.hpp"
#include "device_check.hpp"
#include "draw_distance.hpp"
#include "frame_log.hpp"
#include "frame_rate.hpp"
#include "focus.hpp"
#include "fsr_frame_generation.hpp"
#include "fxaa.hpp"
#include "fxui/fxui.hpp"
#include "engine_scene.hpp"
#include "game_loop.hpp"
#include "global_illumination.hpp"
#include "hdr.hpp"
#include "indirect_light.hpp"
#include "local_lights.hpp"
#include "log.hpp"
#include "motion_blur.hpp"
#include "performance.hpp"
#include "photo_mode.hpp"
#include "presenter.hpp"
#include "render_scale.hpp"
#include "shadows.hpp"
#include "sun_movement.hpp"
#include "upscaler.hpp"
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
        + (supportedBuild() ? " (supported)." : " (not supported)."));
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
    shadows::attach(device);
    indirect_light::attach(device);
    global_illumination::attach(device, context);
    local_lights::attach(device, context);
    atmosphere::attach(device);
    performance::attach(device, context, dataDirectory() / "PhantomFX-performance.txt");
    log("Immediate context " + hex(reinterpret_cast<std::uintptr_t>(context)) + ", ReShade wrapper "
        + hex(reinterpret_cast<std::uintptr_t>(wrapper)) + ".");
}

// The performance measurement first: one running gives the effects back.
void detachEffects()
{
    performance::detach();
    upscaler::detach();
    shadows::detach();
    indirect_light::detach();
    global_illumination::detach();
    local_lights::detach();
    atmosphere::detach();
    focus::detach();
    hdr::detach();
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
            // Shaders HDR made from patched code are known by the game's own.
            const auto [code, size] = hdr::originalOf(shader->code, shader->code_size);
            auto* created = reinterpret_cast<IUnknown*>(pipeline.handle);
            upscaler::shaderCreated(code, size, created);
            shadows::shaderCreated(code, size, created);
            indirect_light::shaderCreated(code, size, created);
            global_illumination::pixelShaderCreated(code, size, created);
            local_lights::pixelShaderCreated(code, size, created);
            atmosphere::shaderCreated(code, size, created);
            hdr::shaderCreated(code, size, created);
        }
        else if (subobjects[i].type == pipeline_subobject_type::vertex_shader)
        {
            ui_redirect::vertexShaderCreated(shader->code, shader->code_size, reinterpret_cast<IUnknown*>(pipeline.handle));
            global_illumination::vertexShaderCreated(shader->code, shader->code_size, reinterpret_cast<IUnknown*>(pipeline.handle));
        }
    }
}

// With the UI hidden, the 2D pass's draws into the scene are skipped.
bool hiddenUi(command_list* list)
{
    return photo_mode::uiHidden() && ui_redirect::drawing2D(native(list)) && ui_redirect::drawingIntoScene(native(list));
}

bool onDraw(command_list* list, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t firstInstance)
{
    const ui_redirect::DrawCall call{vertices, instances, first, firstInstance, 0, false};
    if (hiddenUi(list)) return true;

    auto* context = native(list);
    // HDR's work comes first, as others may make the draw: the resolve's curve, the depth of field's
    // float copy. A draw through an sRGB view of the scene (the resolve) writes linear colour, which
    // is encoded once drawn, by whoever draws it.
    const bool encode = hdr::beforeDraw(context, call);
    // The global illumination only notes what it needs (the sky's probe constants).
    global_illumination::draw(context, call);
    // While upscaling, the game's grading draws at the output size, whoever draws it.
    if (upscaler::beforePostDraw(context, call))
    {
        if (!hdr::draw(context, call)) context->DrawInstanced(vertices, instances, first, firstInstance);
        upscaler::afterPostDraw(context);

        return true;
    }

    // The fog notes the sun pass and fills its texture before the resolve, whoever draws them. GTVBAO
    // notes the sun pass's G-buffer before the sun shadows may draw that pass themselves.
    const bool drawn = atmosphere::draw(context, call) || indirect_light::draw(context, call) || shadows::draw(context, call)
        || hdr::draw(context, call) || upscaler::draw(context, call);
    if (!encode) return drawn;

    if (!drawn) context->DrawInstanced(vertices, instances, first, firstInstance);
    hdr::encode(context);

    return true;
}

bool onDrawIndexed(command_list* list, uint32_t indices, uint32_t instances, uint32_t first, int32_t offset, uint32_t firstInstance)
{
    const ui_redirect::DrawCall call{indices, instances, first, firstInstance, offset, true};
    // The global illumination notes the camera and the terrain's inputs from the draws; it never takes one.
    global_illumination::draw(native(list), call);
    // The point and spot lights' draws before the lamps' contact shadows, which leave a light the rays shadow alone.
    return hiddenUi(list) || local_lights::draw(native(list), call) || shadows::draw(native(list), call) || upscaler::draw(native(list), call);
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

void onBindPipeline(command_list* list, pipeline_stage stages, pipeline pipeline)
{
    if ((stages & pipeline_stage::vertex_shader) == pipeline_stage::vertex_shader)
    {
        ui_redirect::vertexShaderBound(native(list), reinterpret_cast<IUnknown*>(pipeline.handle));
        global_illumination::vertexShaderBound(native(list), reinterpret_cast<IUnknown*>(pipeline.handle));
    }
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

// ---- HDR's swap chain and targets, registered only while HDR runs ----

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
    if (!hdr::active() || !hdr::upgradeTexture(textureFormat, unorderedAccess)) return false;

    desc.texture.format = static_cast<format>(textureFormat);
    if (unorderedAccess) desc.usage |= resource_usage::unordered_access;

    return true;
}

void onInitResource(device*, const resource_desc&, const subresource_data*, resource_usage, resource created)
{
    atmosphere::textureCreated(reinterpret_cast<ID3D11Resource*>(created.handle));
    if (hdr::active()) hdr::textureCreated(reinterpret_cast<ID3D11Resource*>(created.handle));
}

void onDestroyResource(device*, resource destroyed)
{
    atmosphere::resourceDestroyed(reinterpret_cast<ID3D11Resource*>(destroyed.handle));
    if (hdr::active()) hdr::resourceDestroyed(reinterpret_cast<ID3D11Resource*>(destroyed.handle));
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
    global_illumination::bufferMapped(reinterpret_cast<ID3D11Resource*>(buffer.handle), *data);
}

// The upscaler adds the jitter to the scene constants first; the screen-space shadows and the global
// illumination read them as jittered.
void onUnmapBuffer(device*, resource buffer)
{
    upscaler::bufferUnmapped(reinterpret_cast<ID3D11Resource*>(buffer.handle));
    global_illumination::bufferUnmapped(reinterpret_cast<ID3D11Resource*>(buffer.handle));
}

// Called before the immediate context executes a command list, and after FinishCommandList with the
// new command list and the deferred context it came from.
void onExecuteSecondary(command_list* list, command_list* secondary)
{
    if (list == immediateList)
    {
        // The frame's timing starts before DLSS, which runs here.
        performance::executing(secondary->get_native());
        upscaler::executing(native(list), secondary->get_native());
        global_illumination::executing(native(list), secondary->get_native());
        // After the global illumination's wait: the point and spot lights read their shadows.
        local_lights::executing(native(list), secondary->get_native());
        return;
    }

    performance::recorded(list->get_native(), secondary->get_native());
    upscaler::recorded(list->get_native(), secondary->get_native());
    global_illumination::recorded(list->get_native(), secondary->get_native());
    local_lights::recorded(list->get_native(), secondary->get_native());
    ui_redirect::contextFinished(native(secondary));
    shadows::contextFinished(native(secondary));
}

void onDestroyCommandList(command_list* list)
{
    upscaler::forget(list->get_native());
    performance::forget(list->get_native());
    global_illumination::forget(list->get_native());
    local_lights::forget(list->get_native());
}

// With frame generation the game's swap chain is the presenter's stand-in, whose Present follows.
void onPresent(command_queue* queue, swapchain*, const rect*, const rect*, uint32_t, const rect*)
{
    if (queue->get_immediate_command_list() != immediateList) return;

    const bool generating = presenter::active();
    upscaler::setFrameGeneration(generating);

    if (generating)
    {
        const auto mode = upscaler::currentSettings().frameGeneration;
        frame_generation::select(upscaler::multiplier(mode), mode == upscaler::FrameGeneration::Dynamic);
        fsr_frame_generation::select(mode == upscaler::FrameGeneration::Fsr);
        presenter::submit(upscaler::frameGenerationInputs());
    }

    upscaler::present();
    shadows::present();
    indirect_light::present();
    global_illumination::present();
    local_lights::present();
    atmosphere::present(native(queue->get_immediate_command_list()));
    performance::present();
}

void onOverlay(effect_runtime*)
{
    upscaler::drawMenu();
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
    if (runtime->is_key_pressed(VK_F1)) upscaler::toggleEnhancements();
    if (runtime->is_key_pressed(VK_F2)) photo_mode::toggle();
    if (runtime->is_key_pressed(VK_F3)) photo_mode::toggleUi();
    if (runtime->is_key_pressed(VK_F4)) photo_mode::toggleBlur();

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
    log(draw_distance::hook());
    log(engine_scene::hook());
    log(character::allowFobCharacters());
    log(sun_movement::makeSmooth());
    if (supportedBuild() && !weather::supported()) log("The weather manager has unexpected code; the weather menu is unavailable.");

    frame_log::install(directory);
    upscaler::initialize(directory, supportedBuild());
    // The game makes its swap chain and targets next, so HDR is decided now.
    hdr::enable(supportedBuild() && upscaler::currentSettings().hdr);

    // Frame generation takes over the swap chain the game creates next, so it starts with the game.
    if (const auto mode = upscaler::currentSettings().frameGeneration; supportedBuild() && mode != upscaler::FrameGeneration::Off)
    {
        // The stand-in swap chain and the hooks point into this module; keep it loaded until exit.
        HMODULE pinned{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&AddonInit), &pinned);
        presenter::install(directory, mode == upscaler::FrameGeneration::Fsr ? presenter::Route::Fsr : presenter::Route::Streamline);
    }

    reshade::register_event<reshade::addon_event::init_command_queue>(onInitCommandQueue);
    reshade::register_event<reshade::addon_event::destroy_command_queue>(onDestroyCommandQueue);
    reshade::register_event<reshade::addon_event::init_pipeline>(onInitPipeline);
    reshade::register_event<reshade::addon_event::draw>(onDraw);
    reshade::register_event<reshade::addon_event::draw_indexed>(onDrawIndexed);
    reshade::register_event<reshade::addon_event::clear_render_target_view>(onClearRenderTarget);
    reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(onClearDepthStencil);
    reshade::register_event<reshade::addon_event::push_descriptors>(onPushDescriptors);
    reshade::register_event<reshade::addon_event::bind_pipeline>(onBindPipeline);
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

    // The game's fog texture is made larger as it is created (atmosphere.hpp), and HDR's targets
    // change format (hdr.hpp).
    if (supportedBuild())
    {
        reshade::register_event<reshade::addon_event::create_resource>(onCreateResource);
        reshade::register_event<reshade::addon_event::init_resource>(onInitResource);
        reshade::register_event<reshade::addon_event::destroy_resource>(onDestroyResource);
    }

    // Every view and copy passes these, so they are only there while HDR runs.
    if (hdr::active())
    {
        reshade::register_event<reshade::addon_event::create_swapchain>(onCreateSwapchain);
        reshade::register_event<reshade::addon_event::create_resource_view>(onCreateResourceView);
        reshade::register_event<reshade::addon_event::init_resource_view>(onInitResourceView);
        reshade::register_event<reshade::addon_event::destroy_resource_view>(onDestroyResourceView);
        reshade::register_event<reshade::addon_event::copy_resource>(onCopyResource);
        reshade::register_event<reshade::addon_event::create_pipeline>(onCreatePipeline);
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
