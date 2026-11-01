#include "passes/indirect_light_pass.hpp"

#include "d3d11_helpers.hpp"
#include "features/ambient_occlusion/gtvbao.hpp"
#include "features/global_illumination/light_combine.hpp"
#include "features/indirect_light/ssgi.hpp"
#include "features/path_tracing/path_tracing.hpp"
#include "features/ray_traced_lights/local_lights.hpp"
#include "frame_latch.hpp"
#include "game_constants.hpp"
#include "log.hpp"
#include "passes/indirect_light_pass_shaders.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "passes/visibility_bitmask.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace phantom_fx::indirect_light_pass
{
namespace
{
using Microsoft::WRL::ComPtr;
using game_shaders::GamePass, game_shaders::GameShader;

// The game passes the effects note (docs/DEVELOPMENT.md, "Ambient occlusion"): every sun pass, inside the
// shadow range or beyond it, and every resolve. The sky light binds the G-buffer normals right before
// the upscale in the same command list.
constexpr GamePass NotedPasses[]{GamePass::SkyLight, GamePass::IndirectUpscale, GamePass::Sun, GamePass::SkySpecular, GamePass::Resolve,
    GamePass::CameraVelocity};
// And those they draw in the game's place, or skip.
constexpr GamePass DrawnPasses[]{GamePass::IndirectUpscale, GamePass::Resolve, GamePass::GameOcclusion};

constexpr UINT NormalSlot = 0;     // The sky light's g_NormalTexture
constexpr UINT SpecularTarget = 2; // Among the G-buffer's targets
constexpr UINT LobeSlot = 8, FresnelSlot = 9, MaterialSlot = 10; // The sun's g_lut2, g_lut1 and g_MaterialTexture
constexpr UINT DepthSlot = 2;    // The upscale's inDepthBuffer
constexpr UINT SceneSlot = game_constants::SceneConstants::Slot; // In the upscale and the resolves
// Every resolve's inAlbedo, inLightDiffuse, inLightSpecular, inDepth and inOcclusion; the rain's inSpecular.
constexpr UINT AlbedoSlot = 8, DiffuseSlot = 9, SpecularLightSlot = 10, ResolveDepthSlot = 11, OcclusionSlot = 13, RainSpecularSlot = 3;

// The effects that change these passes, by the effect plan, and the diagnostics' views of them.
struct Composition
{
    bool occlusion{}, light{}, globalIllumination{};
    // The ray-traced scene runs, for the global illumination or the ray-traced shadows or lights: the
    // passes tell it what it needs (ray_tracing::noteNormals and the others).
    bool rayTracing{};
    bool showOcclusion{}, showLight{}, showGlobalIllumination{};
};

FrameLatch<Composition> latch;
std::atomic<bool> active{}, pipelineFailed{};

// The normals each context's sky light pass bound, for the upscale it records next. The resolve is
// in another command list; it takes the G-buffer normals any sky light pass bound last. The specular
// target comes from the G-buffer's own binding, not from the sun pass that reads it: some scenes have
// none (the hospital), and a view kept from an earlier one would hold on to a target the game replaced
// at a new render size, its picture frozen.
std::mutex gBufferMutex;
std::unordered_map<ID3D11DeviceContext*, ComPtr<ID3D11ShaderResourceView>> normals;
ComPtr<ID3D11ShaderResourceView> lastNormals, lastSpecular;
// The game's velocity buffer (camera and object motion, .ba) that this frame's CameraVelocity pass
// drew into, as a shader resource view; only drawn while the velocity passes run, which VB-SSGI
// forces on. Cleared each present: the buffer is rewritten every frame.
ComPtr<ID3D11Texture2D> velocityTexture;
ComPtr<ID3D11ShaderResourceView> velocityView;
bool velocityDrawn{};
MaterialTables lastTables;

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11PixelShader> showOcclusionShader, showLightShader;
ComPtr<ID3D11ShaderResourceView> whiteView; // 1x1, the resolve's occlusion while an effect replaces the game's
// The game's diffuse light with SSGI's or the global illumination's added, read by the resolve in
// its place.
visibility_bitmask::Target diffuseWithLight;

// Compiles the shaders and makes the fixed resources once; call with resourceMutex held. Returns
// false when they are unavailable.
bool ensurePipeline()
{
    if (whiteView) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        const D3D_SHADER_MACRO occlusionView[]{{"LIGHT", "0"}, {}}, lightView[]{{"LIGHT", "1"}, {}};
        showOcclusionShader = pixelShader(device.Get(), shaders::Show, "gtvbao-show", occlusionView);
        showLightShader = pixelShader(device.Get(), shaders::Show, "ssgi-show", lightView);

        D3D11_TEXTURE2D_DESC white{};
        white.Width = white.Height = white.MipLevels = white.ArraySize = 1;
        white.Format = DXGI_FORMAT_R8_UNORM;
        white.SampleDesc.Count = 1;
        white.Usage = D3D11_USAGE_IMMUTABLE;
        white.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        constexpr std::uint8_t unoccluded = 255;
        const D3D11_SUBRESOURCE_DATA data{&unoccluded, 1, 1};
        ComPtr<ID3D11Texture2D> whiteTexture;
        check(device->CreateTexture2D(&white, &data, whiteTexture.GetAddressOf()), "Creating the white occlusion");
        // Last, so it marks a complete pipeline.
        check(device->CreateShaderResourceView(whiteTexture.Get(), nullptr, whiteView.ReleaseAndGetAddressOf()), "Creating the white occlusion");
        log("Indirect light pass ready.");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        log(std::string("GTVBAO, SSGI and the global illumination's light are unavailable: ") + error.what());

        return false;
    }
}

void noteNormals(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11ShaderResourceView> view;
    context->PSGetShaderResources(NormalSlot, 1, view.GetAddressOf());
    std::lock_guard lock(gBufferMutex);
    lastNormals = view;
    normals[context] = std::move(view);
}

void noteTables(ID3D11DeviceContext* context)
{
    MaterialTables tables;
    context->PSGetShaderResources(LobeSlot, 1, tables.lobe.GetAddressOf());
    context->PSGetShaderResources(FresnelSlot, 1, tables.fresnel.GetAddressOf());
    context->PSGetShaderResources(MaterialSlot, 1, tables.materials.GetAddressOf());
    std::lock_guard lock(gBufferMutex);
    if (tables.lobe && tables.fresnel && tables.materials) lastTables = std::move(tables);
}

ComPtr<ID3D11ShaderResourceView> takeNormals(ID3D11DeviceContext* context)
{
    std::lock_guard lock(gBufferMutex);
    const auto found = normals.find(context);
    if (found == normals.end()) return nullptr;

    auto view = std::move(found->second);
    normals.erase(found);

    return view;
}

ComPtr<ID3D11ShaderResourceView> latestNormals()
{
    std::lock_guard lock(gBufferMutex);
    return lastNormals;
}

// The specular target the game bound last, if it has the render size; otherwise none, and the march
// treats nothing as foliage.
ComPtr<ID3D11ShaderResourceView> latestSpecular(std::pair<UINT, UINT> size)
{
    ComPtr<ID3D11ShaderResourceView> view;
    {
        std::lock_guard lock(gBufferMutex);
        view = lastSpecular;
    }

    return sizeOf(view.Get()) == size ? view : nullptr;
}

// The CameraVelocity pass (MotionBlurCameraVelocity) draws the game's velocity buffer — camera and
// object motion in .ba — as its render target, right after the G-buffer. Keep the target and make a
// view of it, for SSGI's temporal accumulation at the resolve.
void noteVelocity(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11RenderTargetView> target;
    context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
    ComPtr<ID3D11Resource> resource;
    if (target) target->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture;
    if (resource) resource->QueryInterface(IID_PPV_ARGS(&texture));
    if (!texture) return;

    std::lock_guard lock(resourceMutex);
    if (!device) return;
    std::lock_guard glock(gBufferMutex);
    if (texture != velocityTexture)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
        viewDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> view;
        const auto result = device->CreateShaderResourceView(texture.Get(), &viewDesc, view.GetAddressOf());
        logOnce(std::string("SSGI: velocity view ") + (SUCCEEDED(result) ? "created" : "failed (" + hex(static_cast<std::uint32_t>(result)) + ")") + ".");
        if (FAILED(result)) return;
        velocityTexture = texture;
        velocityView = std::move(view);
    }
    velocityDrawn = true;
}

