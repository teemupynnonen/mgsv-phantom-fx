#include "features/motion_blur/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr MotionBlur Defaults{};
}

void describe(MotionBlur& blur, SettingVisitor& visit)
{
    visit.feature(blur.on,
        {.menuId = "motionBlur",
            .iniKey = "motion_blur",
            .section = Section::Camera,
            .name = "Motion blur",
            .tip = "The game's own motion blur, drawn by the engine at the speed and radius its graphics setting asks for, with the upscaler on "
                   "too. Off leaves it out whatever the game's setting says.",
            .summary = "The game's own motion blur, at the strength below.",
            .keywords = "motion blur strength smear trail shutter"});
    visit.slider(blur.strength,
        {.iniKey = "motion_blur_strength",
            .label = "Strength",
            .max = 2,
            .standard = Defaults.strength,
            .format = "%.0f%%",
            .readoutScale = 100,
            .tip = "How far each pixel's motion smears, against what the game's graphics setting asks for: 100% the game's own, 0% none, 200% "
                   "up to twice as far, within the game's limit. Halved while frame generation runs."});
}
}
