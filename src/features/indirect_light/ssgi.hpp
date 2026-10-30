#pragma once

#include "passes/visibility_bitmask.hpp"
#include "phantom_fx_settings.hpp"

#include <d3d11.h>
#include <string>

// SSGI, screen-space global illumination (docs/DEVELOPMENT.md, "SSGI"), adds the light that nearby
// surfaces on screen reflect onto each other. It is the RenoDX MGSV add-on's indirect lighting
// (renodx-mgsv src/games/mgsv/il) ported to Direct3D 11: a gather of the neighbouring light at the
// gather's resolution, temporal accumulation against the game's velocity buffer, an a-trous
// denoiser, and a joint-bilateral resolve. At the resolve, once the sun and the local lights are
// in the diffuse light, the resolve reads the diffuse light with that bounce added
// (indirect_light_pass.hpp).
namespace phantom_fx::ssgi
{
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Configuration& configuration) noexcept;
// For the budget (fine while off).
Health health();
void present() noexcept;

// ---- Recording, on the game's render and worker threads (indirect_light_pass.cpp) ----
// The game's diffuse light with SSGI's added, into `lit` at the render size, from what the resolve
// binds. `velocity` is the game's velocity buffer for this frame, or null; without it the temporal
// accumulation collapses to the raw gather. False leaves the resolve the game's diffuse light this
// frame.
bool compute(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* normals, ID3D11ShaderResourceView* diffuse,
    ID3D11ShaderResourceView* albedo, ID3D11ShaderResourceView* specular, ID3D11ShaderResourceView* velocity, ID3D11Buffer* scene,
    visibility_bitmask::Target& lit);
}  // namespace phantom_fx::ssgi
