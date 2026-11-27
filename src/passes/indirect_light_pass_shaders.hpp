#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/indirect_show.hpp"
#include "phantom_shaders/indirect_show_light.hpp"
#include "phantom_shaders/indirect_show_scene.hpp"

// HLSL for the indirect light pass (indirect_light_pass.hpp), precompiled at build time (shaders/).
namespace phantom_fx::indirect_light_pass::shaders
{
inline constexpr Shader Show{g_indirect_show, sizeof(g_indirect_show), "passes/shaders/indirect_light_show.ps.hlsl", "ps_5_0"};
inline constexpr Shader ShowLight{g_indirect_show_light, sizeof(g_indirect_show_light), "passes/shaders/indirect_light_show_light.ps.hlsl", "ps_5_0"};
inline constexpr Shader ShowScene{g_indirect_show_scene, sizeof(g_indirect_show_scene), "passes/shaders/indirect_light_show_scene.ps.hlsl", "ps_5_0"};
}
