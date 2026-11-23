#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/sea_fft.hpp"
#include "phantom_shaders/sea_fft_columns.hpp"
#include "phantom_shaders/sea_maps.hpp"
#include "phantom_shaders/sea_spectrum.hpp"
#include "phantom_shaders/sea_surface_ps.hpp"
#include "phantom_shaders/sea_surface_vs.hpp"

// The sea's shaders (sea.hpp), precompiled at build time (shaders/). The waves are Tessendorf's
// ("Simulating Ocean Water", 2001), the spectrum JONSWAP's, and the surface a set of
// geometry clipmaps around the eye (see shaders/sea_common.hlsli).
namespace phantom_fx::sea::shaders
{
inline constexpr Shader Spectrum{g_sea_spectrum, sizeof(g_sea_spectrum), "features/sea/shaders/spectrum.cs.hlsl", "cs_5_0"};
inline constexpr Shader Fft{g_sea_fft, sizeof(g_sea_fft), "features/sea/shaders/fft.cs.hlsl", "cs_5_0"};
inline constexpr Shader FftColumns{g_sea_fft_columns, sizeof(g_sea_fft_columns), "features/sea/shaders/fft_columns.cs.hlsl", "cs_5_0"};
inline constexpr Shader Maps{g_sea_maps, sizeof(g_sea_maps), "features/sea/shaders/maps.cs.hlsl", "cs_5_0"};
inline constexpr Shader SurfaceVertex{g_sea_surface_vs, sizeof(g_sea_surface_vs), "features/sea/shaders/surface_vs.hlsl", "vs_5_0"};
inline constexpr Shader SurfacePixel{g_sea_surface_ps, sizeof(g_sea_surface_ps), "features/sea/shaders/surface_ps.hlsl", "ps_5_0"};
}
