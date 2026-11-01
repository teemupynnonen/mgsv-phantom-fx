#include "features/water_reflections/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr WaterReflections Defaults{};
}

void describe(WaterReflections& reflections, SettingVisitor& visit)
{
    visit.feature({.menuId = "waterReflections",
        .section = Section::Atmosphere,
        .name = "Water reflections",
        .summary = "How the traced reflection on a river spreads.",
        .keywords = "water river reflection roughness ripples"});
    visit.slider(reflections.roughness,
        {.iniKey = "water_reflection_roughness",
            .label = "Roughness",
            .min = 0.05f,
            .max = 0.45f,
            .standard = Defaults.roughness,
            .tip = "How far the traced reflection spreads on a river. 0.1 is nearly a mirror; higher blurs it as ripples would. Past 0.45 the trace "
                   "takes its rough path, where the reflection is mostly the light cache's."});
}
}
