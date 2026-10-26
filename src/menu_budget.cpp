// Phantom FX's effects as fxui budget lines (menu_budget.hpp).
#include "menu_budget.hpp"

#include "atmosphere.hpp"
#include "dev_tools.hpp"
#include "global_illumination.hpp"
#include "hdr.hpp"
#include "indirect_light.hpp"
#include "local_lights.hpp"
#include "presenter.hpp"
#include "shadows.hpp"
#include "upscaler.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>

namespace phantom_fx::menu
{
namespace
{
using fxui::State;
using performance::Pass;

constexpr std::array<fxui::Section, 10> all{{
    {"Budget", 0, true},
    {"Upscaling", fxui::hue::Teal},
    {"Shadows", fxui::hue::Straw},
    {"Lighting", fxui::hue::Blue},
    {"Atmosphere", fxui::hue::Violet},
    {"Camera", fxui::hue::Pink},
    {"Display", fxui::hue::Sand},
    {"World"},
    {"Diagnostics", 0, false, true},
    {"Developer"},
}};

struct Timed
{
    Pass pass;
    std::string_view name;
};

// An effect's state from its setting, the F1 switch, what replaces it, and its module.
State stateOf(bool on, bool held, bool replaced, bool failed, std::string_view problem)
{
    if (!on) return State::Off;
    if (held) return State::Held;
    if (replaced) return State::Replaced;
    if (failed) return State::Unavailable;
    if (!problem.empty()) return State::NotRunning;

    return State::Working;
}

std::string messageFor(State state, std::string_view problem, std::string_view unavailable = {})
{
    if (state == State::NotRunning) return std::string(problem);
    if (state == State::Unavailable) return std::string(unavailable);

    return {};
}

using Effect = std::pair<std::string_view, State>; // A line's id and state

// A pass several effects share counts under the first of them that works, else the first that is on.
std::string_view owner(std::span<const Effect> effects)
{
    for (const auto& [id, state] : effects)
        if (state == State::Working) return id;
    for (const auto& [id, state] : effects)
        if (state != State::Off) return id;

    return effects.begin()->first;
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

fxui::Budget budget(upscaler::Settings& wanted, const performance::Costs& costs)
{
    const bool held = !upscaler::enhancementsOn();
    const bool shadowsFailed = shadows::failed();
    const auto rayTracing = global_illumination::problem();
    const auto lightsProblem = local_lights::problem();
    const auto occlusionProblem = indirect_light::occlusionProblem();
    const auto lightProblem = indirect_light::lightProblem();
    const auto fogProblem = atmosphere::problem();

    const auto status = upscaler::status();
    const State upscaling = wanted.mode == upscaler::Mode::Off ? State::Off
        : !status.supportedBuild || status.failed                ? State::Unavailable
        : status.running                                          ? State::Working
                                                                  : State::NotRunning;
    const State rayTracedShadows = stateOf(wanted.rayTracedShadows, held, false, shadowsFailed, rayTracing);
    const State softShadows = stateOf(wanted.softShadows, held, wanted.rayTracedShadows, shadowsFailed, {});
    const State sunContact = stateOf(wanted.sunScreenSpaceShadows, held, wanted.rayTracedShadows, shadowsFailed, {});
    const State rayTracedLights = stateOf(wanted.rayTracedLights, held, false, false, lightsProblem);
    const State lampContact = stateOf(wanted.lightScreenSpaceShadows, held, wanted.rayTracedLights, shadowsFailed, {});
    const State global = stateOf(wanted.globalIllumination, held, false, false, rayTracing);
    const State occlusion = stateOf(wanted.ambientOcclusion, held, wanted.globalIllumination, false, occlusionProblem);
    const State indirect = stateOf(wanted.indirectLight, held, wanted.globalIllumination, false, lightProblem);
    const State fog = stateOf(wanted.volumetricFog, held, false, false, fogProblem);

    State output = State::Working;
    std::string outputMessage;
    if (wanted.hdr != hdr::active())
    {
        output = State::Restart;
        outputMessage = wanted.hdr ? "Restart the game to turn HDR on." : "Restart the game to turn HDR off.";
    }
    else if (!wanted.hdr) output = State::Off;
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

    // The replacement sun light pass runs for every sun shadow.
    const Effect sunShadows[]{{"rayTracedShadows", rayTracedShadows}, {"softShadows", softShadows}, {"sunContactShadows", sunContact}};
    const auto sunLight = owner(sunShadows);

    // Each ray-traced effect has its own rays on Direct3D 12. What they share, the scene and handing the
    // frame over to Direct3D 12 and back, splits evenly between those working. Direct3D 11 waits while
    // Direct3D 12 works, so its inputs and wait, less all the Direct3D 12 passes, are the hand-over.
    const Effect tracers[]{{"globalIllumination", global}, {"rayTracedShadows", rayTracedShadows}, {"rayTracedLights", rayTracedLights}};
    float traced{};
    for (const auto pass : {Pass::RayTracedScene, Pass::SunShadowRays, Pass::SunShadowDenoiser, Pass::LightShadowRays, Pass::LightShadowFilter, Pass::GiRays,
             Pass::GiDenoiser})
        traced += timed(pass);

    const float handOver = std::max(0.0f, timed(Pass::GiInputs) + timed(Pass::GiWait) - traced);
    const auto working = std::ranges::count(tracers, State::Working, &Effect::second);
    const auto rays = owner(tracers);
    // How much of what the ray tracing shares a line carries: an even share while any of them works,
    // else all of it on the first that is on.
    const auto shareOf = [&](std::string_view id, State state) {
        if (std::ranges::find(tracers, id, &Effect::first) == std::ranges::end(tracers)) return 0.0f;
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
        result.lines.push_back(std::move(line));
    };

    add({.id = "upscaler", .name = "Upscaler", .tech = upscaler::label(wanted.upscaler), .section = at(Section::Upscaling), .state = upscaling},
        {{Pass::UpscalerInputs, "Motion vectors and inputs"}, {Pass::Upscaler, "DLSS or FSR"}, {Pass::UpscalerOutput, "Sharpening and output"}});
    add({.id = "rayTracedShadows", .name = "Ray-traced shadows", .section = at(Section::Shadows), .state = rayTracedShadows, .toggle = &wanted.rayTracedShadows,
            .message = messageFor(rayTracedShadows, rayTracing, "Ray-traced shadows are unavailable (see PhantomFX.log).")},
        {{Pass::SunShadowRays, "Shadow rays"}, {Pass::SunShadowDenoiser, "Denoiser"}});
    add({.id = "softShadows", .name = "Soft shadows", .tech = "PCSS", .section = at(Section::Shadows), .state = softShadows, .toggle = &wanted.softShadows,
            .replacedBy = "rayTracedShadows", .message = messageFor(softShadows, {}, "Soft shadows are unavailable (see PhantomFX.log).")},
        {});
    add({.id = "sunContactShadows", .name = "Sun contact shadows", .section = at(Section::Shadows), .state = sunContact, .toggle = &wanted.sunScreenSpaceShadows,
            .replacedBy = "rayTracedShadows", .message = messageFor(sunContact, {}, "Contact shadows are unavailable (see PhantomFX.log).")},
        {{Pass::SunTrace, "Contact shadow trace"}});
    add({.id = "rayTracedLights", .name = "Ray-traced lights", .section = at(Section::Shadows), .state = rayTracedLights, .toggle = &wanted.rayTracedLights,
            .message = messageFor(rayTracedLights, lightsProblem)},
        {{Pass::LightShadowRays, "Shadow rays"}, {Pass::LightShadowFilter, "Shadow filter"}, {Pass::LocalLights, "Lights with their traced shadows"}});
    add({.id = "lampContactShadows", .name = "Lamp contact shadows", .section = at(Section::Shadows), .state = lampContact, .toggle = &wanted.lightScreenSpaceShadows,
            .replacedBy = "rayTracedLights", .message = messageFor(lampContact, {}, "Contact shadows are unavailable (see PhantomFX.log).")},
        {{Pass::LampTraces, "Traces"}, {Pass::LampShadows, "Shadow draws"}});
    add({.id = "globalIllumination", .name = "Global illumination", .tech = "RTGI", .section = at(Section::Lighting), .state = global,
            .toggle = &wanted.globalIllumination, .message = messageFor(global, rayTracing)},
        {{Pass::GiRays, "Rays and the light cache"}, {Pass::GiDenoiser, "Denoiser and render size"},
            {Pass::GiProbeLight, "The game's probe light, copied"}, {Pass::GiCombine, "Adding the light"}});
    add({.id = "ambientOcclusion", .name = "Ambient occlusion", .tech = "GTVBAO", .section = at(Section::Lighting), .state = occlusion,
            .toggle = &wanted.ambientOcclusion, .replacedBy = "globalIllumination", .message = messageFor(occlusion, occlusionProblem)},
        {{Pass::OcclusionPrefilter, "Depth prefilter"}, {Pass::OcclusionMarch, "March"}, {Pass::OcclusionDenoise, "Denoise"},
            {Pass::OcclusionApply, "Applying to the indirect light"}});
    add({.id = "indirectLight", .name = "Indirect lighting", .tech = "GT-VBGI", .section = at(Section::Lighting), .state = indirect,
            .toggle = &wanted.indirectLight, .replacedBy = "globalIllumination", .message = messageFor(indirect, lightProblem)},
        {{Pass::LightPrefilter, "Depth prefilter"}, {Pass::LightMarch, "March (half size)"}, {Pass::LightDenoise, "Denoise"},
            {Pass::LightUpsample, "Upscaling to the render size"}});
    add({.id = "volumetricFog", .name = "Volumetric fog", .section = at(Section::Atmosphere), .state = fog, .toggle = &wanted.volumetricFog,
            .message = messageFor(fog, fogProblem)},
        {{Pass::FogPass, "The game's fog pass, larger"}, {Pass::SkyTables, "Sky tables (physical look)"}, {Pass::FogShadow, "Sun shadow in the fog"},
            {Pass::FogScattering, "Scattering"}, {Pass::FogIntegration, "Integration"}, {Pass::Sky, "Physical sky"}});
    add({.id = "hdr", .name = "HDR output", .tech = "HDR10", .section = at(Section::Display), .state = output, .toggle = &wanted.hdr, .message = outputMessage},
        {{Pass::HdrEncode, "Encoding the scene"}, {Pass::HdrDecode, "Decoding copies"}, {Pass::HdrGrade, "Grading"}});

    return result;
}
}