// This frame's velocity buffer, if the CameraVelocity pass drew it; null otherwise.
ComPtr<ID3D11ShaderResourceView> velocity() noexcept
{
    std::lock_guard lock(gBufferMutex);
    return velocityDrawn ? velocityView : nullptr;
}

// The game's upscale, then GTVBAO's occlusion multiplied into what it wrote. False leaves the draw to the game.
bool drawUpscale(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call)
{
    const auto normalView = takeNormals(context);
    ComPtr<ID3D11ShaderResourceView> depth;
    ComPtr<ID3D11Buffer> scene;
    ComPtr<ID3D11RenderTargetView> light;
    context->PSGetShaderResources(DepthSlot, 1, depth.GetAddressOf());
    context->PSGetConstantBuffers(SceneSlot, 1, scene.GetAddressOf());
    context->OMGetRenderTargets(1, light.GetAddressOf(), nullptr);
    const auto specular = latestSpecular(sizeOf(depth.Get()));
    if (!gtvbao::compute(context, depth.Get(), normalView.Get(), specular.Get(), light.Get(), scene.Get())) return false;

    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    gtvbao::apply(context, call);

    return true;
}

// The resolve, with the diffuse light SSGI added to while it runs, and white in place of the
// game's occlusion while GTVBAO runs; or, for the diagnostics, either effect alone. False leaves
// the draw to the game.
bool drawResolve(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, const Composition& composition, ID3D11PixelShader* gameShader)
{
    const auto normalView = composition.light ? latestNormals() : nullptr;
    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;

    bool lit{};
    if (composition.light)
    {
        ComPtr<ID3D11ShaderResourceView> depth, diffuse, albedo, specular;
        ComPtr<ID3D11Buffer> scene;
        context->PSGetShaderResources(ResolveDepthSlot, 1, depth.GetAddressOf());
        context->PSGetShaderResources(DiffuseSlot, 1, diffuse.GetAddressOf());
        context->PSGetShaderResources(AlbedoSlot, 1, albedo.GetAddressOf());
        // The specular light the resolve adds to albedo x diffuse, as the RenoDX gather reads it;
        // not the G-buffer's specular target, whose rgb is material data.
        context->PSGetShaderResources(SpecularLightSlot, 1, specular.GetAddressOf());
        context->PSGetConstantBuffers(SceneSlot, 1, scene.GetAddressOf());
        const auto specularMaterial = latestSpecular(sizeOf(depth.Get()));
        lit = ssgi::compute(context, depth.Get(), normalView.Get(), diffuse.Get(), albedo.Get(), specular.Get(), specularMaterial.Get(), velocity().Get(),
            scene.Get(), diffuseWithLight);
    }

    if (!lit && !composition.occlusion) return false;

    ComPtr<ID3D11ShaderResourceView> gameDiffuse, gameOcclusion;
    context->PSGetShaderResources(DiffuseSlot, 1, gameDiffuse.GetAddressOf());
    context->PSGetShaderResources(OcclusionSlot, 1, gameOcclusion.GetAddressOf());

    auto* occlusion = composition.occlusion && composition.showOcclusion ? gtvbao::occlusion() : nullptr;
    if (lit) context->PSSetShaderResources(DiffuseSlot, 1, diffuseWithLight.view.GetAddressOf());
    if (composition.occlusion) context->PSSetShaderResources(OcclusionSlot, 1, whiteView.GetAddressOf());
    if (lit && composition.showLight) context->PSSetShader(showLightShader.Get(), nullptr, 0);
    else if (occlusion)
    {
        context->PSSetShader(showOcclusionShader.Get(), nullptr, 0);
        context->PSSetShaderResources(OcclusionSlot, 1, &occlusion);
    }

    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    context->PSSetShader(gameShader, nullptr, 0);
    context->PSSetShaderResources(DiffuseSlot, 1, gameDiffuse.GetAddressOf());
    context->PSSetShaderResources(OcclusionSlot, 1, gameOcclusion.GetAddressOf());

    return true;
}

