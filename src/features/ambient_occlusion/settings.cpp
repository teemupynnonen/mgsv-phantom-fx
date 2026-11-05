#include "features/ambient_occlusion/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr AmbientOcclusion Defaults{};
}

void describe(AmbientOcclusion& occlusion, SettingVisitor& visit)
{
    visit.feature(occlusion.on,
        {.menuId = "ambientOcclusion",
            .iniKey = "ambient_occlusion",
            .section = Section::Lighting,
            .name = "Ambient occlusion",
            .tech = "GTVBAO",
            .tip = "Ground-truth visibility bitmask ambient occlusion in place of the game's SSAO, whatever its SSAO setting. It darkens only "
                   "the indirect light, so sunlit surfaces keep their brightness. Off keeps the game's SSAO, which darkens all light.",
            .summary = "Soft shade in corners and creases.",
            .keywords = "AO SSAO radius strength",
            .effect = Effect::AmbientOcclusion,
            .replacedBy = Effect::GlobalIllumination,
            .needsSupportedBuild = true});
    visit.slider(occlusion.radiusMetres,
        {.iniKey = "occlusion_radius",
            .label = "Radius",
            .min = 0.25f,
            .max = 4,
            .standard = Defaults.radiusMetres,
            .format = "%.2f m",
            .tip = "How far around a surface GTVBAO looks for what occludes it."});
    visit.slider(occlusion.strength,
        {.iniKey = "occlusion_strength",
            .label = "Strength",
            .max = 3,
            .standard = Defaults.strength,
            .tip = "1 is physically based; higher darkens occluded areas more, 0 turns the occlusion off."});
}
}
