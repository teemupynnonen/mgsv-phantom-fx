#include "features/volumetric_fog/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr PhysicalSky Defaults{};
}

void describe(VolumetricFog& fog, SettingVisitor& visit)
{
    visit.feature(fog.on,
        {.menuId = "volumetricFog",
            .iniKey = "volumetric_fog",
            .section = Section::World,
            .name = "Light shafts",
            .tip = "The sun shadowed in the fog: light shafts through trees, buildings and dust, and shade where the sun cannot reach. "
                   "Shadows reach about 90 m, as the game's do. The sky and the fog's colours stay as Physical sky leaves them.",
            .summary = "Light shafts through trees, buildings and dust.",
            .keywords = "volumetric fog light shafts god rays sun shadow",
            .effect = Effect::VolumetricFog,
            .needsSupportedBuild = true});
}

void describe(PhysicalSky& sky, SettingVisitor& visit)
{
    visit.feature(sky.on,
        {.menuId = "physicalSky",
            .iniKey = "physical_sky",
            .section = Section::World,
            .name = "Physical sky",
            .tip = "The sky and the air as they are in nature (Hillaire's atmosphere, as in Unreal Engine), in place of the game's sky "
                   "and haze colours: as bright as the game's sky, with height fog that gathers low, as thick as the game's weather makes "
                   "its fog. Indoors, without the sun, the game's own look stays. Off keeps the game's sky.",
            .summary = "A natural sky and air in place of the game's.",
            .keywords = "sky atmosphere physical haze fog air horizon",
            .effect = Effect::PhysicalSky,
            .needsSupportedBuild = true});
    visit.slider(sky.haze,
        {.iniKey = "atmosphere_haze",
            .label = "Haze",
            .standard = Defaults.haze,
            .format = "%.0f%%",
            .readoutScale = 100,
            .tip = "Dust in the air that fades the distance. A matter of taste: no setting is more physically based or closer to the game's "
                   "look. 10% (the default) is a little dust, about 30 km of view on a clear day; 0% is clear air, about 120 km. 100% is about 8 km, with a paler sky. Fog, "
                   "sandstorms and rain are always at full thickness."});
}
}