// What the ray-traced scene needs from the game's passes (ray_tracing.hpp): the normals, the
// kick at the upscale, the wait at the resolve, and the sun; the first of the sun pass, the sky's specular
// pass and the resolve draws the point and spot lights too.
void noteForRayTracing(ID3D11DeviceContext* context, const GameShader& shader)
{
    ComPtr<ID3D11ShaderResourceView> view;
    switch (shader.pass)
    {
    case GamePass::SkyLight:
        context->PSGetShaderResources(NormalSlot, 1, view.GetAddressOf());
        ray_tracing::noteNormals(context, view.Get());
        return;
    case GamePass::IndirectUpscale:
        context->PSGetShaderResources(DepthSlot, 1, view.GetAddressOf());
        ray_tracing::noteUpscale(context, view.Get());
        return;
    case GamePass::Resolve:
        context->PSGetShaderResources(AlbedoSlot, 1, view.GetAddressOf());
        ray_tracing::noteResolve(context, view.Get());
        local_lights::noteDrawPoint(context);
        return;
    case GamePass::Sun:
        ray_tracing::noteSun(context, shader.isShadowMapped);
        local_lights::noteDrawPoint(context);
        return;
    case GamePass::SkySpecular:
        ray_tracing::noteSkySpecular(context);
        local_lights::noteDrawPoint(context);
        return;
    default: return;
    }
}

