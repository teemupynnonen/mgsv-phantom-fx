#pragma once

#include "ui_redirect.hpp"

#include <d3d11.h>

// A river's own light under the global illumination (docs/DEVELOPMENT.md, "Forward surfaces: rivers, liquids and glass"). The
// game's river shaders light the water with the sky's hemisphere ambient (cPSLight's first two rows), which the global
// illumination never reaches: under a forest's canopy the ground took the traced light, darker, and the water kept the open
// sky's, milky over a dark bed. The river's colour is linear in that ambient, so its draw is made twice more aside, with the
// ambient and without, and the scene loses what the ambient would lose where the traced light is below the game's probe light
// at the bed (light_combine::ambient).
namespace phantom_fx::river_light
{
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;

// On the game's recording threads, right after a river's own draw, with what it had bound (water::drawRiver).
void draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept;

// Diagnostics (the automation's `diagnostic river_light`): off leaves the river as the game lights it.
void setEnabled(bool enabled) noexcept;
}
