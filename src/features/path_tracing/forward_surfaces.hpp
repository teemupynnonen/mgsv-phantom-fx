#pragma once

#include "game_shaders.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

// The forward pass's surfaces under path tracing (docs/DEVELOPMENT.md, "Forward surfaces: rivers, liquids and glass"):
// glass, mirrors, rivers and liquids. They are drawn forward, after the G-buffer, so the kick's rays know nothing of
// them; and most are drawn among the particles, after the game's reflection blend. This module owns what they share:
//
// - the surface targets: the nearest surface's depth and normal at each pixel, and the weight the game's shader gives
//   its reflection, which glass and water record through `record`, with the shaders kept here for them (surface_shaders.hpp);
// - the second, short trace before the exposure's list (ray_tracing::traceSurfaces) and the composite that adds what
//   those pixels reflect over the scene at those weights (executing);
// - Ray Reconstruction's guides, which tell it where a surface dominates (guideReconstruction).
//
// So whatever the game draws reflects, its shapes and normals its own: a character's visor needs no pose from the rays.
// A frame without such a surface traces nothing.
namespace phantom_fx::forward_surfaces
{
// The textures the second trace reads and writes, each shared with Direct3D 12.
struct SurfaceTextures
{
    ID3D11Texture2D* depth{};      // R32_FLOAT, as the game's depth; 0 without a surface
    ID3D11Texture2D* normal{};     // R16G16B16A16_FLOAT: world space, the roughness in w; water's w is minus its share of what it reflects
    ID3D11Texture2D* reflection{}; // R16G16B16A16_FLOAT: what it reflects, in the scene's units; a: the ray's hit distance
};

void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
void present() noexcept;

// ---- Recording, on the game's render and worker threads ----
// Before a draw, whoever draws it: the exposure's draw ends the scene.
void note(ID3D11DeviceContext* context, game_shaders::DrawnShader& shader) noexcept;
// Whether the surface targets are this frame's size and the module has its pipeline: a draw should leave the game's
// reflection in place when they are not.
bool accepts(ID3D11DeviceContext* context) noexcept;
// The game's draw of `row`'s shader again, into the surface targets with the surface's own, in its depth test without writing;
// then the scene is what the composite adds to. False when the targets are not this frame's size, or the module has no pipeline.
bool record(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, const game_shaders::GameShader& row) noexcept;
// What `from` recorded now belongs to `into`: FinishCommandList, or a command list executed in a deferred context.
void recorded(std::uint64_t into, std::uint64_t from) noexcept;
void forget(std::uint64_t contextOrList) noexcept;
// The surfaces' weights so far at the render size, R16G16B16A16_FLOAT, for the game's reflection blend to leave them be
// (path_tracing::draw); null while off.
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> surfaceWeights(UINT width, UINT height) noexcept;

// ---- The immediate context ----
// At the kick (ray_tracing.cpp): the surface targets, empty for this frame's, at the render size; all null when unavailable.
SurfaceTextures beginFrame(ID3D11DeviceContext* immediate, UINT width, UINT height) noexcept;
// Before the immediate context executes `list`: the trace and what it adds, before the exposure's.
void executing(ID3D11DeviceContext* context, std::uint64_t list) noexcept;
// Before the upscaler's inputs, with Ray Reconstruction's guides to write into: the depth of the surface that dominates
// its pixels, 0 elsewhere, for the depth and motion it reads; null when no surface reflected this frame. Clears the
// compute state.
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> guideReconstruction(ID3D11DeviceContext* immediate, ID3D11UnorderedAccessView* normals,
    ID3D11UnorderedAccessView* hitDistance) noexcept;
}
