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
    visit.slider(sea.speed,
        {.iniKey = "sea_speed",
            .label = "Wave speed",
            .min = 0.1f,
            .max = 2,
            .standard = Defaults.speed,
            .tip = "How fast the waves travel, against real ocean waves. The game's own sea rolls at about half."});
    visit.slider(sea.peak,
        {.iniKey = "sea_peak",
            .label = "Wavelength",
            .min = 0.3f,
            .max = 2,
            .standard = Defaults.peak,
            .tip = "The longest waves' size, against the weather's sea: lower is longer, slower swells, as the game draws them."});
    visit.slider(sea.height,
        {.iniKey = "sea_height",
            .label = "Height",
            .min = 0.2f,
            .max = 2,
            .standard = Defaults.height,
            .tip = "How tall the waves stand, against the weather's sea. It moves the water alone, so the crests keep the light they had."});
    visit.slider(sea.ripples,
        {.iniKey = "sea_ripples",
            .label = "Ripples",
            .min = 0,
            .max = 3,
            .standard = Defaults.ripples,
            .tip = "The chop too short for the waves to draw, as extra roughness where the light breaks up: at 0 the swells stay "
                   "and the water turns glassy between them."});
    visit.slider(sea.chop,
        {.iniKey = "sea_chop",
            .label = "Choppiness",
            .min = 0,
            .max = 2,
            .standard = Defaults.chop,
            .tip = "How sharp the crests are: higher piles the water up into pointed waves that break more."});
    visit.slider(sea.split,
        {.iniKey = "sea_split",
            .label = "Long waves",
            .min = 2,
            .max = 10,
            .standard = Defaults.split,
            .tip = "How much of the sea the long waves hold against the short ones: higher is fewer, broader swells."});
    visit.slider(sea.foamCoverage,
        {.iniKey = "sea_foam_coverage",
            .label = "Foam",
            .min = 0,
            .max = 3,
            .standard = Defaults.foamCoverage,
            .tip = "How much of each crest turns to foam. 1.2 is about the game's own sea; above that the crests break more."});
    constexpr std::string_view AlbedoKeys[]{"sea_albedo_r", "sea_albedo_g", "sea_albedo_b"};
    for (int channel = 0; channel < 3; ++channel)
        visit.slider(sea.albedo[channel],
            {.iniKey = AlbedoKeys[channel], .label = "Water albedo", .min = 0, .max = 1, .standard = Defaults.albedo[channel], .format = "%.3f", .visible = false});
}
}
