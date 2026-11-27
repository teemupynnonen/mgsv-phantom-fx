#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/pt_add_lamp_light.hpp"
#include "phantom_shaders/pt_add_reflections.hpp"
#include "phantom_shaders/pt_full_screen.hpp"
#include "phantom_shaders/pt_mask_copy.hpp"
#include "phantom_shaders/pt_reflection_blend.hpp"

// HLSL for path tracing on Direct3D 11 (path_tracing.hpp), precompiled at build time (shaders/).
namespace phantom_fx::path_tracing::shaders
{
inline constexpr Shader AddReflections{g_pt_add_reflections, sizeof(g_pt_add_reflections), "features/path_tracing/shaders/add_reflections.cs.hlsl", "cs_5_0"};
inline constexpr Shader FullScreen{g_pt_full_screen, sizeof(g_pt_full_screen), "features/path_tracing/shaders/full_screen.vs.hlsl", "vs_5_0"};
inline constexpr Shader AddLampLight{g_pt_add_lamp_light, sizeof(g_pt_add_lamp_light), "features/path_tracing/shaders/add_lamp_light.ps.hlsl", "ps_5_0"};
inline constexpr Shader MaskCopy{g_pt_mask_copy, sizeof(g_pt_mask_copy), "features/path_tracing/shaders/mask_copy.cs.hlsl", "cs_5_0"};
inline constexpr Shader ReflectionBlend{g_pt_reflection_blend, sizeof(g_pt_reflection_blend), "features/path_tracing/shaders/reflection_blend.ps.hlsl", "ps_5_0"};
}
