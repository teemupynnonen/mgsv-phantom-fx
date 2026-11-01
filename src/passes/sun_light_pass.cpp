#include "passes/sun_light_pass.hpp"

#include "constant_capture.hpp"
#include "d3d11_helpers.hpp"
#include "dev_tools.hpp"
#include "features/upscaling/upscaler.hpp"
#include "features/sun_contact_shadows/sun_contact_shadows.hpp"
#include "frame_latch.hpp"
#include "log.hpp"
#include "passes/contact_shadow_trace.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "passes/sun_light_pass_shaders.hpp"
#include "performance.hpp"
#include "resource_dump.hpp"

#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>

namespace phantom_fx::sun_light_pass
{
namespace
{
using Microsoft::WRL::ComPtr;
using game_shaders::GamePass, game_shaders::GameShader;

constexpr UINT DepthSlot = 2;             // t2 of the light passes
constexpr UINT RayTracedSlot = 6;         // t6 of the replacement shaders
constexpr UINT ContactShadowSlot = 7;     // t7 of the replacement shaders
constexpr UINT OptionsSlot = 6;           // b6 of the replacement shaders
constexpr std::uint32_t PatternTurns = 64; // Frames before the soft shadows' samples repeat

// cSunShadows (b6) of the replacement shaders.
struct ShaderOptions
{
    float soft, sunTangent, frame, screenSpace, rayTraced, padding[3];
};
static_assert(sizeof(ShaderOptions) % 16 == 0);

// The sun shadow effects the pass adds, by the effect plan, with their settings.
struct Composition
{
    bool softShadows{}, contactShadows{}, rayTracedShadows{};
    // Path tracing lights the sun with the rays' shadow, and traces the reflections in place of the sky's
    // (path_tracing.hpp), while it runs.
    bool isPathTracing{};
    Sun sun;
    RayTracedShadows rayTraced;
    bool onlyContactShadows{}; // Diagnostics: the sun is shadowed by its contact shadows alone

