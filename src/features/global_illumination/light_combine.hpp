#pragma once

#include "passes/visibility_bitmask.hpp"
#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <optional>
#include <utility>

// The global illumination's light in the game's (docs/DEVELOPMENT.md, "Global illumination"): the
// upscale also draws the game's probe light aside, and at the resolve the traced light takes its
// place in the diffuse light (indirect_light_pass.hpp). The game's light target keeps the probe
// light, as the game's exposure (Tonemap_Lum_Lacc) measures that target: without it there, the
// exposure ran away in the shade (a white screen).
namespace phantom_fx::light_combine
{
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Configuration& configuration) noexcept;
void present() noexcept;
// The global illumination's, for the budget (fine while off): the ray-traced scene's frame, then the
// combine at the resolve.
Health health();

// ---- Recording, on the game's render and worker threads (indirect_light_pass.cpp) ----
// The upscale, drawn aside into the probe light's target first; the game then draws it as always.
void drawProbeLightAside(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call);
// The game's diffuse light with the traced `light` in place of its probe light, into `lit` at
// `size`. False leaves the resolve the game's light this frame.
bool combine(ID3D11DeviceContext* context, ID3D11ShaderResourceView* diffuse, ID3D11ShaderResourceView* light, std::pair<UINT, UINT> size,
    visibility_bitmask::Target& lit);

// What the last combine read, for the forward draws after it (river_light.hpp): the traced light at its strength in place of
// the game's probe light, both at the render size. None when the combine has not run in the last few frames.
struct Ambient
{
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> traced, probe;
    float strength{};
};
std::optional<Ambient> ambient();
}
