#pragma once

#include "effects.hpp"
#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>

// The sunlight a river's waves focus on the bed under it (docs/DEVELOPMENT.md, "Forward surfaces: rivers, liquids and glass"):
// a web of light, drawn over each river right after its own draw. It reads only what that draw left bound, so it needs
// neither the rays nor the upscaler.
namespace phantom_fx::water_caustics
{
void attach(ID3D11Device* device) noexcept;
void configure(const Configuration& configuration) noexcept;
void present() noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
Health health();

// On the game's recording threads, right after a river's own draw (forward_water_pass).
void draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept;
}