    bool any() const { return softShadows || contactShadows || rayTracedShadows || isPathTracing; }
};

FrameLatch<Composition> latch;
std::atomic<bool> active{}, pipelineFailed{};
// Counts presents, to turn the soft shadows' samples while DLSS or FSR runs: they accumulate frames.
std::atomic<std::uint32_t> frame{};
std::atomic<bool> turnPattern{};

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
// The game's sun shaders rebuilt: with and without the shadow map, each with and without the sky's
// reflection (game_shaders::GameShader); then path tracing's, inside the shadow range and beyond it
// (pathTracedOf).
std::array<ComPtr<ID3D11PixelShader>, 6> replacements;
ComPtr<ID3D11Buffer> optionsBuffer;

#if PHANTOM_FX_DEV_TOOLS
// Diagnostics (keepGameShadow): the shader that draws the shadow map's answer, its target, and the depth
// state it draws with, made from `gameShadowDepthOf`, the sun pass's.
std::atomic<bool> keepsGameShadow{};
ComPtr<ID3D11PixelShader> gameShadowShader;
ComPtr<ID3D11RenderTargetView> gameShadowTarget;
ComPtr<ID3D11DepthStencilState> gameShadowDepth, gameShadowDepthOf;
// The answer. Under its own lock, held across no Direct3D call: the present saves it, and it holding
// Direct3D 11's lock while waiting for resourceMutex, held by a sun pass waiting for Direct3D 11's, froze the
// game (2026-10-30, both threads' stacks read from the hung process).
std::mutex gameShadowMutex;
ComPtr<ID3D11Texture2D> gameShadow;
#endif

std::size_t replacementOf(bool isShadowMapped, bool reflectsSky)
{
    return (isShadowMapped ? 2 : 0) + (reflectsSky ? 1 : 0);
}

// Path tracing's: the game's pass inside the shadow range has the clouds' shadow, beyond it none.
std::size_t pathTracedOf(bool isShadowMapped)
{
    return 4 + (isShadowMapped ? 1 : 0);
}

// Compiles the shaders once; call with resourceMutex held. Returns false when they are unavailable.
bool ensurePipeline()
{
    if (optionsBuffer) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        for (const bool isShadowMapped : {true, false})
            for (const bool reflectsSky : {true, false})
            {
                const D3D_SHADER_MACRO defines[]{{"SHADOW_MAP", isShadowMapped ? "1" : "0"}, {"SKY_REFLECTION", reflectsSky ? "1" : "0"}, {}};
                replacements[replacementOf(isShadowMapped, reflectsSky)] = pixelShader(device.Get(), shaders::SunLight, "sun-light", defines);
            }

        for (const bool isInShadowRange : {true, false})
        {
            const D3D_SHADER_MACRO defines[]{{"SHADOW_MAP", "0"}, {"SKY_REFLECTION", "0"}, {"PATH_TRACED", "1"}, {"CLOUD_SHADOW", isInShadowRange ? "1" : "0"}, {}};
            replacements[pathTracedOf(isInShadowRange)] = pixelShader(device.Get(), shaders::SunLight, "sun-light-path-traced", defines);
        }

        // Last, so it marks a complete pipeline.
        optionsBuffer = makeConstantBuffer(device.Get(), sizeof(ShaderOptions), "Creating the sun shadow options");
        log("Sun light pass shaders ready.");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        for (auto& replacement : replacements) replacement.Reset();
        log(std::string("Sun shadows unavailable: ") + error.what());

        return false;
    }
}

// Writes the replacement shaders' options on `context`; call with resourceMutex held. With the
// ray-traced shadow, the shadow map's part is always soft.
void writeOptions(ID3D11DeviceContext* context, const Composition& composition, bool hasContactShadow, bool rayTraced)
{
    constexpr float DegreesToRadians = 3.14159265f / 180;
    const ShaderOptions values{composition.softShadows || rayTraced ? 1.0f : 0.0f, std::tan(composition.sun.diameterDegrees * 0.5f * DegreesToRadians),
        turnPattern.load(std::memory_order_relaxed) ? static_cast<float>(frame.load(std::memory_order_relaxed) % PatternTurns) : 0.0f,
        hasContactShadow ? 1.0f : 0.0f, rayTraced ? 1.0f : 0.0f, {}};
    check(write(context, optionsBuffer.Get(), values), "Writing the sun shadow options");
}

#if PHANTOM_FX_DEV_TOOLS
// Diagnostics: what the shadow map alone says, with the sun pass's state and inputs as bound, so pixels
// the pass skips stay at -1. Before the pass's own draw and leaving the stencil as it is: the pass zeroes the
// bit it tests as it draws (read 2026-10-30: pass op zero, write mask 8), so a draw after it found nothing.
// Call with resourceMutex held.
void drawGameShadow(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, UINT width, UINT height)
{
    if (!gameShadowShader)
    {
        const D3D_SHADER_MACRO defines[]{{"SHADOW_MAP", "1"}, {"SKY_REFLECTION", "0"}, {"GAME_SHADOW", "1"}, {}};
        gameShadowShader = pixelShader(device.Get(), shaders::SunLight, "sun-light-game-shadow", defines);
    }

    ComPtr<ID3D11Texture2D> texture;
    {
        std::lock_guard lock(gameShadowMutex);
        texture = gameShadow;
    }

    D3D11_TEXTURE2D_DESC desc{};
    if (texture) texture->GetDesc(&desc);
    if (desc.Width != width || desc.Height != height)
    {
        desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        texture.Reset();
        gameShadowTarget.Reset();
        check(device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()), "Creating the game's shadow copy");
        check(device->CreateRenderTargetView(texture.Get(), nullptr, gameShadowTarget.GetAddressOf()), "Creating the game's shadow target");
        std::lock_guard lock(gameShadowMutex);
        gameShadow = texture;
    }

    ComPtr<ID3D11RenderTargetView> targets[2];
    ComPtr<ID3D11DepthStencilView> depthStencil;
    context->OMGetRenderTargets(2, targets[0].GetAddressOf(), depthStencil.GetAddressOf());
    ComPtr<ID3D11BlendState> blend;
    FLOAT blendFactor[4]{};
    UINT sampleMask{};
    context->OMGetBlendState(blend.GetAddressOf(), blendFactor, &sampleMask);
    ComPtr<ID3D11DepthStencilState> depthState;
    UINT stencilReference{};
    context->OMGetDepthStencilState(depthState.GetAddressOf(), &stencilReference);
    if (!gameShadowDepth || gameShadowDepthOf != depthState)
    {
        D3D11_DEPTH_STENCIL_DESC depthDesc{};
        if (depthState) depthState->GetDesc(&depthDesc);
        depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depthDesc.StencilWriteMask = 0;
        gameShadowDepth.Reset();
        check(device->CreateDepthStencilState(&depthDesc, gameShadowDepth.GetAddressOf()), "Creating the game's shadow depth state");
        gameShadowDepthOf = depthState;
    }

    // The last that can fail: from here the game's targets are unbound.
    check(write(context, optionsBuffer.Get(), ShaderOptions{}), "Writing the sun shadow options");

