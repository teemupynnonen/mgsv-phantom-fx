#pragma once

#include "game_constants.hpp"
#include "game_shaders.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
#include <vector>

// What the ray tracing takes from the game's draws (docs/DEVELOPMENT.md, "Global illumination"): the
// main camera the frame was drawn with, its skinned meshes' bones and its terrain's inputs
// (engine_scene::noteTerrain), from the constants the game wrote for them (constant_capture.hpp). The
// scene itself comes from the game's memory (engine_scene.hpp).
namespace phantom_fx::gi_inputs
{
// Releases what it holds; only when no Direct3D work can run.
void detach() noexcept;
// Inputs are noted only while on; the constant capture keeps all the ray tracing reads.
void setActive(bool active) noexcept;

// ---- Recording, on the game's render and worker threads ----
void vertexShaderBound(ID3D11DeviceContext* context, game_shaders::VertexShader kind) noexcept;
// Before an indexed draw of the game's models or terrain: notes the camera and the terrain's inputs.
void drawIndexed(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept;
// What `from` noted now belongs to `into`: FinishCommandList, or a command list executed in a deferred
// context.
void recorded(std::uint64_t into, std::uint64_t from) noexcept;
void forget(std::uint64_t contextOrList) noexcept;

// ---- The immediate context ----
// Before the immediate context executes `list`.
void executing(std::uint64_t list) noexcept;
// At present: the cameras no list handed over expire; how many contexts' or lists' did.
std::size_t present() noexcept;

// At the kick: the main camera's scene constants (jittered) of the draws executed since the last call;
// none when no such draw ran.
std::optional<game_constants::SceneConstants> takeCamera() noexcept;

// A skinned mesh as the main camera's draw posed it: where its model is in the world, which of its
// meshes it is (the draw's index range), and the bones it drew with (game_constants::BonesConstants'
// bones).
struct DrawnBones
{
    std::array<float, 3> position{};
    std::array<float, 3> eye{}; // The draw's camera, for the frame it belongs to
    std::uint32_t firstIndex{}, indexCount{};
    std::array<float, 32 * 12> bones{};
};
// At the kick: the skinned meshes drawn for the frame `camera` draws since the last call.
std::vector<DrawnBones> takeDrawnBones(const game_constants::SceneConstants& camera) noexcept;

// A terrain patch as the main camera's terrain draw drew it (scene_contents::TerrainPatchDraw): its rect of the virtual
// heightmap (cb5 c9), the index range of the patch mesh it drew, and the draw's camera.
struct DrawnPatch
{
    std::array<float, 4> uvScaleOffset{};
    std::array<float, 3> worldScale{}, worldOffset{}; // cb5 c10, c11
    std::uint32_t firstIndex{}, indexCount{};
    std::int32_t baseVertex{};
    std::array<float, 3> eye{};
};
// At the kick: the terrain patches drawn for the frame `camera` draws since the last call.
std::vector<DrawnPatch> takeDrawnPatches(const game_constants::SceneConstants& camera) noexcept;

// The buffers the terrain's patch draws read their mesh from, as the last one bound them, with their formats: the
// vertices' from the input layout (game_shaders::positionFormatOf).
struct PatchMeshSource
{
    Microsoft::WRL::ComPtr<ID3D11Buffer> indices, vertices;
    DXGI_FORMAT indexFormat{}, vertexFormat{};
    UINT indexOffset{}, vertexStride{}, vertexOffset{};

    bool operator==(const PatchMeshSource&) const = default;
};
std::optional<PatchMeshSource> patchMeshSource() noexcept;

// Diagnostics (dev builds): where the main camera's rigid model draws put their models, and which mesh (its index
// range), recorded from recordRigidDraws(true) until takeRigidDraws.
struct RigidDraw
{
    std::array<float, 3> position{};
    std::uint32_t firstIndex{}, indexCount{};
};
void recordRigidDraws(bool on) noexcept;
std::vector<RigidDraw> takeRigidDraws() noexcept;

// Diagnostics: the draw hook's time on the game's threads, all threads together, and the draws it
// looked at, since the last call.
struct Work
{
    double hookMilliseconds{};
    std::uint64_t draws{};
};
Work takeWork() noexcept;
}
