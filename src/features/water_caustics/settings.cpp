#include "features/water_caustics/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr WaterCaustics Defaults{};
}

void describe(WaterCaustics& caustics, SettingVisitor& visit)
{
    visit.feature(caustics.on,
        {.menuId = "waterCaustics",
            .iniKey = "water_caustics",
            .section = Section::Atmosphere,
            .name = "Water caustics",
            .tip = "The sunlight a river's waves focus on the bed under them: a web of bright cells in the sun, strongest just under the "
                   "surface, fading where the water is deep and gone where it meets the shore or anything standing in it.",
            .summary = "The sunlight a river's waves focus on the bed under them.",
            .keywords = "caustics river water bed waves sun refraction",
            .effect = Effect::WaterCaustics,
            .needsSupportedBuild = true});
    visit.slider(caustics.intensity,
        {.iniKey = "water_caustics_intensity",
            .label = "Intensity",
            .min = 0.0f,
            .max = 2.0f,
            .standard = Defaults.intensity,
            .tip = "How strongly the water's waves focus the sun on the bed. The brightest cells reach past full sun at the top of the range."});
}
}
