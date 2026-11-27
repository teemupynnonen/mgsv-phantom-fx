#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/ui_redirect_composite_behind.hpp"
#include "phantom_shaders/ui_redirect_stretch_depth.hpp"
#include "phantom_shaders/ui_redirect_whole_target.hpp"

// HLSL for the UI at the output size (ui_redirect.cpp), precompiled at build time (passes/shaders/).
namespace phantom_fx::ui_redirect::shaders
{
inline constexpr Shader WholeTarget{g_ui_redirect_whole_target, sizeof(g_ui_redirect_whole_target),
    "passes/shaders/ui_redirect_whole_target.vs.hlsl", "vs_5_0"};
inline constexpr Shader StretchDepth{g_ui_redirect_stretch_depth, sizeof(g_ui_redirect_stretch_depth),
    "passes/shaders/ui_redirect_stretch_depth.ps.hlsl", "ps_5_0"};
inline constexpr Shader CompositeBehind{g_ui_redirect_composite_behind, sizeof(g_ui_redirect_composite_behind),
    "passes/shaders/ui_redirect_composite_behind.ps.hlsl", "ps_5_0"};
}
