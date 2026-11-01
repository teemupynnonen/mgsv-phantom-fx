#pragma once

#include "game_shaders.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>

// Rivers and other liquids under path tracing (docs/DEVELOPMENT.md, "Forward surfaces: rivers, liquids and glass"). They
// are drawn forward, after the G-buffer and after the upscaler, so neither the rays nor their denoisers see them. Their
// draw is recorded here as a surface - the same targets glass and mirrors use (forward_surfaces) - so the second, short
// trace reflects them after the scene, from where the water is now.
namespace phantom_fx::water
{
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;

// A river or another liquid, after its own draw (forward_shadows): the surface into the targets the surface trace reads.
// Its cube map is already bound black by the caller. `row` says how its shader builds its ripples' normal, which the
// surface takes as its own.
void recordWater(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, const game_shaders::GameShader& row) noexcept;
}
