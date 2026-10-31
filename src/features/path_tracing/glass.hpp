#pragma once

#include "game_shaders.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

// Glass and mirrors under path tracing (docs/DEVELOPMENT.md, "Path tracing", "Glass and mirrors"). They are drawn forward,
// after the G-buffer, so the kick's rays know nothing of them; and most are drawn among the particles, after the game's
// reflection blend. While the reflections are traced:
//
// - each of their draws leaves out what the game reflects with, a small cube map or a mirror's painted picture, and is
//   drawn again into the surface targets: the nearest surface's depth and normal at each pixel, and the weight the
//   game's shader gives its reflection (draw);
// - before the exposure's list, the first after the scene, a second short trace reflects those pixels
//   (ray_tracing::traceSurfaces), and what they reflect is added over the scene at those weights (executing);
// - Ray Reconstruction is told of the glass where it dominates, or it moves the pane's detail as what is behind it
//   (guideReconstruction).
//
// So whatever the game draws reflects, its shapes and normals its own: a character's visor needs no pose from the rays.
// A frame without glass traces nothing.
namespace phantom_fx::glass
{
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
void present() noexcept;

// ---- Recording, on the game's render and worker threads ----
// Before a draw, whoever draws it: the exposure's draw ends the scene.
void note(ID3D11DeviceContext* context, game_shaders::DrawnShader& shader) noexcept;
// A glass or mirror draw, drawn here while the reflections are traced; returns true when the game's draw must be skipped.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
// A river or another liquid, after its own draw (forward_shadows): the surface into the same targets glass uses, so
// the surface trace reflects it too. Its cube map is already bound black by the caller.
void recordWater(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept;
// What `from` recorded now belongs to `into`: FinishCommandList, or a command list executed in a deferred context.
void recorded(std::uint64_t into, std::uint64_t from) noexcept;
void forget(std::uint64_t contextOrList) noexcept;
// The surfaces' weights so far at the render size, R16G16B16A16_FLOAT, for the game's reflection blend to leave them be
// (path_tracing::draw); null while off.
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> surfaceWeights(UINT width, UINT height) noexcept;

// ---- The immediate context ----
// The textures the second trace reads and writes, each shared with Direct3D 12.
struct SurfaceTextures
{
    ID3D11Texture2D* depth{};      // R32_FLOAT, as the game's depth; 0 without a surface
    ID3D11Texture2D* normal{};     // R16G16B16A16_FLOAT: world space, the roughness in w
    ID3D11Texture2D* reflection{}; // R16G16B16A16_FLOAT: what it reflects, in the scene's units; a: the ray's hit distance
};
// At the kick (ray_tracing.cpp): the surface targets, empty for this frame's, at the render size; all null when unavailable.
SurfaceTextures beginFrame(ID3D11DeviceContext* immediate, UINT width, UINT height) noexcept;
// Before the immediate context executes `list`: the trace and what it adds, before the exposure's.
void executing(ID3D11DeviceContext* context, std::uint64_t list) noexcept;
// Before the upscaler's inputs, with Ray Reconstruction's guides to write into: the depth of the glass that dominates its
// pixels, 0 elsewhere, for the depth and motion it reads; null when no glass reflected this frame. Clears the compute state.
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> guideReconstruction(ID3D11DeviceContext* immediate, ID3D11UnorderedAccessView* normals,
    ID3D11UnorderedAccessView* hitDistance) noexcept;
}
