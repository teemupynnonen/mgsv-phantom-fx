#pragma once

#include "game_shaders.hpp"
#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <string>

// Volumetric fog with light shafts (docs/DEVELOPMENT.md, "Volumetric fog"). Fox Engine computes its
// fog into a 512x256 texture at the start of each frame: 128 slices of view depth, each a 16x64 tile
// of view directions holding the fog from the camera to that depth, by an analytic formula that no
// shadow touches. The resolves, the sky, the clouds and every transparent draw read it.
//
// The add-on makes that texture larger, in the same layout, so all of them read finer fog unchanged.
// The game's fog pass fills all of it. With volumetric fog on, the add-on fills it again at the
// resolve, once the sun's shadow map is drawn: the game's formula slice by slice, with the sun's
// share shadowed, which gives light shafts. The physical look fills it with Earth's air and a
// height fog instead, and draws the sky from the same atmosphere in place of the game's sky.
namespace phantom_fx::atmosphere
{
// The immediate context is for the sky parameters the diagnostic log reads at present.
void attach(ID3D11Device* device, ID3D11DeviceContext* immediate) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Configuration& configuration) noexcept;
// For the budget (fine while off).
Health health();

// ---- Creation ----
// A 2D texture about to be made: the game's fog texture becomes larger, with unordered access.
// Returns true when the size changed; textureCreated follows on the same thread.
bool upgradeTexture(UINT& width, UINT& height, DXGI_FORMAT format, bool renderTarget) noexcept;
void textureCreated(ID3D11Resource* texture) noexcept;
void resourceDestroyed(ID3D11Resource* resource) noexcept;

// ---- Recording, on the game's render and worker threads ----
// Before a draw, whoever draws it: the sun pass inside the shadow range, whose shadow map the fill reads,
// and before the resolve the fill of the fog texture.
void note(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
// The fog pass, and with the physical look the sky. Returns true when the add-on made the draw itself,
// so the game's draw must be skipped.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
// Each present. The game records a whole frame between presents. While the physical look runs, the
// diagnostic log gets the sky's parameters every ten seconds.
void present() noexcept;

// The physical sky's parameters (shaders/atmosphere.hlsli, SkyParameters) for the clouds to be lit by the same sun and sky, or null
// while the physical look is off. On the render thread, after this frame's fog pass.
ID3D11ShaderResourceView* skyParametersView() noexcept;
}
