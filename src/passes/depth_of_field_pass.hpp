#pragma once

#include "game_shaders.hpp"
#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>

// The game's depth of field with its focus edges at the render size, and a lens's blur behind the focus
// (docs/DEVELOPMENT.md, "Depth of field"). The game decides what is blurred per half-size pixel: one
// background pixel of the four makes all four blurred. The steps that leaves stay put as the upscaler's
// jitter moves the image, so the upscaler keeps them, a half-size pixel wide: 3.4 output pixels at
// Balanced at 4K. Here the half-size layers give the blur's colour and each render pixel's own depth
// decides how much of it shows, which moves with the jitter as any other edge does.
namespace phantom_fx::depth_of_field_pass
{
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// The background's blur, from the settings.
void configure(const Configuration& configuration) noexcept;

// ---- Recording, on the game's render and worker threads ----
// Before a draw: notes each pixel's cover at the depth of field's first pass, keeps its far layer apart
// and composites both. Returns true when the game's draw must be skipped.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
}