    constexpr FLOAT NoAnswer[4]{-1, -1, -1, -1};
    context->ClearRenderTargetView(gameShadowTarget.Get(), NoAnswer);
    context->OMSetRenderTargets(1, gameShadowTarget.GetAddressOf(), depthStencil.Get());
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(gameShadowDepth.Get(), stencilReference);
    context->PSSetShader(gameShadowShader.Get(), nullptr, 0);
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);

    ID3D11RenderTargetView* const restored[]{targets[0].Get(), targets[1].Get()};
    context->OMSetRenderTargets(2, restored, depthStencil.Get());
    context->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
    context->OMSetDepthStencilState(depthState.Get(), stencilReference);
}
#endif

// Path tracing lights this sun pass and traces its reflections: the main view's, while the rays run.
bool isPathTraced(ID3D11DeviceContext* context)
{
    return ray_tracing::isPathTracing() && viewportFillsTarget(context);
}

// The sun pass, drawn with the replacement shaders.
bool drawSun(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, const GameShader& sun, ID3D11PixelShader* gameShader,
    const Composition& composition)
{
    ComPtr<ID3D11ShaderResourceView> depth;
    context->PSGetShaderResources(DepthSlot, 1, depth.GetAddressOf());

    // The ray-traced shadow at the depth's size, once Direct3D 12 has traced one; until then the
    // game's shadows stay. The rays trace the main view only, not a smaller camera's (a mirror's).
    // Path tracing shades the sun with the rays' shadow alone (path_tracing.hpp).
    const bool isPathTracedPass = isPathTraced(context);
    ID3D11ShaderResourceView* rayTraced{};
    if ((composition.rayTracedShadows || isPathTracedPass) && depth && viewportFillsTarget(context))
    {
        const auto size = contact_shadow_trace::depthSize(depth.Get());
        rayTraced = ray_tracing::sunShadow(static_cast<UINT>(size[0]), static_cast<UINT>(size[1]));
    }

    // Beyond the shadow range there is no shadow map to soften.
    if (!composition.contactShadows && !(composition.softShadows && sun.isShadowMapped) && !rayTraced && !isPathTracedPass) return false;

    const auto scene = constant_capture::pixelShaderReads<game_constants::SceneConstants>(context);
    const auto light = constant_capture::pixelShaderReads<game_constants::SunLightConstants>(context);

    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;

    bool isShadowMapped = sun.isShadowMapped;
    // Path tracing's sun has the rays' shadow alone: no contact shadow to trace, nor to fail on and leave the
    // game's sky reflection to add to the traced reflections.
    ID3D11ShaderResourceView* contactShadow{};
    if (composition.contactShadows && !isPathTracedPass)
    {
        if (!depth || !scene || !light)
        {
            logOnce("Shadows: a sun pass without its depth or constants keeps the game's shadows.");
            return false;
        }

        contactShadow = sun_contact_shadows::trace(context, depth.Get(), *scene, *light);
        if (!contactShadow) return false;

        // The diagnostics view lights everything as beyond the shadow range, with the trace's shadow.
        if (composition.onlyContactShadows) isShadowMapped = false;
    }

    // The rays' shadow alone, unless the shadow map's joins it: everything lit as beyond the shadow range.
    if (rayTraced && !composition.rayTraced.withGameShadows) isShadowMapped = false;

    writeOptions(context, composition, contactShadow != nullptr, rayTraced != nullptr);
    ComPtr<ID3D11ShaderResourceView> gameView, gameRayTracedView;
    ComPtr<ID3D11Buffer> gameOptions;
    context->PSGetShaderResources(ContactShadowSlot, 1, gameView.GetAddressOf());
    context->PSGetShaderResources(RayTracedSlot, 1, gameRayTracedView.GetAddressOf());
    context->PSGetConstantBuffers(OptionsSlot, 1, gameOptions.GetAddressOf());
    // Path tracing reflects the sky itself (ray_tracing::reflection): its pass leaves the sky's reflection out,
    // and while the rays have no shadow yet the game's pass draws without it.
    const auto replacement = isPathTracedPass && rayTraced ? pathTracedOf(sun.isShadowMapped) : replacementOf(isShadowMapped, sun.reflectsSky && !isPathTracedPass);
    context->PSSetShader(replacements[replacement].Get(), nullptr, 0);
    context->PSSetShaderResources(ContactShadowSlot, 1, &contactShadow);
    context->PSSetShaderResources(RayTracedSlot, 1, &rayTraced);
    context->PSSetConstantBuffers(OptionsSlot, 1, optionsBuffer.GetAddressOf());
#if PHANTOM_FX_DEV_TOOLS
    if (keepsGameShadow.load(std::memory_order_relaxed) && sun.isShadowMapped && depth && viewportFillsTarget(context))
    {
        const auto size = contact_shadow_trace::depthSize(depth.Get());
        try
        {
            drawGameShadow(context, call, static_cast<UINT>(size[0]), static_cast<UINT>(size[1]));
        }
        catch (const std::exception& error)
        {
            // Only a debug case's reference: the sun pass still draws, bound as it is.
            keepsGameShadow = false;
            logOnce(std::string("Debug case: the game's shadow could not be kept: ") + error.what());
        }

        context->PSSetShader(replacements[replacement].Get(), nullptr, 0);
        writeOptions(context, composition, contactShadow != nullptr, rayTraced != nullptr);
    }
#endif

    performance::Timed timed(context, performance::Pass::SunLight);
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    timed.end();
    context->PSSetShader(gameShader, nullptr, 0);
    context->PSSetShaderResources(ContactShadowSlot, 1, gameView.GetAddressOf());
    context->PSSetShaderResources(RayTracedSlot, 1, gameRayTracedView.GetAddressOf());
    context->PSSetConstantBuffers(OptionsSlot, 1, gameOptions.GetAddressOf());
    if (composition.softShadows) logOnce("Shadows: the sun pass draws with soft shadows.");
    if (rayTraced) logOnce("Shadows: the sun pass draws with ray-traced shadows.");

    return true;
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    const bool wantedOn = latch.latest().any();

    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    if (wantedOn) ensurePipeline();
}

