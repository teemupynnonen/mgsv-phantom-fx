#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
struct GlobalIllumination
{
    bool on{};
    float strength{1};
    int raysPerLightPixel{2};
    bool usesOpacityMicromaps{};

    bool operator==(const GlobalIllumination&) const = default;
};

void describe(GlobalIllumination& light, SettingVisitor& visit);
}
