#include "features/photo_mode/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr PhotoMode Defaults{};
}

void describe(PhotoMode& photoMode, SettingVisitor& visit)
{
    visit.feature({.menuId = "photoMode",
        .section = Section::Camera,
        .name = "Photo mode",
        .summary = "F2 opens it. Fly with WASD and the mouse, Space and Ctrl for up and down, Shift for faster. F3 hides the HUD, F4 the blur.",
        .keywords = "f-stop aperture blur camera depth of field DOF"});
    visit.slider(photoMode.fNumber,
        {.iniKey = "f_number",
            .label = "f-stop",
            .min = 0.7f,
            .max = 32,
            .standard = Defaults.fNumber,
            .format = "f/%.1f",
            .logarithmic = true,
            .tip = "Photo mode's aperture, for the game's depth of field: a lower f-stop blurs more of what is out of focus. It needs Depth of "
                   "Field on in the game's graphics settings. F4 turns the blur off."});
    visit.toggle(photoMode.noNearClip,
        {.iniKey = "photo_mode_no_near_clip",
            .label = "No near clip plane",
            .tip = "Photo mode's camera sees what is right in front of it: the near clip plane moves from 7 cm to 1 mm, so a wall, "
                   "a face or a blade of grass the camera nearly touches no longer vanishes. Only in photo mode."});
}
}
