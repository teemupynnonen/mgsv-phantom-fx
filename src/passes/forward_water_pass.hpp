#pragma once

#include "game_shaders.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>

// The game's rivers and liquids (GamePass::ForwardWater), drawn forward over the lit scene, with what the features put
// on them composed in (docs/DEVELOPMENT.md, "Forward surfaces: rivers, liquids and glass"): the rays take the game's
// shadow map and cube map out of the draw (water.hpp), the caustics draw over it (water_caustics.hpp), and the rays
// record it as a surface (water.hpp).
namespace phantom_fx::forward_water_pass
{
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
}
