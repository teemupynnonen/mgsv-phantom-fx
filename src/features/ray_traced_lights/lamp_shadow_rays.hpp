#pragma once

#include <d3d11.h>
#include <string>

namespace phantom_fx::trace
{
class Trace;
}

// The lamps' shadow rays, a trace on the ray-traced scene (docs/DEVELOPMENT.md, "Ray-traced lights"): a ray-traced share
// of the lights a pixel, then a filter no wider than the penumbra (light_shadows.hlsl), the rays alone for DLSS Ray
// Reconstruction, which denoises itself. The light pass reads the result (local_lights.hpp).
namespace phantom_fx::lamp_shadow_rays
{
trace::Trace& trace() noexcept;

// The share of the chosen lights' light that reaches each pixel, this frame's kick's, at the render size: R16_FLOAT.
// Null when none were traced.
ID3D11ShaderResourceView* share(UINT width, UINT height) noexcept;
// Diagnostics: the strongest point and spot lights at the screen's centre and what blocks each, a few frames old; empty
// while the debug view is off. Logged by itself whenever those change.
std::string probe();
}
