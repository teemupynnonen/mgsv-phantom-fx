#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/ssgi_accumulate.hpp"
#include "phantom_shaders/ssgi_filter.hpp"
#include "phantom_shaders/ssgi_gather.hpp"
#include "phantom_shaders/ssgi_resolve.hpp"

// HLSL for VB-SSGI (ssgi.hpp), precompiled at build time (shaders/, with ssgi_common.hlsli shared).
namespace phantom_fx::ssgi::shaders
{
inline constexpr Shader Gather{g_ssgi_gather, sizeof(g_ssgi_gather), "features/indirect_light/shaders/ssgi_gather.cs.hlsl", "cs_5_0"};
inline constexpr Shader Accumulate{g_ssgi_accumulate, sizeof(g_ssgi_accumulate), "features/indirect_light/shaders/ssgi_accumulate.cs.hlsl", "cs_5_0"};
inline constexpr Shader Filter{g_ssgi_filter, sizeof(g_ssgi_filter), "features/indirect_light/shaders/ssgi_filter.cs.hlsl", "cs_5_0"};
inline constexpr Shader Resolve{g_ssgi_resolve, sizeof(g_ssgi_resolve), "features/indirect_light/shaders/ssgi_resolve.cs.hlsl", "cs_5_0"};
}
