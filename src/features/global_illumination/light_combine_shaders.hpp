#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/gi_light_combine.hpp"

// HLSL for the global illumination's combine (light_combine.hpp), precompiled at build time (shaders/).
namespace phantom_fx::light_combine::shaders
{
inline constexpr Shader AddLight{g_gi_light_combine, sizeof(g_gi_light_combine), "features/global_illumination/shaders/light_combine.cs.hlsl", "cs_5_0"};
}
