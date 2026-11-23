#include "features/clouds/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr Clouds Defaults{};
constexpr SettingChoice Qualities[]{
    {static_cast<int>(CloudQuality::Low), "Low", "low"},
    {static_cast<int>(CloudQuality::Medium), "Medium", "medium"},
    {static_cast<int>(CloudQuality::High), "High", "high"},
    {static_cast<int>(CloudQuality::Ultra), "Ultra", "ultra"},
};
}

void describe(Clouds& clouds, SettingVisitor& visit)
{
    visit.feature(clouds.on,
        {.menuId = "clouds",
            .iniKey = "clouds",
            .section = Section::World,
            .name = "Clouds",
            .tag = "Experimental",
            .tip = "The game's cloud deck raymarched with far more steps than its own four, so the shapes hold together instead of "
                   "blurring into a grey sheet. It keeps the game's weather, its coverage and its wind, and lights the clouds with the "
                   "sun's phase and a powder edge. Off leaves the game's clouds.",
            .summary = "The cloud deck with shape and lit edges.",
            .keywords = "clouds sky weather overcast volumetric"});
    visit.choice(clouds.quality,
        {.iniKey = "clouds_quality",
            .label = "Quality",
            .choices = Qualities,
            .tip = "How finely the cloud is marched. Medium is the cheapest shape that still holds; High resolves the billows at the edges; "
                   "Ultra is for screenshots and costs the most. Low is a soft, cheap cloud for weak GPUs.",
            .visible = clouds.on});
    visit.slider(clouds.coverage,
        {.iniKey = "clouds_coverage",
            .label = "Coverage",
            .min = 0.2f,
            .max = 2,
            .standard = Defaults.coverage,
            .format = "%.0f%%",
            .readoutScale = 100,
            .tip = "More or fewer clouds in the same weather: how much of the sky the weather's coverage fills. 100% is the game's own "
                   "coverage, 200% an overcast even in fair weather.",
            .visible = clouds.on});
    visit.slider(clouds.size,
        {.iniKey = "clouds_size",
            .label = "Size",
            .min = 0.5f,
            .max = 2,
            .standard = Defaults.size,
            .format = "%.0f%%",
            .readoutScale = 100,
            .tip = "How large each cloud is: the same sky, filled by wider or smaller shapes. Larger gives fewer, grander clouds.",
            .visible = clouds.on});
    visit.slider(clouds.shadow,
        {.iniKey = "clouds_shadow",
            .label = "Ground shadow",
            .min = 0,
            .max = 1.5f,
            .standard = Defaults.shadow,
            .format = "%.0f%%",
            .readoutScale = 100,
            .tip = "How much of the sun the clouds keep from the ground. The shadow follows the clouds' own shape and drift; with "
                   "ray-traced shadows it falls through the rays, and 0% leaves the ground lit.",
            .visible = clouds.on});
    visit.slider(clouds.wind,
        {.iniKey = "clouds_wind",
            .label = "Wind",
            .min = 0,
            .max = 3,
            .standard = Defaults.wind,
            .format = "%.0f%%",
            .readoutScale = 100,
            .tip = "How fast the clouds travel. 100% is the game's own wind speed for these clouds, 0% holds them still.",
            .visible = clouds.on});
}
}
