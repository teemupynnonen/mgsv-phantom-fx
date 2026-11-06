#include "features/frame_rate/settings.hpp"

namespace phantom_fx
{
void describe(FrameRate& frameRate, SettingVisitor& visit)
{
    visit.feature(frameRate.unlocked,
        {.menuId = "unlockFrameRate",
            .iniKey = "unlock_fps",
            .section = Section::Image,
            .name = "Unlock frame rate",
            .tip = "Lifts the game's 60 fps cap, like framerate_control \"Variable\" in its settings file. Off keeps the game's setting.",
            .summary = "Removes the game's 60 fps cap.",
            .keywords = "fps 60 cap limit"});
}
}