void detach() noexcept
{
    std::lock_guard lock(resourceMutex);
    for (auto& replacement : replacements) replacement.Reset();
    optionsBuffer.Reset();
#if PHANTOM_FX_DEV_TOOLS
    gameShadowShader.Reset();
    gameShadowTarget.Reset();
    gameShadowDepth.Reset();
    gameShadowDepthOf.Reset();
    {
        std::lock_guard shadowLock(gameShadowMutex);
        gameShadow.Reset();
    }
#endif

    device.Reset();
    pipelineFailed = false;
}

void configure(const Configuration& configuration) noexcept
{
    const Composition composition{
        .softShadows = configuration.effects.runs(Effect::SoftShadows),
        .contactShadows = configuration.effects.runs(Effect::SunContactShadows),
        .rayTracedShadows = configuration.effects.runs(Effect::RayTracedShadows),
        .isPathTracing = configuration.effects.runs(Effect::PathTracing),
        .sun = configuration.settings.sun,
        .rayTraced = configuration.settings.rayTracedShadows,
        .onlyContactShadows = configuration.diagnostics.onlyScreenSpaceShadows,
    };

    latch.set(composition);

    if (!composition.any()) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

bool failed() noexcept
{
    return pipelineFailed;
}

#if PHANTOM_FX_DEV_TOOLS
void keepGameShadow(bool isKept) noexcept
{
    if (isKept)
    {
        // Only this capture's sun passes may give the reference: indoors there are none.
        std::lock_guard lock(gameShadowMutex);
        gameShadow.Reset();
    }

    keepsGameShadow = isKept;
}

void saveGameShadow(ID3D11DeviceContext* context, const std::filesystem::path& file) noexcept
{
    ComPtr<ID3D11Texture2D> texture;
    {
        std::lock_guard lock(gameShadowMutex);
        texture = gameShadow;
    }

    if (texture) saveTexture(context, texture.Get(), DXGI_FORMAT_UNKNOWN, file);
}
#endif

Health softShadowHealth()
{
    if (pipelineFailed) return Health::unavailable("Soft shadows are unavailable (see PhantomFX.log).");

    return {};
}

Health rayTracedShadowHealth()
{
    if (pipelineFailed) return Health::unavailable("Ray-traced shadows are unavailable (see PhantomFX.log).");

    return ray_tracing::health();
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    if (!active.load(std::memory_order_relaxed) || call.indexed || call.instances != 1 || call.count != 3) return false;

    try
    {
        const auto* sun = shader.row();
        if (!sun || (sun->pass != GamePass::Sun && sun->pass != GamePass::SkySpecular)) return false;

        const auto composition = latch.frame();
        // Indoors, the sky's specular alone: path tracing's reflections take its place.
        if (sun->pass == GamePass::SkySpecular) return isPathTraced(context);

        return drawSun(context, call, *sun, shader.object(), composition);
    }
    catch (const std::exception& error)
    {
        std::lock_guard lock(resourceMutex);
        logOnce(std::string("Shadows: a sun pass kept the game's shadows: ") + error.what());
    }
    catch (...)
    {
    }

    return false;
}

void present() noexcept
{
    frame.fetch_add(1, std::memory_order_relaxed);
    turnPattern = upscaler::replaces();
    const auto current = latch.present();
    active = current.any() && !pipelineFailed;
    contact_shadow_trace::tracesThisFrame(contact_shadow_trace::Tracer::Sun, active && current.contactShadows);
}
}
