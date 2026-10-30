#include "features/path_tracing/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr PathTracing Defaults{};
}

void describe(PathTracing& pathTracing, SettingVisitor& visit)
{
    visit.feature(pathTracing.on,
        {.menuId = "pathTracing",
            .iniKey = "path_tracing",
            .section = Section::Lighting,
            .name = "Path tracing",
            .tech = "PT",
            .tag = "Experimental",
            .tip = "With DLSS Ray Reconstruction denoising, the reflections and the direct light are traced too. A reflection ray from every "
                   "pixel, sharp on smooth surfaces and spread on rough ones, in place of the game's screen-space and sky reflections; water "
                   "and glass keep the game's. The sun lit by its traced shadow alone, and every point and spot light by the path tracer, "
                   "each with a shadow ray, in place of the game's light shaders. Rays that hit a surface see its real material, reflect on from "
                   "shiny surfaces, take light from glowing ones and see the game's own sky where they get away. The "
                   "sun's and the lights' rays are its own: it replaces ray-traced shadows and lights while on. Needs the global illumination on "
                   "and DLSS Ray Reconstruction running.",
            .summary = "Traced reflections, sun and lamps, in place of the game's reflections and light shaders.",
            .keywords = "PT path tracing reflections ray reconstruction lamps lights sun",
            .effect = Effect::PathTracing,
            .needs = Effect::GlobalIllumination,
            .needsRayReconstruction = true,
            .needsSupportedBuild = true});
    visit.slider(pathTracing.reflections,
        {.iniKey = "path_traced_reflections",
            .label = "Reflections",
            .max = 4,
            .standard = Defaults.reflections,
            .tip = "How strongly surfaces reflect. 1 is the strength the game gives each material; higher makes the reflections stronger, 0 leaves them out."});
    visit.slider(pathTracing.emission,
        {.iniKey = "path_traced_emission",
            .label = "Self-lit surfaces",
            .max = 4,
            .standard = Defaults.emission,
            .tip = "How strongly glowing surfaces such as bulbs and signs light what is around them. 1 is the brightness the game gives them; 0 leaves their light out."});
}
}
