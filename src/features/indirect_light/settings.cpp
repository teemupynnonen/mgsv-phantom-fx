#include "features/indirect_light/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr IndirectLight Defaults{};
}

void describe(IndirectLight& light, SettingVisitor& visit)
{
    visit.feature(light.on,
        {.menuId = "indirectLight",
            .iniKey = "indirect_light",
            .section = Section::Lighting,
            .name = "Bounce light",
            .tech = "SSILVB",
            .tip = "Screen-space indirect lighting with visibility bitmask: light bounces off nearby surfaces onto others, taking their "
                   "colour, such as sunlit ground brightening the underside of a jeep. It only knows what is on screen. Off leaves the game's "
                   "indirect light unchanged.",
            .summary = "Light picks up colour from nearby surfaces.",
            .keywords = "SSGI bounce radius strength",
            .effect = Effect::IndirectLight,
            .replacedBy = Effect::GlobalIllumination,
            .needsSupportedBuild = true});
    visit.slider(light.radiusMetres,
        {.iniKey = "indirect_light_radius",
            .label = "Radius",
            .min = 0.25f,
            .max = 4,
            .standard = Defaults.radiusMetres,
            .format = "%.2f m",
            .tip = "How far around a surface SSILVB looks for surfaces that light it."});
    visit.slider(light.strength,
        {.iniKey = "indirect_light_strength",
            .label = "Strength",
            .max = 3,
            .standard = Defaults.strength,
            .tip = "1 is physically based; higher brightens the bounced light, 0 turns it off."});
}
}
