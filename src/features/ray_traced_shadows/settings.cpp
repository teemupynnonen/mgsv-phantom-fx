#include "features/ray_traced_shadows/settings.hpp"

namespace phantom_fx
{
void describe(RayTracedShadows& shadows, SettingVisitor& visit)
{
    visit.feature(shadows.on,
        {.menuId = "rayTracedShadows",
            .iniKey = "ray_traced_shadows",
            .section = Section::Lighting,
            .name = "Sun shadows",
            .tech = "RT",
            .tag = "Experimental",
            .tip = "The sun's shadows from rays traced through the game's own scene: as soft as the sun's disc makes them, exact where objects "
                   "meet the ground, and as far as you can see, where the game's shadows end at about 90 m. It replaces the game's shadows, soft "
                   "shadows and contact shadows while on; Ray traced: Ultra traces its own. Needs a GPU that traces rays (DXR 1.1). Off keeps the "
                   "game's shadow map.",
            .summary = "Accurate sun shadows, as far as you can see.",
            .keywords = "RT DXR ray-traced shadows sun size vanilla",
            .effect = Effect::RayTracedShadows,
            .replacedBy = Effect::PathTracing,
            .needsSupportedBuild = true});
    visit.toggle(shadows.withGameShadows,
        {.iniKey = "vanilla_shadows_with_ray_tracing",
            .label = "Show the game's shadows too, near you",
            .tip = "The game's own shadow map as well, near you (about 90 m): the darker of its shadow and the rays' shows. Off, the rays alone "
                   "shadow the sun."});
}
}
