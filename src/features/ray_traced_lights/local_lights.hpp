#pragma once

#include "features/ray_traced_lights/light_choice.hpp"
#include "game_constants.hpp"
#include "game_shaders.hpp"
#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

// Ray-traced lights (docs/DEVELOPMENT.md, "Ray-traced lights"): the game's point and spot lights, the
// flashlights among them, shadowed by rays through the ray-traced scene (ray_tracing.hpp) in
// place of their shadow maps or of none. A light's draws keep their stencil work but add no light;
// Direct3D 12 traces its shadow, and at the sun pass's list, which waits for that, it is drawn again
// with the game's own shader for its kind of light with specular and without a shadow map, scaled by
// its shadow, within the volume (a box) its draws had.
namespace phantom_fx::local_lights
{
void attach(ID3D11Device* device, ID3D11DeviceContext* immediate) noexcept;
// Releases everything; only when no Direct3D work can run.
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Configuration& configuration) noexcept;
// For the budget (fine while off): while it is not fine, the lights stay the game's.
Health health();

// ---- Recording, on the game's render and worker threads ----
// Before an indexed draw. Returns true when it made a light's draw itself, so the game's is skipped.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
// indirect_light calls this at a pass that comes after the game's lights and before anything reads them:
// the sun pass, indoors the sky's specular pass in its place, or the resolve, as some scenes draw no sun
// pass at all. Each list that has one draws the lights recorded before it again.
void noteDrawPoint(ID3D11DeviceContext* context) noexcept;
// What `from` recorded now belongs to `into`: FinishCommandList, or a command list executed in a
// deferred context.
void recorded(std::uint64_t into, std::uint64_t from) noexcept;
void forget(std::uint64_t contextOrList) noexcept;

// ---- Immediate context ----
// A light for the rays, with a masked spot's mask (the game's t4) for path tracing.
struct TracedLight
{
    light_choice::LocalLight light;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mask;
};
// At the kick: the chosen lights (light_choice::choose) among those recorded and not traced yet, as the
// rays take them; those given are drawn with the rays' shadow.
std::vector<TracedLight> takeForTrace(const game_constants::SceneConstants& camera) noexcept;
// Before the immediate context executes `list`, after the global illumination's wait: at each list with
// a draw point (noteDrawPoint), the lights of the lists run so far are drawn.
void executing(ID3D11DeviceContext* context, std::uint64_t list) noexcept;
void present() noexcept;
}
