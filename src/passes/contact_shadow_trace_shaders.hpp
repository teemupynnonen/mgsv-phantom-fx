#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/contact_shadow_trace.hpp"

// HLSL for the screen-space shadow trace (contact_shadow_trace.hpp), precompiled at build time (shaders/).
namespace phantom_fx::contact_shadow_trace::shaders
{
inline constexpr Shader BendShadow{g_contact_shadow_trace, sizeof(g_contact_shadow_trace), "passes/shaders/contact_shadow_trace.cs.hlsl", "cs_5_0"};
}
