// Phantom FX's effects as fxui budget lines (menu_budget.hpp).
#include "menu_budget.hpp"

#include "core.hpp"
#include "dev_tools.hpp"
#include "features/ambient_occlusion/gtvbao.hpp"
#include "features/frame_generation/presenter.hpp"
#include "features/global_illumination/light_combine.hpp"
#include "features/hdr/hdr.hpp"
#include "features/indirect_light/ssilvb.hpp"
#include "features/lamp_contact_shadows/lamp_contact_shadows.hpp"
#include "features/lamp_soft_shadows/lamp_soft_shadows.hpp"
#include "features/path_tracing/path_tracing.hpp"
#include "features/ray_traced_lights/local_lights.hpp"
#include "features/sea/sea.hpp"
#include "features/water_caustics/water_caustics.hpp"
#include "features/sun_contact_shadows/sun_contact_shadows.hpp"
#include "features/upscaling/upscaler.hpp"
#include "features/volumetric_fog/atmosphere.hpp"
#include "passes/sun_light_pass.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <initializer_list>
#include <iterator>
#include <span>
#include <string>
#include <utility>

namespace phantom_fx::menu
{
namespace
{
using fxui::State;
using performance::Pass;

constexpr std::array<fxui::Section, 8> all{{
    {"Lighting", fxui::hue::Blue, false, "Graphics"},
    {"Image", fxui::hue::Teal, false, "Graphics"},
    {"World", fxui::hue::Violet, false, "Graphics"},
    {"Camera", fxui::hue::Pink, false, "Graphics"},
    {"Sandbox"},
    {"Budget", 0, true},
    {"Diagnostics"},
    {"Developer"},
}};

struct Timed
{
    Pass pass;
    std::string_view name;
};

// A lighting effect's budget line, beside what its feature says (features/): its timed passes, and
// how it is doing while the plan runs it.
struct EffectLine
{
    Effect effect;
    std::initializer_list<Timed> passes;
    Health (*health)();
};

// In the budget's order. Not constexpr: MSVC leaves a constexpr table's initializer lists empty at run
// time.
const EffectLine EffectLines[]{
    {.effect = Effect::RayTracedShadows, .passes = {{Pass::SunShadowRays, "Shadow rays"}, {Pass::SunShadowDenoiser, "Denoiser"}},
        .health = sun_light_pass::rayTracedShadowHealth},
    {.effect = Effect::RayTracedLights, .passes = {{Pass::LightShadowRays, "Shadow rays"}, {Pass::LightShadowFilter, "Shadow filter"}, {Pass::LocalLights, "Lights with their traced shadows"}},
        .health = local_lights::health},
    {.effect = Effect::SoftShadows, .health = sun_light_pass::softShadowHealth},
    {.effect = Effect::LampSoftShadows, .passes = {{Pass::LampSoftLight, "Lamps: light draws, with the soft shadow"}}, .health = lamp_soft_shadows::health},
    {.effect = Effect::SunContactShadows, .passes = {{Pass::SunTrace, "Sun: contact shadow trace"}}, .health = sun_contact_shadows::health},
    {.effect = Effect::LampContactShadows, .passes = {{Pass::LampShadows, "Lamps: shadow draws"}}, .health = lamp_contact_shadows::health},
    {.effect = Effect::GlobalIllumination, .passes = {{Pass::GiRays, "Rays and the light cache"}, {Pass::GiDenoiser, "Denoiser and render size"},
            {Pass::GiProbeLight, "The game's probe light, copied"}, {Pass::GiCombine, "Adding the light"}},
        .health = light_combine::health},
    {.effect = Effect::PathTracing, .passes = {{Pass::Reflections, "Reflections"}, {Pass::LampLight, "Point and spot lights"},
            {Pass::PathTracedLight, "Adding the light"}},
        .health = path_tracing::health},
    {.effect = Effect::AmbientOcclusion, .passes = {{Pass::OcclusionPrefilter, "Depth prefilter"}, {Pass::OcclusionMarch, "March"}, {Pass::OcclusionDenoise, "Denoise"},
            {Pass::OcclusionApply, "Applying to the indirect light"}},
        .health = gtvbao::health},
    {.effect = Effect::IndirectLight, .passes = {{Pass::LightPrefilter, "Depth prefilter"}, {Pass::LightMarch, "March (half size)"}, {Pass::LightDenoise, "Denoise"},
            {Pass::LightUpsample, "Upscaling to the render size"}},
        .health = ssilvb::health},
    {.effect = Effect::VolumetricFog, .passes = {{Pass::FogPass, "The game's fog pass, larger"}, {Pass::SkyTables, "Sky tables (physical look)"}, {Pass::FogShadow, "Sun shadow in the fog"},
            {Pass::FogScattering, "Scattering"}, {Pass::FogIntegration, "Integration"}, {Pass::Sky, "Physical sky"}},
        .health = atmosphere::health},
    {.effect = Effect::Sea, .passes = {{Pass::SeaWaves, "Waves: spectrum, FFT and maps"}, {Pass::SeaSurface, "Surface into the G-buffer"}},
        .health = sea::health},
    {.effect = Effect::WaterCaustics, .passes = {{Pass::WaterCaustics, "Webs over the rivers"}}, .health = water_caustics::health},
};
static_assert(std::size(EffectLines) == static_cast<std::size_t>(Effect::Count));

std::string_view lineOf(Effect effect)
{
    return featureOf(effect).menuId;
}

// What a Lacking effect needs, in its features' words.
std::string needsOf(Effect effect)
{
    const auto& feature = featureOf(effect);
    std::string needs = feature.needs ? featureOf(*feature.needs).name : "";
    if (feature.needsRayReconstruction) needs += std::string(needs.empty() ? "" : " and ") + "DLSS Ray Reconstruction";

    return "Needs " + needs + " on.";
}

// A lighting effect's line state, and its message.
struct Report
{
    State state;
    std::string message;
};

// From the plan, then, while it runs, from its health.
Report reportOf(Effect effect, EffectState planned, Health health)
{
    if (planned == EffectState::Off) return {State::Off};
    if (planned == EffectState::Unsupported) return {State::Unavailable, "This game version is not supported, so it stays off."};
    if (planned == EffectState::Held) return {State::Held};
    if (planned == EffectState::Replaced) return {State::Replaced};
    if (planned == EffectState::Lacking) return {State::NotRunning, needsOf(effect)};
    if (health.kind == Health::Kind::Unavailable) return {State::Unavailable, std::move(health.reason)};
    if (health.kind == Health::Kind::Waiting) return {State::NotRunning, std::move(health.reason)};

    return {State::Working};
}

using LineState = std::pair<std::string_view, State>; // A line's id and state

// A pass several effects share counts under the first of them that works, else the first that is on.
std::string_view owner(std::span<const LineState> effects)
{
    for (const auto& [id, state] : effects)
        if (state == State::Working) return id;
    for (const auto& [id, state] : effects)
        if (state != State::Off) return id;

    return effects.begin()->first;
}

// What the upscaler is doing right now, for its line.
std::string upscalerMessage(const upscaler::Status& state, const Upscaling& wanted)
{
    const auto& name = state.upscalerName;
    const auto nativeMode = upscaler::label(upscaler::Mode::Dlaa, wanted.upscaler);
    std::string message;
    if (!state.supportedBuild) message = "This game version is not supported, so upscaling stays off.";
    else if (wanted.mode == upscaler::Mode::Off) message = "Upscaling is off. The game's FXAA is in use.";
    else if (state.failed) message = std::format("{} stopped: {} Choose a mode or upscaler again to retry.", name, state.failure);
    else if (upscaler::upscales(wanted.mode) && !state.upscalingAvailable) message = std::format("Upscaling is unavailable (see PhantomFX.log); {} still works.", nativeMode);
    else if (core::currentDiagnostics().gameFxaa) message = "Showing the game's FXAA for comparison (Diagnostics).";
    else if (!state.fxaaRecognised) message = "Waiting for the game to draw its anti-aliasing.";
    else if (!state.running && state.lastFallback.empty()) message = std::format("{} is waiting for the game's 3D view.", name);
    else if (!state.running) message = std::format("{} is waiting: {}.", name, state.lastFallback);
    else if (state.renderWidth == state.outputWidth && state.renderHeight == state.outputHeight)
        message = std::format("{} {} at {}\xC3\x97{}.", name, nativeMode, state.outputWidth, state.outputHeight);
    else
        message = std::format("{} {}: {}\xC3\x97{} upscaled to {}\xC3\x97{}.", name, upscaler::label(wanted.mode, wanted.upscaler), state.renderWidth, state.renderHeight,
            state.outputWidth, state.outputHeight);

    if (wanted.upscaler != upscaler::Upscaler::Dlss || !wanted.rayReconstruction || !state.running || state.rayReconstruction) return message;

    return message + (state.reconstructionAvailable ? " Ray Reconstruction waits for a ray-traced effect; DLSS runs meanwhile."
                                                     : " Ray Reconstruction could not run here (see PhantomFX.log); DLSS runs instead.");
}

int at(Section section)
{
    return static_cast<int>(section);
}
}

std::span<const fxui::Section> sections()
{
    return std::span(all).first(DevTools ? all.size() : all.size() - 1);
}

fxui::Budget budget(Settings& wanted, const performance::Costs& costs)
{
    const auto status = upscaler::status();
    const State upscaling = wanted.upscaling.mode == upscaler::Mode::Off ? State::Off
        : !status.supportedBuild || status.failed                ? State::Unavailable
        : status.running                                          ? State::Working
                                                                  : State::NotRunning;

    const auto plan = core::effectPlan(wanted);
    ByEffect<Report> reports;
    for (const auto& line : EffectLines) reports[line.effect] = reportOf(line.effect, plan[line.effect], line.health());

    const auto lineState = [&](Effect effect) { return LineState{lineOf(effect), reports[effect].state}; };

    State output = State::Working;
    std::string outputMessage;
    if (wanted.hdr.on != hdr::active())
    {
        output = State::Restart;
        outputMessage = wanted.hdr.on ? "Restart the game to turn HDR on." : "Restart the game to turn HDR off.";
    }
    else if (!wanted.hdr.on) output = State::Off;
    else if (hdr::output() == hdr::Output::Unavailable)
    {
        output = State::Unavailable;
        outputMessage = "HDR output is unavailable (see PhantomFX.log).";
    }
    else if (hdr::output() == hdr::Output::Sdr)
    {
        output = State::NotRunning;
        outputMessage = "HDR is off for this display in Windows, so the game shows SDR.";
    }

    const auto timed = [&](Pass pass) { return costs.passes[static_cast<std::size_t>(pass)]; };

    // The replacement sun light pass runs for path tracing's sun and every sun shadow.
    const LineState sunShadows[]{lineState(Effect::PathTracing), lineState(Effect::RayTracedShadows), lineState(Effect::SoftShadows),
        lineState(Effect::SunContactShadows)};
    const auto sunLight = owner(sunShadows);

    // Each ray-traced effect has its own rays on Direct3D 12. What they share, the scene and handing the
    // frame over to Direct3D 12 and back, splits evenly between those working. Direct3D 11 waits while
    // Direct3D 12 works, so its inputs and wait, less all the Direct3D 12 passes, are the hand-over.
    std::array<LineState, RayTracedEffects.size()> tracers;
    std::ranges::transform(RayTracedEffects, tracers.begin(), lineState);

    float traced{};
    for (const auto pass : {Pass::RayTracedScene, Pass::SunShadowRays, Pass::SunShadowDenoiser, Pass::LightShadowRays, Pass::LightShadowFilter, Pass::GiRays,
             Pass::GiDenoiser, Pass::Reflections, Pass::LampLight})
        traced += timed(pass);

    const float handOver = std::max(0.0f, timed(Pass::GiInputs) + timed(Pass::GiWait) - traced);
    const auto working = std::ranges::count(tracers, State::Working, &LineState::second);
    const auto rays = owner(tracers);
    // How much of what the ray tracing shares a line carries: an even share while any of them works,
    // else all of it on the first that is on.
    const auto shareOf = [&](std::string_view id, State state) {
        if (std::ranges::find(tracers, id, &LineState::first) == std::ranges::end(tracers)) return 0.0f;
        if (working) return state == State::Working ? 1.0f / static_cast<float>(working) : 0.0f;

        return id == rays ? 1.0f : 0.0f;
    };

    fxui::Budget result;
    result.gpuMs = costs.gpuFrame;
    result.frameMs = costs.frame;
    result.presented = presenter::presentedPerFrame();
    const auto add = [&](fxui::Line line, std::initializer_list<Timed> passes) {
        const auto addPass = [&](std::string_view name, float ms) {
            line.passes.push_back({name, ms});
            line.ms += ms;
        };
        for (const auto& [pass, name] : passes) addPass(name, timed(pass));
        if (line.id == sunLight) addPass("Sun light pass (with the game's lighting)", timed(Pass::SunLight));
        if (const float share = shareOf(line.id, line.state); share > 0)
        {
            const bool split = share < 1;
            addPass(split ? "Its share of the ray-traced scene" : "The ray-traced scene", timed(Pass::RayTracedScene) * share);
            addPass(split ? "Its share of the hand-over to Direct3D 12" : "Hand-over to Direct3D 12 (G-buffer pack, waits)", handOver * share);
        }
        // Effects that share a switch share a line (FeatureInfo::menuId): their passes add up, and it says
        // what the one that does most says: one that works, else one that is on.
        const auto shared = std::ranges::find(result.lines, line.id, &fxui::Line::id);
        if (shared == result.lines.end())
        {
            result.lines.push_back(std::move(line));
            return;
        }

        shared->ms += line.ms;
        std::ranges::move(line.passes, std::back_inserter(shared->passes));
        const bool doesMore = line.state == State::Working ? shared->state != State::Working : line.state != State::Off && shared->state == State::Off;
        if (!doesMore) return;

        shared->state = line.state;
        shared->replacedBy = line.replacedBy;
        shared->message = std::move(line.message);
    };

    const auto upscalerFeature = infoOf<Upscaling>();
    add({.id = upscalerFeature.menuId, .name = upscalerFeature.name, .tech = upscaler::label(wanted.upscaling.upscaler), .section = at(upscalerFeature.section),
            .state = upscaling, .message = upscalerMessage(status, wanted.upscaling)},
        {{Pass::UpscalerInputs, "Motion vectors and inputs"}, {Pass::Upscaler, "DLSS or FSR"}, {Pass::UpscalerOutput, "Sharpening and output"}});

    for (const auto& line : EffectLines)
    {
        const auto& feature = featureOf(line.effect);
        const auto& [state, message] = reports[line.effect];
        add({.id = feature.menuId, .name = feature.name, .tech = feature.tech ? feature.tech : "", .section = at(feature.section), .state = state,
                .toggle = &switchOf(wanted, line.effect), .replacedBy = plan.replacers[line.effect] ? lineOf(*plan.replacers[line.effect]) : std::string_view{}, .message = message},
            line.passes);
    }

    const auto hdrFeature = infoOf<Hdr>();
    add({.id = hdrFeature.menuId, .name = hdrFeature.name, .tech = hdrFeature.tech, .section = at(hdrFeature.section), .state = output,
            .toggle = &wanted.hdr.on, .message = outputMessage},
        {{Pass::HdrEncode, "Encoding the scene"}, {Pass::HdrDecode, "Decoding copies"}, {Pass::HdrGrade, "Grading"}});

    return result;
}
}
