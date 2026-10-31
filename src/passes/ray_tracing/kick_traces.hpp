#pragma once

namespace phantom_fx
{
struct Configuration;
}

// What each kick traces (docs/DEVELOPMENT.md, "What a kick traces"). The state of an effect does not say:
// path tracing needs the sun's rays while it replaces the ray-traced shadows. So the effect plan gives what
// a frame asks of the rays, what the kick finds gives what can run, and everything that makes, runs or reads
// a trace takes this one answer.
namespace phantom_fx::kick_traces
{
// What the effect plan asks of the rays for a frame.
struct Asked
{
    bool runsGlobalIllumination{};
    bool runsLight{}; // Its light passes, which a cost measurement can leave out: the output keeps its last light
    bool runsRayTracedShadows{};
    bool runsRayTracedLights{};
    bool runsPathTracing{};
    bool asksRayReconstruction{};
    bool showsScene{}; // Diagnostics: the output shows the traced scene

    bool needsScene() const { return runsGlobalIllumination || runsRayTracedShadows || runsRayTracedLights; }
    // Path tracing's sun and lamps read the rays as the ray-traced shadows and lights do, in their place.
    bool needsSunRays() const { return runsRayTracedShadows || runsPathTracing; }
    bool needsLampRays() const { return runsRayTracedLights || runsPathTracing; }
};

Asked askedBy(const Configuration& configuration);

// What a kick finds.
struct KickFacts
{
    bool isDenoisedByReconstruction{}; // Ray Reconstruction denoised a frame since the last kick
    bool hasSun{};                     // The rays need the sun's direction, which a sun pass gives
    bool hasLights{};                  // The chosen lights
    bool hasMaterialTables{};          // The lamps' specular reads the game's tables, which the first sun pass gives
    bool hasGlassTargets{};
};

// What a kick traces. The glass's reflections run later, on the scene the kick built.
struct KickTraces
{
    bool isPathTraced{};
    bool isForReconstruction{}; // Noisy for Ray Reconstruction: a sample a render pixel, the denoisers off
    bool tracesLampShadows{};
    bool tracesLampLight{};
    bool tracesSun{};
    bool tracesLight{};
    bool tracesReflections{};
    bool tracesGlass{};
};

// What the kick must know before it gathers its facts: it takes the lights, and sets up the glass, only for these.
bool isPathTraced(const Asked& asked, bool isDenoisedByReconstruction);
bool takesLights(const Asked& asked, bool isDenoisedByReconstruction);

KickTraces tracesOf(const Asked& asked, const KickFacts& facts);
}