// The resolve while the global illumination runs: the diffuse light with the traced light in place
// of the game's probe light, white in place of the game's occlusion; or, for the diagnostics, the
// global illumination's output alone. False leaves the draw to the game.
bool drawResolveForGlobal(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, const Composition& composition, const GameShader& resolve,
    ID3D11PixelShader* gameShader)
{
    if (!ray_tracing::lightRuns()) return false;

    ComPtr<ID3D11ShaderResourceView> depth, diffuse, specular, gameOcclusion;
    context->PSGetShaderResources(ResolveDepthSlot, 1, depth.GetAddressOf());
    context->PSGetShaderResources(DiffuseSlot, 1, diffuse.GetAddressOf());
    context->PSGetShaderResources(SpecularLightSlot, 1, specular.GetAddressOf());

    const auto size = sizeOf(depth.Get());
    auto* light = ray_tracing::light(size.first, size.second);
    // Path tracing's reflections join the specular light.
    auto* specularWithReflections = path_tracing::addReflections(context, specular.Get(), size);
    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;
    if (!light_combine::combine(context, diffuse.Get(), light, size, diffuseWithLight)) return false;

    context->PSGetShaderResources(OcclusionSlot, 1, gameOcclusion.GetAddressOf());
    if (composition.showGlobalIllumination)
    {
        context->PSSetShader(showLightShader.Get(), nullptr, 0);
        context->PSSetShaderResources(DiffuseSlot, 1, &light);
    }
    else context->PSSetShaderResources(DiffuseSlot, 1, diffuseWithLight.view.GetAddressOf());

    context->PSSetShaderResources(OcclusionSlot, 1, whiteView.GetAddressOf());
    if (specularWithReflections) context->PSSetShaderResources(SpecularLightSlot, 1, &specularWithReflections);
    // With the traced reflections in the specular light, the rain's reflection of the sky goes: no specular
    // intensity (a null view reads 0), as the sun pass leaves its own out.
    const bool leavesRainReflection = specularWithReflections && resolve.hasRainReflection;
    ComPtr<ID3D11ShaderResourceView> rainSpecular;
    if (leavesRainReflection)
    {
        context->PSGetShaderResources(RainSpecularSlot, 1, rainSpecular.GetAddressOf());
        ID3D11ShaderResourceView* none{};
        context->PSSetShaderResources(RainSpecularSlot, 1, &none);
    }

    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    context->PSSetShader(gameShader, nullptr, 0);
    context->PSSetShaderResources(DiffuseSlot, 1, diffuse.GetAddressOf());
    context->PSSetShaderResources(SpecularLightSlot, 1, specular.GetAddressOf());
    context->PSSetShaderResources(OcclusionSlot, 1, gameOcclusion.GetAddressOf());
    if (leavesRainReflection) context->PSSetShaderResources(RainSpecularSlot, 1, rainSpecular.GetAddressOf());

    return true;
}

