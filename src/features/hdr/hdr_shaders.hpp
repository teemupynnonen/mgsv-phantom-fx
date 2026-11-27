#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/hdr_capture.hpp"
#include "phantom_shaders/hdr_decode.hpp"
#include "phantom_shaders/hdr_encode.hpp"
#include "phantom_shaders/hdr_grade.hpp"
#include "phantom_shaders/hdr_grade_upgrade.hpp"
#include "phantom_shaders/hdr_output.hpp"
#include "phantom_shaders/hdr_reduce.hpp"

// HLSL for HDR output (hdr.hpp), precompiled at build time (shaders/).
namespace phantom_fx::hdr::shaders
{
inline constexpr Shader Capture{g_hdr_capture, sizeof(g_hdr_capture), "features/hdr/shaders/capture.cs.hlsl", "cs_5_0"};
inline constexpr Shader Encode{g_hdr_encode, sizeof(g_hdr_encode), "features/hdr/shaders/encode.cs.hlsl", "cs_5_0"};
inline constexpr Shader Decode{g_hdr_decode, sizeof(g_hdr_decode), "features/hdr/shaders/decode.cs.hlsl", "cs_5_0"};
inline constexpr Shader Reduce{g_hdr_reduce, sizeof(g_hdr_reduce), "features/hdr/shaders/reduce.ps.hlsl", "ps_5_0"};
inline constexpr Shader Grade{g_hdr_grade, sizeof(g_hdr_grade), "features/hdr/shaders/grade.ps.hlsl", "ps_5_0"};
inline constexpr Shader GradeUpgrade{g_hdr_grade_upgrade, sizeof(g_hdr_grade_upgrade), "features/hdr/shaders/grade_upgrade.ps.hlsl", "ps_5_0"};
inline constexpr Shader Output{g_hdr_output, sizeof(g_hdr_output), "features/hdr/shaders/output.ps.hlsl", "ps_5_0"};
}
