#include "passes/ray_tracing/kick_traces.hpp"

#include "phantom_fx_settings.hpp"

#include <iostream>
#include <stdexcept>

namespace
{
using namespace phantom_fx;
using namespace phantom_fx::kick_traces;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

Settings pathTracedSettings()
{
    Settings settings;
    settings.rayTracedShadows.on = settings.rayTracedLights.on = true;
    settings.globalIllumination.on = settings.pathTracing.on = settings.upscaling.rayReconstruction = true;

    return settings;
}

Settings hybridSettings()
{
    Settings settings;
    settings.rayTracedShadows.on = settings.rayTracedLights.on = settings.globalIllumination.on = true;

    return settings;
}

Asked askedOf(const Settings& settings, const Diagnostics& diagnostics = {}, const LeftOut& leftOut = {})
{
    return askedBy(Configuration(settings, diagnostics, {.leftOut = leftOut}));
}

constexpr KickFacts Everything{.isDenoisedByReconstruction = true, .hasSun = true, .hasLights = true, .hasMaterialTables = true, .hasGlassTargets = true};

// Replaced as they are, path tracing still needs their rays. Commit e2852d5 fixed this for the readers alone.
void pathTracingKeepsTheRaysOfWhatItReplaces()
{
    const auto asked = askedOf(pathTracedSettings());
    expect(!asked.runsRayTracedShadows && !asked.runsRayTracedLights, "Path tracing replaces the ray-traced shadows and lights.");
    expect(asked.needsSunRays() && asked.needsLampRays(), "Their readers stay on for path tracing.");

    const auto traces = tracesOf(asked, Everything);
    expect(traces.isPathTraced && traces.tracesSun && traces.tracesLampLight && traces.tracesLight && traces.tracesReflections && traces.tracesGlass,
        "A path-traced kick traces the sun, the lamps' light, the light, the reflections and the glass.");
    expect(!traces.tracesLampShadows, "The lamps' light takes the place of their shadow rays.");
}

// The rays' sun and lamps wait with it: what it replaces stays off, as a replaced effect does.
void pathTracingWaitsForRayReconstruction()
{
    const auto asked = askedOf(pathTracedSettings());
    KickFacts notDenoised = Everything;
    notDenoised.isDenoisedByReconstruction = false;
    notDenoised.hasLights = takesLights(asked, false);
    expect(!notDenoised.hasLights, "Without path tracing and the ray-traced lights the kick takes no lights.");

    const auto traces = tracesOf(asked, notDenoised);
    expect(!traces.isPathTraced && !traces.isForReconstruction, "Nothing is noisy before Ray Reconstruction denoises.");
    expect(!traces.tracesSun && !traces.tracesLampShadows && !traces.tracesLampLight && !traces.tracesReflections && !traces.tracesGlass,
        "The kick traces none of path tracing's rays, nor those it replaced.");
    expect(traces.tracesLight, "The light goes on.");
}

void lampsWaitForTheGamesTables()
{
    KickFacts noTables = Everything;
    noTables.hasMaterialTables = false;
    const auto traces = tracesOf(askedOf(pathTracedSettings()), noTables);
    expect(traces.isPathTraced && traces.tracesLampShadows && !traces.tracesLampLight, "Until the tables come the game's shaders light the lamps, with the rays' shadows.");
}

void noLightsNoLampRays()
{
    KickFacts noLights = Everything;
    noLights.hasLights = false;
    const auto traces = tracesOf(askedOf(pathTracedSettings()), noLights);
    expect(traces.isPathTraced && !traces.tracesLampLight && !traces.tracesLampShadows, "A kick without lights traces neither.");
}

void noSunNoSunRays()
{
    KickFacts noSun = Everything;
    noSun.hasSun = false;
    expect(!tracesOf(askedOf(pathTracedSettings()), noSun).tracesSun, "Path tracing's sun needs its direction.");
    expect(!tracesOf(askedOf(hybridSettings()), noSun).tracesSun, "So do the ray-traced shadows.");
}

void withoutPathTracingEachEffectTracesItsOwn()
{
    const auto asked = askedOf(hybridSettings());
    expect(asked.runsRayTracedShadows && asked.runsRayTracedLights && !asked.runsPathTracing, "Nothing replaces them.");
    expect(takesLights(asked, false), "The ray-traced lights take the lights.");

    const auto traces = tracesOf(asked, Everything);
    expect(traces.tracesSun && traces.tracesLampShadows && traces.tracesLight, "Each runs.");
    expect(!traces.isPathTraced && !traces.tracesLampLight && !traces.tracesReflections && !traces.tracesGlass, "Nothing of path tracing's runs.");
    expect(!traces.isForReconstruction, "Without Ray Reconstruction nothing is noisy for it.");
}

void noisyForRayReconstructionWithoutPathTracing()
{
    auto settings = hybridSettings();
    settings.upscaling.rayReconstruction = true;
    const auto traces = tracesOf(askedOf(settings), Everything);
    expect(traces.isForReconstruction && !traces.isPathTraced, "Ray Reconstruction denoises the light and the shadows of a kick not path traced.");
}

void leavingOutTheLightStopsPathTracing()
{
    LeftOut light;
    light.globalLight = true;
    const auto asked = askedOf(pathTracedSettings(), {}, light);
    expect(asked.runsGlobalIllumination && !asked.runsLight && asked.needsScene(), "The output keeps its last light, on a scene still built.");

    const auto traces = tracesOf(asked, Everything);
    expect(!traces.isPathTraced && !traces.tracesLight && !traces.tracesReflections, "Path tracing's reflections read the light's cache.");
}

// As before the decision was one: the frame stays path traced, its reflections as last traced.
void sceneViewLeavesOutTheReflections()
{
    Diagnostics diagnostics;
    diagnostics.showTracedScene = true;
    const auto traces = tracesOf(askedOf(pathTracedSettings(), diagnostics), Everything);
    expect(traces.isPathTraced && !traces.tracesReflections && !traces.tracesGlass, "The scene view runs no reflections.");
}

void glassNeedsItsTargets()
{
    KickFacts noTargets = Everything;
    noTargets.hasGlassTargets = false;
    const auto traces = tracesOf(askedOf(pathTracedSettings()), noTargets);
    expect(traces.tracesReflections && !traces.tracesGlass, "The kick's reflections do not need the glass.");
}

void offPlanTracesNothing()
{
    const auto asked = askedOf(Settings{});
    expect(!asked.needsScene() && !asked.needsSunRays() && !asked.needsLampRays(), "No effect asks for the scene.");

    const auto traces = tracesOf(asked, {.isDenoisedByReconstruction = true, .hasSun = true, .hasMaterialTables = true, .hasGlassTargets = true});
    expect(!traces.isPathTraced && !traces.tracesSun && !traces.tracesLight && !traces.tracesLampShadows && !traces.tracesLampLight, "Nothing runs.");
}
}

int main()
{
    try
    {
        pathTracingKeepsTheRaysOfWhatItReplaces();
        pathTracingWaitsForRayReconstruction();
        lampsWaitForTheGamesTables();
        noLightsNoLampRays();
        noSunNoSunRays();
        withoutPathTracingEachEffectTracesItsOwn();
        noisyForRayReconstructionWithoutPathTracing();
        leavingOutTheLightStopsPathTracing();
        sceneViewLeavesOutTheReflections();
        glassNeedsItsTargets();
        offPlanTracesNothing();
        std::cout << "Kick traces tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
