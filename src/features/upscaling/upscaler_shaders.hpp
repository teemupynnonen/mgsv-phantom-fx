#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/upscaler_blur_source.hpp"
#include "phantom_shaders/upscaler_composite.hpp"
#include "phantom_shaders/upscaler_inputs.hpp"
#include "phantom_shaders/upscaler_stretch.hpp"
#include "phantom_shaders/upscaler_stretch_vs.hpp"

// HLSL for DLSS or FSR before the game's grading, precompiled at build time (shaders/).
namespace phantom_fx::upscaler::shaders
{
inline constexpr Shader Inputs{g_upscaler_inputs, sizeof(g_upscaler_inputs), "features/upscaling/shaders/inputs.cs.hlsl", "cs_5_0"};
inline constexpr Shader Composite{g_upscaler_composite, sizeof(g_upscaler_composite), "features/upscaling/shaders/composite.ps.hlsl", "ps_5_0"};
inline constexpr Shader StretchVertex{g_upscaler_stretch_vs, sizeof(g_upscaler_stretch_vs), "features/upscaling/shaders/stretch.vs.hlsl", "vs_5_0"};
inline constexpr Shader Stretch{g_upscaler_stretch, sizeof(g_upscaler_stretch), "features/upscaling/shaders/stretch.ps.hlsl", "ps_5_0"};
inline constexpr Shader BlurSource{g_upscaler_blur_source, sizeof(g_upscaler_blur_source), "features/upscaling/shaders/blur_source.ps.hlsl", "ps_5_0"};
}
