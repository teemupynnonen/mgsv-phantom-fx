#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/lamp_contact_shadow.hpp"

// HLSL for the lamps' contact shadows (lamp_contact_shadows.hpp), precompiled at build time (shaders/).
namespace phantom_fx::lamp_contact_shadows::shaders
{
inline constexpr Shader LightShadow{g_lamp_contact_shadow, sizeof(g_lamp_contact_shadow), "features/lamp_contact_shadows/shaders/light_shadow.ps.hlsl", "ps_5_0"};
}
