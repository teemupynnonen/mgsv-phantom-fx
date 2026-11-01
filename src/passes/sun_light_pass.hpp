#pragma once

#include "game_shaders.hpp"
#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>

// The game's sun light pass (docs/DEVELOPMENT.md, "Sun light and shadows"): the engine lights the sun
// in one full-screen pass whose shaders sample the shadow map themselves. While a sun shadow effect
// runs, the pass draws with replacement shaders that compute the same with the effect added:
//
// - Soft shadows filter the sun's shadow map with contact-hardening soft shadows (PCSS): the penumbra
//   widens with the distance between the occluder and the receiver, at the sun's angular size.
// - The sun's contact shadows multiply in Bend Studio's screen-space shadow (sun_contact_shadows.hpp).
// - Ray-traced shadows take the sun's shadow from rays through the ray-traced scene
//   (ray_tracing::sunShadow; docs/DEVELOPMENT.md, "Ray-traced shadows"), everywhere the sun
//   lights. The shadow map keeps only what the rays leave out, the alpha-tested meshes' shadows.
//
// With none running the game draws its own shaders.
namespace phantom_fx::sun_light_pass
{
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Configuration& configuration) noexcept;
// The replacement shaders did not compile, so the game draws its own sun shadows (see PhantomFX.log).
bool failed() noexcept;
// For the budget: the soft shadows are a mode of the replacement shaders, and the ray-traced shadows
// reach the sun through them.
Health softShadowHealth();
Health rayTracedShadowHealth();

// ---- Recording, on the game's render and worker threads ----
// Before a draw: the sun pass, while a sun shadow effect runs, is drawn here instead; returns true
// when the game's draw must be skipped.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
// Each present. The game records a whole frame between presents, so all of a frame's sun passes draw
// with the same effects.
void present() noexcept;
}
