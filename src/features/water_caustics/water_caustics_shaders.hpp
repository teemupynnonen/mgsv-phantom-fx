#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/water_caustics.hpp"

// HLSL for the water's caustics (water_caustics.hpp), precompiled at build time (shaders/).
namespace phantom_fx::water_caustics::shaders
{
inline constexpr Shader Caustics{g_water_caustics, sizeof(g_water_caustics), "features/water_caustics/shaders/caustics.ps.hlsl", "ps_5_0"};
}
