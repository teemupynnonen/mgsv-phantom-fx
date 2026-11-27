#pragma once

#include "d3d11_helpers.hpp"
#include "game_lights.hpp"
#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>

// The lamps' contact shadows (docs/DEVELOPMENT.md, "Screen-space shadows"), drawn by the local light
// pass (local_light_pass.hpp). A shadow-mapped point or spot light draws its volume twice, to mark the
// stencil and then to light. At the first, the add-on marches Bend-style contact shadows through the
// depth buffer towards the light, inside that draw, and writes them into the light targets' unused
// alpha; the second's light is then scaled by it. A march per light's own fits, where renodx-mgsv's
// runs in the light shader itself, because Phantom FX cannot replace every one of the game's 24 light
// shaders. One march per light, so it costs more where many lights are on screen.
namespace phantom_fx::lamp_contact_shadows
{
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Makes the shaders (precompiled; compiled from source in a dev build) while the effect runs, before its first frame.
void configure(const Configuration& configuration) noexcept;
// For the budget: unavailable when its shader did not compile, so the lights keep the game's shadows
// (see PhantomFX.log).
Health health();

// ---- Recording, on the game's render and worker threads ----
// A light's stencil-marking draw, with the game's blend: the game's draw, then the light's shadow into
// the light targets' alpha. Returns true when it drew the game's draw; false leaves the draw, and the
// light's shadows, to the game.
bool drawMarking(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_lights::Falloff falloff, const BoundBlend& gameBlend);
// For a light's lighting draw: `gameBlend` with the light scaled by the shadow its marking draw left
// on `context`, which this uses up. Null when it left none, or the blend is not the game's additive
// one: the light then keeps the game's shadows.
ID3D11BlendState* takeShadowedBlend(ID3D11DeviceContext* context, ID3D11BlendState* gameBlend);
// FinishCommandList on `context`.
void contextFinished(ID3D11DeviceContext* context) noexcept;
void present() noexcept;
}
