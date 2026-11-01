#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
// The caustics a river's waves put on the bed under them (forward_shadows): the light the surface focuses, strongest
// just under it, gone where the water is too thin to focus any or deep enough to have spread it.
struct WaterCaustics
{
    bool on{true};
    float intensity{1.0f};

    bool operator==(const WaterCaustics&) const = default;
};

void describe(WaterCaustics& caustics, SettingVisitor& visit);
}
