#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/dof_composite.hpp"
#include "phantom_shaders/dof_coverage.hpp"
#include "phantom_shaders/dof_far_fill_fine.hpp"
#include "phantom_shaders/dof_far_fill_wide.hpp"
#include "phantom_shaders/dof_far_gather.hpp"
#include "phantom_shaders/dof_far_prefilter.hpp"
#include "phantom_shaders/dof_lens_composite.hpp"

// HLSL for the depth of field (depth_of_field.hpp), precompiled at build time (shaders/).
namespace phantom_fx::depth_of_field::shaders
{
inline constexpr Shader Coverage{g_dof_coverage, sizeof(g_dof_coverage), "features/depth_of_field/shaders/coverage.ps.hlsl", "ps_5_0"};
inline constexpr Shader FarPrefilter{g_dof_far_prefilter, sizeof(g_dof_far_prefilter), "features/depth_of_field/shaders/far_prefilter.ps.hlsl", "ps_5_0"};
inline constexpr Shader FarGather{g_dof_far_gather, sizeof(g_dof_far_gather), "features/depth_of_field/shaders/far_gather.ps.hlsl", "ps_5_0"};
inline constexpr Shader FarFillWide{g_dof_far_fill_wide, sizeof(g_dof_far_fill_wide), "features/depth_of_field/shaders/far_fill_wide.ps.hlsl", "ps_5_0"};
inline constexpr Shader FarFillFine{g_dof_far_fill_fine, sizeof(g_dof_far_fill_fine), "features/depth_of_field/shaders/far_fill_fine.ps.hlsl", "ps_5_0"};
inline constexpr Shader Composite{g_dof_composite, sizeof(g_dof_composite), "features/depth_of_field/shaders/composite.ps.hlsl", "ps_5_0"};
inline constexpr Shader LensComposite{g_dof_lens_composite, sizeof(g_dof_lens_composite), "features/depth_of_field/shaders/lens_composite.ps.hlsl", "ps_5_0"};
}