// A smaller camera's resolve while the global illumination runs, as the intro mirror's: the rays trace
// the main view only, so the game's own light, with white in place of its occlusion, which is skipped.
bool drawSmallerViewResolve(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call)
{
    if (!ray_tracing::lightRuns()) return false;

    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;

    ComPtr<ID3D11ShaderResourceView> gameOcclusion;
    context->PSGetShaderResources(OcclusionSlot, 1, gameOcclusion.GetAddressOf());
    context->PSSetShaderResources(OcclusionSlot, 1, whiteView.GetAddressOf());
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    context->PSSetShaderResources(OcclusionSlot, 1, gameOcclusion.GetAddressOf());

    return true;
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    const auto wanted = latch.latest();
    const bool enable = wanted.occlusion || wanted.light || wanted.globalIllumination;

    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    if (enable) ensurePipeline();
}

void detach() noexcept
{
    active = false;
    {
        std::lock_guard lock(resourceMutex);
        showOcclusionShader.Reset();
        showLightShader.Reset();
        whiteView.Reset();
        diffuseWithLight = {};
        device.Reset();
        pipelineFailed = false;
    }

    {
        std::lock_guard lock(gBufferMutex);
        normals.clear();
        lastNormals.Reset();
        lastSpecular.Reset();
        velocityTexture.Reset();
        velocityView.Reset();
        velocityDrawn = false;
    }
}

