#include "features/sea/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr Sea Defaults{};
}

void describe(Sea& sea, SettingVisitor& visit)
{
    visit.feature(sea.on,
        {.menuId = "sea",
            .iniKey = "sea",
            .section = Section::World,
            .name = "Sea",
            .tag = "Experimental",
            .tip = "The sea around Mother Base as a real surface: waves from an ocean spectrum that rise and roll with the wind, and get rougher "
                   "in rain, with foam on their crests. It is lit like everything else, so it reflects the sky, the sun and the platforms, and "
                   "with path tracing its reflections are traced. Off leaves the game's sea.",
            .summary = "Rolling waves that reflect the sky and the platforms.",
            .keywords = "sea ocean water waves foam mother base",
            .effect = Effect::Sea,
            .needsSupportedBuild = true});
    visit.slider(sea.waves,
        {.iniKey = "sea_waves",
            .label = "Waves",
            .min = 0.25f,
            .max = 2,
            .standard = Defaults.waves,
            .tip = "How strong the wind that raises the waves is, against the weather's. 1 is the weather's own: a breeze in the sun, a gale "
                   "in rain."});
}
}
