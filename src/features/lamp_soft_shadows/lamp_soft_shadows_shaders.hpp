#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/lamp_point_light.hpp"
#include "phantom_shaders/lamp_spot_light.hpp"

// HLSL for the lamps' soft shadows (lamp_soft_shadows.hpp): the game's point and spot light shaders
// rebuilt, precompiled at build time (shaders/).
namespace phantom_fx::lamp_soft_shadows::shaders
{
inline constexpr Shader PointLight{g_lamp_point_light, sizeof(g_lamp_point_light), "features/lamp_soft_shadows/shaders/point_light.ps.hlsl", "ps_5_0"};
inline constexpr Shader SpotLight{g_lamp_spot_light, sizeof(g_lamp_spot_light), "features/lamp_soft_shadows/shaders/spot_light.ps.hlsl", "ps_5_0"};
}
