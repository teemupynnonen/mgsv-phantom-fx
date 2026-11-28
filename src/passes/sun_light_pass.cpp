#include "passes/sun_light_pass.hpp"

#include "constant_capture.hpp"
#include "features/clouds/clouds.hpp"
#include "d3d11_helpers.hpp"
#include "dev/game_shadow.hpp"
#include "dev_tools.hpp"
#include "features/upscaling/upscaler.hpp"
#include "features/sun_contact_shadows/sun_contact_shadows.hpp"
#include "frame_latch.hpp"
#include "log.hpp"
#include "passes/contact_shadow_trace.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "passes/sun_light_pass_shaders.hpp"
#include "performance.hpp"

#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace phantom_fx::sun_light_pass
{
using fxcore::FrameLatch;

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
    float soft, sunTangent, frame, screenSpace, rayTraced, ownCloudShadow, padding[2];
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
    // The add-on's sea hands its sparkles over in the translucency, which the game's shader would light as leaves':
    // the replacement lights them, everywhere, with none of the shadow effects as the game's own shadow.
    bool sea{};

    bool any() const { return softShadows || contactShadows || rayTracedShadows || isPathTracing || sea; }
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

#if PHANTOM_DEV_SHADERS
// A dev build's hot reload, as the sea's: present looks at the sun pass's .hlsl files twice a second or so and, when
// one was saved, compiles them all again. A failed compile is logged and the shaders in use stay. `sourcesSeen` is
// the newest file the shaders in use were compiled from.
std::filesystem::file_time_type sourcesSeen{};
std::uint64_t sourcesChecked{};

// The newest write among the sun pass's files; none when the folder could not be read through, as a partial scan
// would look like a save or hide one.
std::optional<std::filesystem::file_time_type> newestSource() noexcept
{
    std::filesystem::file_time_type newest{};
    std::error_code error;
    const auto directory = std::filesystem::path(PHANTOM_DEV_SHADER_ROOT) / "passes/shaders";
    for (std::filesystem::directory_iterator entry(directory, error), end; !error && entry != end; entry.increment(error))
    {
        if (!entry->path().filename().native().starts_with(L"sun_light")) continue;
        const auto written = entry->last_write_time(error);
        if (error) return std::nullopt;
        newest = std::max(newest, written);
    }
    if (error) return std::nullopt;

    return newest;
}
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

// Makes the shaders once, from their precompiled bytecode (compiled from source in a dev build); call with resourceMutex held. Returns false when they are unavailable.
bool ensurePipeline()
{
    if (optionsBuffer) return true;
    if (!device || pipelineFailed) return false;

#if PHANTOM_DEV_SHADERS
    // Before compiling: a file saved meanwhile is newer, and the next look reloads it.
    if (const auto newest = newestSource()) sourcesSeen = *newest;
#endif
    try
    {
        for (const bool isShadowMapped : {true, false})
            for (const bool reflectsSky : {true, false})
                replacements[replacementOf(isShadowMapped, reflectsSky)] = pixelShader(device.Get(), shaders::SunLight[isShadowMapped][reflectsSky], "sun-light");

        for (const bool isInShadowRange : {true, false})
            replacements[pathTracedOf(isInShadowRange)] = pixelShader(device.Get(), shaders::PathTraced[isInShadowRange], "sun-light-path-traced");

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

#if PHANTOM_DEV_SHADERS
// Call with resourceMutex held.
void reloadShadersIfSaved()
{
    if (++sourcesChecked % 30 != 1) return;

    const auto newest = newestSource();
    if (!newest || *newest == sourcesSeen) return;
    sourcesSeen = *newest;
    if (!optionsBuffer) return; // ensurePipeline compiles them from the files anyway

    try
    {
        std::array<ComPtr<ID3D11PixelShader>, 6> made;
        for (const bool isShadowMapped : {true, false})
            for (const bool reflectsSky : {true, false})
                made[replacementOf(isShadowMapped, reflectsSky)] = pixelShader(device.Get(), shaders::SunLight[isShadowMapped][reflectsSky], "sun-light");
        for (const bool isInShadowRange : {true, false})
            made[pathTracedOf(isInShadowRange)] = pixelShader(device.Get(), shaders::PathTraced[isInShadowRange], "sun-light-path-traced");
        replacements = std::move(made);
        log("Sun light pass shaders reloaded.");
    }
    catch (const std::exception& error)
    {
        log(std::string("Sun light pass shader reload failed, the previous shaders stay: ") + error.what());
    }
}
#endif

// Writes the replacement shaders' options on `context`; call with resourceMutex held. With the
// ray-traced shadow, the shadow map's part is always soft.
void writeOptions(ID3D11DeviceContext* context, const Composition& composition, bool hasContactShadow, bool rayTraced)
{
    constexpr float DegreesToRadians = 3.14159265f / 180;
    const ShaderOptions values{composition.softShadows || rayTraced ? 1.0f : 0.0f, std::tan(composition.sun.diameterDegrees * 0.5f * DegreesToRadians),
        turnPattern.load(std::memory_order_relaxed) ? static_cast<float>(frame.load(std::memory_order_relaxed) % PatternTurns) : 0.0f,
        hasContactShadow ? 1.0f : 0.0f, rayTraced ? 1.0f : 0.0f,
        // The rays trace the add-on's clouds' shadow (clouds::shadowTexture) into theirs: the game's cloud texture would shade twice.
        rayTraced && clouds::shadowTexture() ? 1.0f : 0.0f, {}};
    check(write(context, optionsBuffer.Get(), values), "Writing the sun shadow options");
}

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

    // Beyond the shadow range there is no shadow map to soften; the sea's sparkles reach past it.
    if (!composition.contactShadows && !(composition.softShadows && sun.isShadowMapped) && !rayTraced && !isPathTracedPass && !composition.sea)
        return false;

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
    if constexpr (DevTools)
    {
        if (game_shadow::isKept() && sun.isShadowMapped && depth && viewportFillsTarget(context))
        {
            const auto size = contact_shadow_trace::depthSize(depth.Get());
            if (SUCCEEDED(write(context, optionsBuffer.Get(), ShaderOptions{})))
                game_shadow::draw(device.Get(), context, call, static_cast<UINT>(size[0]), static_cast<UINT>(size[1]));

            context->PSSetShader(replacements[replacement].Get(), nullptr, 0);
            writeOptions(context, composition, contactShadow != nullptr, rayTraced != nullptr);
        }
    }

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
    if constexpr (DevTools) game_shadow::detach();

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
        .sea = configuration.effects.runs(Effect::Sea),
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
#if PHANTOM_DEV_SHADERS
    std::lock_guard lock(resourceMutex);
    reloadShadersIfSaved();
#endif
}
}
