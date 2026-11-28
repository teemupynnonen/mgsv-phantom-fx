#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
// The sun shadowed in the game's fog: light shafts.
struct VolumetricFog
{
    bool on{};

    bool operator==(const VolumetricFog&) const = default;
};

// Hillaire's physical sky and air in place of the game's sky and fog colours.
struct PhysicalSky
{
    bool on{};
    // A tenth: enough dust that the far hills and the far clouds fade into the sky, less than a weather's own fog.
    float haze{0.1f};

    bool operator==(const PhysicalSky&) const = default;
};

void describe(VolumetricFog& fog, SettingVisitor& visit);
void describe(PhysicalSky& sky, SettingVisitor& visit);
}
