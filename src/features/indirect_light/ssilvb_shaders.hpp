#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/ssilvb_upsample.hpp"

// HLSL for SSILVB (ssilvb.hpp), precompiled at build time (shaders/).
namespace phantom_fx::ssilvb::shaders
{
inline constexpr Shader Upsample{g_ssilvb_upsample, sizeof(g_ssilvb_upsample), "features/indirect_light/shaders/ssilvb_upsample.cs.hlsl", "cs_5_0"};
}
