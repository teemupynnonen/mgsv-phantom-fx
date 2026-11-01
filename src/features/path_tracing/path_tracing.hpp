#pragma once

#include "features/ray_traced_lights/local_lights.hpp"
#include "game_shaders.hpp"
#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <span>
#include <utility>

// Path tracing on Direct3D 11 (docs/DEVELOPMENT.md, "Path tracing"). While DLSS Ray Reconstruction denoises
// and the global illumination runs, the ray-traced scene's kick traces the reflections and the point and spot
// lights too (path_tracing_rays.hpp), and the game's own make way for them:
//
// - the resolve adds the reflections to the game's specular light (addReflections), and the rain's resolve
//   leaves its sky's reflection out (indirect_light_pass.cpp);
// - the sun pass is lit by the rays' shadow alone and leaves the sky's reflection out, and indoors the sky's
//   specular pass is skipped (sun_light_pass.cpp);
// - the game's blend of its screen-space reflections takes the rays' hits in their place, but where a surface was
//   drawn in front of the G-buffer's: water, which the rays do not see, keeps them, and glass has its own (glass.hpp) (draw);
// - the lamps' light is added once at the draw point, in place of each traced light drawn again
//   (addLampLight, local_lights.cpp).
namespace phantom_fx::path_tracing
{
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Configuration& configuration) noexcept;
void present() noexcept;
// For the budget: waiting for Ray Reconstruction, or unavailable.
Health health();
// The reflections take the place of the game's: the last kick traced them, and the sun pass can leave its sky reflection
// out. On any thread.
bool isReflecting() noexcept;
// The menu's strength of the reflections.
float reflectionStrength() noexcept;
// The menu's roughness of the water's traced reflection (features/water_reflections).
float waterReflectionRoughness() noexcept;

// ---- Recording, on the game's render and worker threads ----
// The resolve (indirect_light_pass.cpp): the game's `specular` light with the traced reflections added, at
// `size`; null keeps the game's.
ID3D11ShaderResourceView* addReflections(ID3D11DeviceContext* context, ID3D11ShaderResourceView* specular, std::pair<UINT, UINT> size);
// Before a draw, whoever draws it: the scene's depth the screen-space reflections' mask reads.
void note(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
// The game's blend of its reflections over the scene, drawn here while path tracing runs; returns true when the
// game's draw must be skipped.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;

// ---- The immediate context ----
// At the kick (ray_tracing.cpp): the masked spots' masks (TracedLight::mask) into the atlas the lamps' rays
// sample, each mask once, a tile each, given to their lights (light_choice::LocalLight::maskTile). Past
// GI_LAMP_MASK_TILES masks, a spot is lit unmasked (logged). Null without masks.
ID3D11Texture2D* packMasks(ID3D11DeviceContext* immediate, std::span<local_lights::TracedLight> lights) noexcept;
// At the draw point (local_lights.cpp): the lamps' light the last kick traced (ray_tracing::lampLight) added
// to the light targets, the diffuse's and the specular's; false while path tracing does not light them.
bool addLampLight(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* diffuse, ID3D11RenderTargetView* specular) noexcept;
}