void configure(const Configuration& configuration) noexcept
{
    const auto& diagnostics = configuration.diagnostics;
    const Composition composition{
        .occlusion = configuration.effects.runs(Effect::AmbientOcclusion),
        .light = configuration.effects.runs(Effect::IndirectLight),
        .globalIllumination = configuration.effects.runs(Effect::GlobalIllumination),
        .rayTracing = configuration.effects.tracesScene(),
        .showOcclusion = diagnostics.showAmbientOcclusion,
        // SSGI's debug views draw its light alone too: the game's resolve would multiply it by the
        // albedo, and the gathered light is what is being judged.
        .showLight = diagnostics.showIndirectLight || configuration.settings.indirectLight.debug >= 1,
        .showGlobalIllumination = diagnostics.showGlobalIllumination || diagnostics.showTracedScene,
    };

    latch.set(composition);

    if (!composition.occlusion && !composition.light && !composition.globalIllumination) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

bool failed() noexcept
{
    return pipelineFailed;
}

ComPtr<ID3D11ShaderResourceView> specularTarget(UINT width, UINT height) noexcept
{
    try
    {
        return latestSpecular({width, height});
    }
    catch (...)
    {
        return nullptr;
    }
}

MaterialTables materialTables() noexcept
{
    std::lock_guard lock(gBufferMutex);
    return lastTables;
}

void note(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    // The velocity buffer is noted whatever the draw's shape, so the fullscreen-triangle filter
    // below does not apply to it.
    if (active.load(std::memory_order_relaxed) && !call.indexed)
    {
        const auto* row = shader.row();
        if (row && row->pass == GamePass::CameraVelocity)
        {
            noteVelocity(context);
            return;
        }
    }
    if (!active.load(std::memory_order_relaxed) || call.indexed || call.count != 3 || call.instances != 1) return;

    try
    {
        const auto* row = shader.row();
        if (!row || std::ranges::find(NotedPasses, row->pass) == std::end(NotedPasses)) return;

        if (call.indexed || call.count != 3 || call.instances != 1) return;

        const auto composition = latch.frame();

        // The rays trace the main view only, one kick a frame: a smaller camera's view in a corner of the
        // same targets, as the intro's mirror, keeps the game's light.
        const bool mainView = viewportFillsTarget(context);
        if (composition.rayTracing && mainView) noteForRayTracing(context, *row);

        switch (row->pass)
        {
        case GamePass::SkyLight:
            if (!composition.globalIllumination) noteNormals(context);
            return;
        case GamePass::IndirectUpscale:
            if (composition.globalIllumination && mainView && ray_tracing::lightRuns()) light_combine::drawProbeLightAside(context, call);
            return;
        case GamePass::Sun: noteTables(context); return;
        default: return;
        }
    }
    catch (const std::exception& error)
    {
        std::lock_guard lock(resourceMutex);
        logOnce(std::string("GTVBAO or SSGI: a pass kept the game's drawing: ") + error.what());
    }
    catch (...)
    {
    }
}

void targetsBound(UINT count, ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* depth) noexcept
{
    if (!active.load(std::memory_order_relaxed)) return;

    try
    {
        if (!isGBuffer(count, targets, depth)) return;

        auto* target = targets[SpecularTarget];
        const auto texture = resourceOf(target);
        {
            std::lock_guard lock(gBufferMutex);
            if (resourceOf(lastSpecular.Get()) == texture) return;
        }

        D3D11_RENDER_TARGET_VIEW_DESC targetDesc{};
        target->GetDesc(&targetDesc);
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
        viewDesc.Format = targetDesc.Format;
        viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipLevels = 1;
        ComPtr<ID3D11Device> targetDevice;
        target->GetDevice(targetDevice.GetAddressOf());
        ComPtr<ID3D11ShaderResourceView> view;
        check(targetDevice->CreateShaderResourceView(texture.Get(), &viewDesc, view.GetAddressOf()), "Viewing the G-buffer's specular target");

        std::lock_guard lock(gBufferMutex);
        lastSpecular = std::move(view);
    }
    catch (const std::exception& error)
    {
        std::lock_guard lock(resourceMutex);
        logOnce(std::string("GTVBAO, SSILVB or the rays: no specular target: ") + error.what());
    }
    catch (...)
    {
    }
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    if (!active.load(std::memory_order_relaxed) || call.indexed || call.count != 3 || call.instances != 1) return false;

    try
    {
        const auto* row = shader.row();
        if (!row || std::ranges::find(DrawnPasses, row->pass) == std::end(DrawnPasses)) return false;

        const auto composition = latch.frame();
        if (composition.globalIllumination)
            switch (row->pass)
            {
            case GamePass::Resolve:
                return viewportFillsTarget(context) ? drawResolveForGlobal(context, call, composition, *row, shader.object()) : drawSmallerViewResolve(context, call);
            case GamePass::GameOcclusion: return ray_tracing::lightRuns();
            default: return false;
            }

        switch (row->pass)
        {
        case GamePass::IndirectUpscale: return composition.occlusion && drawUpscale(context, call);
        case GamePass::Resolve: return drawResolve(context, call, composition, shader.object());
        case GamePass::GameOcclusion: return composition.occlusion;
        default: return false;
        }
    }
    catch (const std::exception& error)
    {
        std::lock_guard lock(resourceMutex);
        logOnce(std::string("GTVBAO or SSGI: a pass kept the game's drawing: ") + error.what());
    }
    catch (...)
    {
    }

    return false;
}

void present() noexcept
{
    const auto current = latch.present();
    active = (current.occlusion || current.light || current.globalIllumination || current.rayTracing) && !pipelineFailed;
    {
        std::lock_guard lock(gBufferMutex);
        velocityDrawn = false;
    }
}
}
