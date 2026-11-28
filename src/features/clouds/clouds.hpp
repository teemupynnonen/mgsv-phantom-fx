#pragma once

#include "effects.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>

namespace phantom_fx
{
struct Configuration;
}
namespace phantom_fx::game_shaders
{
class DrawnShader;
}

// Better clouds (docs/DEVELOPMENT.md, "Clouds"): tpp3dfw_generativeClouds's draw, painted by a raymarch of the
// add-on's own with the game's vertex shader, bindings and weather, in the place of its four-step shader.
namespace phantom_fx::clouds
{
void attach(ID3D11Device* device) noexcept;
void configure(const Configuration& configuration) noexcept;
void detach() noexcept;

// The ground's cloud shadow (cloud_shadow.hlsl), R8_UNORM and shared with Direct3D 12 so the rays read it: null while the clouds are
// off or before the first sky draw. It covers a square of ShadowExtent metres centred on the eye snapped to a texel, indexed by where
// a point's ray to the sun crosses the height LayerBase (cloud_density.hlsli).
constexpr float ShadowExtent = 30000.0f, ShadowSize = 1024.0f, LayerBase = 900.0f;
ID3D11Texture2D* shadowTexture() noexcept;
// For the budget.
Health health();

// The cloud draw: true when it was drawn here, with the game's own pixel shader put back.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
}
