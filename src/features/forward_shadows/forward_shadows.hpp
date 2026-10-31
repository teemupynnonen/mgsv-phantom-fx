#pragma once

#include "game_shaders.hpp"
#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <array>

// The sun's ray-traced shadow on the forward pass's reflective surfaces. Rivers, liquids and glass blend
// over the lit scene and sample the game's shadow map alone, so their water kept the map's shadow while the
// rays' lit the ground (docs/DEVELOPMENT.md, "Forward shadows"). At such a draw the map's shadow is left
// out and the pixels the surface drew are multiplied by the rays' shadow.
namespace phantom_fx::forward_shadows
{
void configure(const Configuration& configuration) noexcept;
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;

// A forward surface drawn by another module (glass) leaves the game's shadow map out the same way: the 1x1
// white map shadow and the sampler that always passes, for its shadow slot and s15. False when unavailable.
bool tracedShadowResources(ID3D11DeviceContext* context, ID3D11ShaderResourceView** whiteMap, ID3D11SamplerState** alwaysPass) noexcept;

}
