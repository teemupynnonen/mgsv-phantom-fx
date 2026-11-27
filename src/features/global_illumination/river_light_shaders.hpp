#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/gi_river_light.hpp"

// HLSL for the river's light under the global illumination (river_light.hpp), precompiled at build time (shaders/).
namespace phantom_fx::river_light::shaders
{
inline constexpr Shader Remove{g_gi_river_light, sizeof(g_gi_river_light), "features/global_illumination/shaders/river_light.ps.hlsl", "ps_5_0"};
}
