#pragma once

#include "game_shaders.hpp"
#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <array>

// The forward pass's reflective surfaces and the rays' shadow. Rivers, liquids and glass blend over the lit scene
// and would sample the game's shadow map alone, whose shadow the rays' replaced on the ground. At such a draw the
// map's shadow is left out, so the surface is not shadowed by a map that no longer matches the scene; the water
// takes no sun shadow of its own (its reflection is what the eye reads), and the bed under it is lit by the sun
// pass as the rest of the scene is. The draw is also where the water is recorded as a surface for the traced
// reflections (water::recordWater).
namespace phantom_fx::forward_shadows
{
void configure(const Configuration& configuration) noexcept;
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;

// A forward surface drawn by another module (glass) leaves the game's shadow map out the same way: the 1x1
// white map shadow and the sampler that always passes, for its shadow slot and s15. False when unavailable.
bool tracedShadowResources(ID3D11DeviceContext* context, ID3D11ShaderResourceView** whiteMap, ID3D11SamplerState** alwaysPass) noexcept;

// For the caustics the forward pass puts on the bed under the water: the render size and the frame's index (the
// pattern's time). The kick fills it; the camera is the water draw's own, so a frame's lag here cannot move the
// pattern.
struct Cameras
{
    float dimensions[2];
    float frame;
};
void setCameras(const Cameras& cameras) noexcept;

}
