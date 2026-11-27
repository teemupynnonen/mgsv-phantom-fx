#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/light_before_volume.hpp"
#include "phantom_shaders/light_bounds.hpp"
#include "phantom_shaders/light_full_screen.hpp"
#include "phantom_shaders/light_shadow.hpp"
#include "phantom_shaders/light_volume.hpp"

// The lights' own passes' HLSL (local_lights.cpp), precompiled at build time (shaders/).
namespace phantom_fx::local_lights::shaders
{
inline constexpr Shader FullScreen{g_light_full_screen, sizeof(g_light_full_screen), "features/ray_traced_lights/shaders/light_full_screen.vs.hlsl", "vs_5_0"};
inline constexpr Shader Volume{g_light_volume, sizeof(g_light_volume), "features/ray_traced_lights/shaders/light_volume.vs.hlsl", "vs_5_0"};
inline constexpr Shader Shadow{g_light_shadow, sizeof(g_light_shadow), "features/ray_traced_lights/shaders/light_shadow.ps.hlsl", "ps_5_0"};
inline constexpr Shader BeforeVolume{g_light_before_volume, sizeof(g_light_before_volume), "features/ray_traced_lights/shaders/light_before_volume.ps.hlsl",
    "ps_5_0"};
inline constexpr Shader Bounds{g_light_bounds, sizeof(g_light_bounds), "features/ray_traced_lights/shaders/light_bounds.cs.hlsl", "cs_5_0"};
}
