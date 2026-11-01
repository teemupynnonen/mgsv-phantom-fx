#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
// How the traced reflection on a river spreads (reflections.hlsl, TraceSurfaceReflections): the water's roughness.
// The game's ripple normal is screen-space and flow driven, out of the trace's reach, so the spread stands in for the
// detail it would add.
struct WaterReflections
{
    float roughness{0.25f};

    bool operator==(const WaterReflections&) const = default;
};

void describe(WaterReflections& reflections, SettingVisitor& visit);
}
