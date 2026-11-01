#include "features/global_illumination/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr GlobalIllumination Defaults{};
constexpr SettingChoice Qualities[]{{1, "Low 1"}, {2, "Medium 2"}, {4, "High 4"}};
}

void describe(GlobalIllumination& light, SettingVisitor& visit)
{
    visit.feature(light.on,
        {.menuId = "globalIllumination",
            .iniKey = "global_illumination",
            .section = Section::Lighting,
            .name = "Global illumination",
            .tech = "RTGI",
            .tag = "Experimental",
            .tip = "Ray-traced indirect light: rays for every 2 x 2 pixels, each hit lit afresh by the sun and the lights, its bounce from a "
                   "light cache, then NVIDIA's NRD denoiser. Sunlight, skylight and lamplight bounce between surfaces, also those off screen, "
                   "and the sky only lights what it can reach. It replaces the game's baked probe light and SSAO, and GTVBAO while "
                   "on. Needs a GPU that traces rays (DXR 1.1). Off keeps the game's indirect light.",
            .summary = "Sun and sky light bounce between surfaces, also those off screen, in place of the game's baked light.",
            .keywords = "RT DXR GI strength ray quality sun size",
            .effect = Effect::GlobalIllumination,
            .needsSupportedBuild = true});
    visit.choice(light.raysPerLightPixel,
        {.iniKey = "global_illumination_rays",
            .label = "Ray quality",
            .choices = Qualities,
            .tip = "Rays for every 2 x 2 pixels a frame, twice on grass and leaves. More rays leave less of the noise that crawls over shaded "
                   "surfaces (High has half of Low's); the rays' cost grows with them."});
    visit.slider(light.strength,
        {.iniKey = "global_illumination_strength",
            .label = "Strength",
            .max = 3,
            .standard = Defaults.strength,
            .tip = "1 is physically based; higher brightens the indirect light, 0 leaves only the sun and the lamps."});
}
}
