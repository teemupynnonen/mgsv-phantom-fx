#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/visibility_denoise_occlusion.hpp"
#include "phantom_shaders/visibility_denoise_occlusion_final.hpp"
#include "phantom_shaders/visibility_march.hpp"
#include "phantom_shaders/visibility_prefilter.hpp"

// HLSL for the visibility bitmask march (visibility_bitmask.hpp), precompiled at build time (shaders/).
// The occlusion is ported from three-gtvbao (norio, MIT; GT-VBAO by Mirko Salm, CC0 / MIT; SSRT3 by
// Olivier Therrien, MIT), the noise and the denoiser from XeGTAO (Intel, MIT). Their licenses are in
// licenses/.
namespace phantom_fx::visibility_bitmask::shaders
{
inline constexpr Shader PrefilterDepths{g_visibility_prefilter, sizeof(g_visibility_prefilter), "passes/shaders/visibility_prefilter.cs.hlsl", "cs_5_0"};

inline constexpr Shader March{g_visibility_march, sizeof(g_visibility_march), "passes/shaders/visibility_march.cs.hlsl", "cs_5_0"};

// The denoiser before its last pass, and the last pass with its wider blur.
inline constexpr Shader Denoise[2]{
    Shader{g_visibility_denoise_occlusion, sizeof(g_visibility_denoise_occlusion), "passes/shaders/visibility_denoise_occlusion.cs.hlsl", "cs_5_0"},
    Shader{g_visibility_denoise_occlusion_final, sizeof(g_visibility_denoise_occlusion_final),
        "passes/shaders/visibility_denoise_occlusion_final.cs.hlsl", "cs_5_0"}};
}
