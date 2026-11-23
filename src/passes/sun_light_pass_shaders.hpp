#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/sun_light_no_shadow.hpp"
#include "phantom_shaders/sun_light_no_shadow_sky.hpp"
#include "phantom_shaders/sun_light_path_traced.hpp"
#include "phantom_shaders/sun_light_path_traced_cloud.hpp"
#include "phantom_shaders/sun_light_shadow.hpp"
#include "phantom_shaders/sun_light_shadow_sky.hpp"
#if PHANTOM_FX_DEV_TOOLS
#include "phantom_shaders/sun_light_game_shadow.hpp"
#endif

// HLSL for the sun light pass (sun_light_pass.hpp), precompiled at build time (shaders/).
namespace phantom_fx::sun_light_pass::shaders
{
// [isShadowMapped][reflectsSky]
inline constexpr Shader SunLight[2][2]{
    {Shader{g_sun_light_no_shadow, sizeof(g_sun_light_no_shadow), "passes/shaders/sun_light_no_shadow.ps.hlsl", "ps_5_0"},
        Shader{g_sun_light_no_shadow_sky, sizeof(g_sun_light_no_shadow_sky), "passes/shaders/sun_light_no_shadow_sky.ps.hlsl", "ps_5_0"}},
    {Shader{g_sun_light_shadow, sizeof(g_sun_light_shadow), "passes/shaders/sun_light_shadow.ps.hlsl", "ps_5_0"},
        Shader{g_sun_light_shadow_sky, sizeof(g_sun_light_shadow_sky), "passes/shaders/sun_light_shadow_sky.ps.hlsl", "ps_5_0"}}};

// [inside the shadow range]
inline constexpr Shader PathTraced[2]{
    Shader{g_sun_light_path_traced_cloud, sizeof(g_sun_light_path_traced_cloud), "passes/shaders/sun_light_path_traced_cloud.ps.hlsl", "ps_5_0"},
    Shader{g_sun_light_path_traced, sizeof(g_sun_light_path_traced), "passes/shaders/sun_light_path_traced.ps.hlsl", "ps_5_0"}};

#if PHANTOM_FX_DEV_TOOLS
inline constexpr Shader GameShadow{g_sun_light_game_shadow, sizeof(g_sun_light_game_shadow), "dev/shaders/sun_light_game_shadow.ps.hlsl", "ps_5_0"};
#endif
}
