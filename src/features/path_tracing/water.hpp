#pragma once

#include "ui_redirect.hpp"

#include <d3d11.h>

// Rivers and other liquids under the rays (docs/DEVELOPMENT.md, "Forward surfaces: rivers, liquids and glass"). They are drawn
// forward, after the G-buffer, so the rays know nothing of them: their draw leaves out what the rays took the place of, and
// forward_surfaces::record then keeps them as a surface for the second, short trace.
namespace phantom_fx::water
{
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;

// The game's draw of a river, with its shadow map left out where the rays took that shadow's place on the ground, and its cube
// map where they reflect it. True when they reflect it: then forward_surfaces::record follows.
bool drawRiver(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept;
}
