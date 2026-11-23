#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/gtvbao_apply.hpp"

// HLSL for GTVBAO (gtvbao.hpp), precompiled at build time (shaders/gtvbao_apply.ps.hlsl).
namespace phantom_fx::gtvbao::shaders
{
inline constexpr Shader Apply{g_gtvbao_apply, sizeof(g_gtvbao_apply), "features/ambient_occlusion/shaders/gtvbao_apply.ps.hlsl", "ps_5_0"};
}
