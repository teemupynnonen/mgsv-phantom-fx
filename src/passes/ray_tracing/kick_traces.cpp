#include "passes/ray_tracing/kick_traces.hpp"

#include "phantom_fx_settings.hpp"

namespace phantom_fx::kick_traces
{
Asked askedBy(const Configuration& configuration)
{
    const auto& effects = configuration.effects;
    const bool runsGlobalIllumination = effects.runs(Effect::GlobalIllumination);

    return {.runsGlobalIllumination = runsGlobalIllumination,
        .runsLight = runsGlobalIllumination && !configuration.leftOut.globalLight,
        .runsRayTracedShadows = effects.runs(Effect::RayTracedShadows),
        .runsRayTracedLights = effects.runs(Effect::RayTracedLights),
        .runsPathTracing = effects.runs(Effect::PathTracing),
        .asksRayReconstruction = configuration.settings.upscaling.isRayReconstructionAsked(),
        .showsScene = configuration.diagnostics.showTracedScene};
}

bool isPathTraced(const Asked& asked, bool isDenoisedByReconstruction)
{
    return asked.runsPathTracing && asked.asksRayReconstruction && isDenoisedByReconstruction && asked.runsLight;
}

bool takesLights(const Asked& asked, bool isDenoisedByReconstruction)
{
    return asked.runsRayTracedLights || isPathTraced(asked, isDenoisedByReconstruction);
}

KickTraces tracesOf(const Asked& asked, const KickFacts& facts)
{
    const bool pathTraced = isPathTraced(asked, facts.isDenoisedByReconstruction);
    const bool tracesLampLight = pathTraced && facts.hasLights && facts.hasMaterialTables;
    // The reflections read the light's cache, which the scene view leaves out.
    const bool tracesReflections = pathTraced && !asked.showsScene;

    return {.isPathTraced = pathTraced,
        // Only while it denoised the frame before: a frame it left to DLSS, or none, turns the denoisers back on.
        .isForReconstruction = asked.asksRayReconstruction && facts.isDenoisedByReconstruction,
        .tracesLampShadows = facts.hasLights && !tracesLampLight,
        .tracesLampLight = tracesLampLight,
        .tracesSun = facts.hasSun && (asked.runsRayTracedShadows || pathTraced),
        .tracesLight = asked.runsLight,
        .tracesReflections = tracesReflections,
        .tracesGlass = tracesReflections && facts.hasGlassTargets};
}
}
