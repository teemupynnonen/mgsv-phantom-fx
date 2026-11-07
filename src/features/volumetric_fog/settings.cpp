#include "features/volumetric_fog/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr VolumetricFog Defaults{};
constexpr SettingChoice Looks[]{
    {static_cast<int>(AtmosphereLook::Mgsv), "MGSV", "mgsv"},
    {static_cast<int>(AtmosphereLook::Physical), "Physical", "physical"},
};
}

void describe(VolumetricFog& fog, SettingVisitor& visit)
{
    visit.feature(fog.on,
        {.menuId = "volumetricFog",
            .iniKey = "volumetric_fog",
            .section = Section::World,
            .name = "Volumetric fog",
            .tip = "Fog with the sun shadowed in it: light shafts through trees, buildings and dust, and shade where the sun cannot reach. "
                   "Shadows reach about 90 m, as the game's do. Off leaves the game's fog unchanged.",
            .summary = "Light shafts through trees, buildings and dust.",
            .keywords = "sky look physical MGSV haze light shafts",
            .effect = Effect::VolumetricFog,
            .needsSupportedBuild = true});
    visit.choice(fog.look,
        {.iniKey = "atmosphere_look",
            .label = "Look",
            .choices = Looks,
            .tip = "MGSV keeps the game's own sky and haze colours, with the sun shadowed in the fog. Physical draws the sky and the air as they "
                   "are in nature (Hillaire's atmosphere, as in Unreal Engine), as bright as the game's sky, with height fog that gathers low, as "
                   "thick as the game's weather makes its fog."});
    visit.slider(fog.haze,
        {.iniKey = "atmosphere_haze",
            .label = "Haze",
            .standard = Defaults.haze,
            .format = "%.0f%%",
            .readoutScale = 100,
            .tip = "Dust in the air that fades the distance. A matter of taste: no setting is more physically based or closer to the game's "
                   "look. 0% (the default) is clear air, about 120 km of view on a clear day; 100% is about 8 km, with a paler sky. Fog, "
                   "sandstorms and rain are always at full thickness.",
            .visible = fog.look == AtmosphereLook::Physical});
}
}
