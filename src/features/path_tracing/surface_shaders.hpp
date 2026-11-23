#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/surfaces_composite.hpp"
#include "phantom_shaders/surfaces_glass.hpp"
#include "phantom_shaders/surfaces_glass_color.hpp"
#include "phantom_shaders/surfaces_guides.hpp"
#include "phantom_shaders/surfaces_mirror.hpp"
#include "phantom_shaders/water_surface_0_0_0.hpp"
#include "phantom_shaders/water_surface_0_0_1.hpp"
#include "phantom_shaders/water_surface_0_1_0.hpp"
#include "phantom_shaders/water_surface_0_1_1.hpp"
#include "phantom_shaders/water_surface_1_0_0.hpp"
#include "phantom_shaders/water_surface_1_0_1.hpp"
#include "phantom_shaders/water_surface_1_1_0.hpp"
#include "phantom_shaders/water_surface_1_1_1.hpp"

// HLSL for the forward surfaces on Direct3D 11 (forward_surfaces.hpp), precompiled at build time (shaders/).
namespace phantom_fx::surface_shaders
{
inline constexpr Shader ReconstructionGuides{g_surfaces_guides, sizeof(g_surfaces_guides), "features/path_tracing/shaders/reconstruction_guides.cs.hlsl",
    "cs_5_0"};
inline constexpr Shader Glass{g_surfaces_glass, sizeof(g_surfaces_glass), "features/path_tracing/shaders/glass_surface.ps.hlsl", "ps_5_0"};
inline constexpr Shader GlassColor{g_surfaces_glass_color, sizeof(g_surfaces_glass_color), "features/path_tracing/shaders/glass_surface_color.ps.hlsl", "ps_5_0"};
inline constexpr Shader Mirror{g_surfaces_mirror, sizeof(g_surfaces_mirror), "features/path_tracing/shaders/mirror_surface.ps.hlsl", "ps_5_0"};
inline constexpr Shader Composite{g_surfaces_composite, sizeof(g_surfaces_composite), "features/path_tracing/shaders/surfaces_composite.ps.hlsl", "ps_5_0"};

// [isLayeredRiver][hasTwoChannelNormal][hasRipples] (game_shaders.hpp)
inline constexpr Shader Water[2][2][2]{
    {{Shader{g_water_surface_0_0_0, sizeof(g_water_surface_0_0_0), "features/path_tracing/shaders/water_surface_0_0_0.ps.hlsl", "ps_5_0"},
         Shader{g_water_surface_0_0_1, sizeof(g_water_surface_0_0_1), "features/path_tracing/shaders/water_surface_0_0_1.ps.hlsl", "ps_5_0"}},
        {Shader{g_water_surface_0_1_0, sizeof(g_water_surface_0_1_0), "features/path_tracing/shaders/water_surface_0_1_0.ps.hlsl", "ps_5_0"},
            Shader{g_water_surface_0_1_1, sizeof(g_water_surface_0_1_1), "features/path_tracing/shaders/water_surface_0_1_1.ps.hlsl", "ps_5_0"}}},
    {{Shader{g_water_surface_1_0_0, sizeof(g_water_surface_1_0_0), "features/path_tracing/shaders/water_surface_1_0_0.ps.hlsl", "ps_5_0"},
         Shader{g_water_surface_1_0_1, sizeof(g_water_surface_1_0_1), "features/path_tracing/shaders/water_surface_1_0_1.ps.hlsl", "ps_5_0"}},
        {Shader{g_water_surface_1_1_0, sizeof(g_water_surface_1_1_0), "features/path_tracing/shaders/water_surface_1_1_0.ps.hlsl", "ps_5_0"},
            Shader{g_water_surface_1_1_1, sizeof(g_water_surface_1_1_1), "features/path_tracing/shaders/water_surface_1_1_1.ps.hlsl", "ps_5_0"}}}};
}
