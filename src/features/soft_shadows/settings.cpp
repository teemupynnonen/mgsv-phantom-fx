#include "features/soft_shadows/settings.hpp"

namespace phantom_fx
{
void describe(SoftShadows& shadows, SettingVisitor& visit)
{
    visit.feature(shadows.on,
        {.menuId = "softShadows",
            .iniKey = "soft_shadows",
            .section = Section::Lighting,
            .name = "Soft shadows",
            .tech = "PCSS",
            .tip = "Contact-hardening soft shadows from the shadow maps of the sun and the lamps: sharp where an object meets the ground, "
                   "softer the further the shadow falls from it, as the sun's disc or the lamp's size gives. Lamps get them in a rebuilt copy "
                   "of the game's own shader; only those the game gives a shadow map take it, and most of its point lights have none. Off "
                   "keeps the game's evenly sharp shadow edges.",
            .summary = "Shadows get softer the farther they fall.",
            .keywords = "sun size lamp point spot",
            .effect = Effect::SoftShadows,
            .replacedBy = Effect::RayTracedShadows});
    visit.feature(shadows.on,
        {.menuId = "softShadows",
            .section = Section::Lighting,
            .name = "Lamp soft shadows",
            .tech = "PCSS",
            .effect = Effect::LampSoftShadows,
            .replacedBy = Effect::RayTracedLights});
}
}
