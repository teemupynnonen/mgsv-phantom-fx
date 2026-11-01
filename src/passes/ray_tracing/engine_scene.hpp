#pragma once

#include "game_constants.hpp"
#include "passes/ray_tracing/gi_inputs.hpp"
#include "passes/ray_tracing/scene_description.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The game's own scene for the ray tracing (docs/DEVELOPMENT.md, "Scene objects"): every rigid model
// the game has loaded, on screen or not, where it is and its triangles, read from the model
// visibility manager while the game's visibility tests run (nothing changes in it then), once a game
// frame; the object brush's vegetation and props; and from the object registry's model list the
// characters, every skinned model the game shows, in its pose, and the rigid models no visibility
// manager holds; the terrain from its heightmap.
namespace phantom_fx::engine_scene
{
// Hooks the visibility tests on the supported build, with MinHook initialised; returns what was
// done, for the log. Without the hooks the scene has none of the game's models.
std::string hook() noexcept;
// Snapshots are taken only while on.
void setActive(bool active) noexcept;

struct Snapshot
{
    bool instancesUpdated{}; // A newer list came: `instances` and `characters` replace the last, even when empty
    std::vector<scene_description::ModelInstance> instances;
    std::vector<scene_description::CharacterInstance> characters;
    // Models first seen since the last take, read from their files.
    std::vector<std::shared_ptr<const scene_description::Model>> newModels;
};

// On the render thread: the latest snapshot's instances and characters, when one came since the last
// call, and the new models, a few megabytes of model files a call at most.
// The characters take their bones as the frame's draws had them (`drawn`), where a draw found its
// character and mesh; the rest as they are now, at the kick.
void take(Snapshot& snapshot, const std::vector<gi_inputs::DrawnBones>& drawn);

// The renderer let these models go: their files are read again when the game places them again.
void forget(const std::vector<std::uint64_t>& hashes);

// The game's terrain as its terrain shaders draw it (NewTerrain_*; docs/DEVELOPMENT.md, "Scene
// objects"): the virtual heightmap's page table and height atlas (vertex shader t0, t1), the page
// table's converter (b4 c0, c1) and the world's scale and offset (b5 c10, c11).
struct TerrainInputs
{
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> pages, heights;
    std::array<float, 8> converter{};   // c0, c1
    std::array<float, 8> scaleOffset{}; // c10, c11
};

// Before a terrain draw of the main view: notes its inputs, once a game frame, with the object and material
// constants the game wrote for it (the material's buffer holds other draws' by the time the frame is lit).
void noteTerrain(ID3D11DeviceContext* context, const game_constants::ObjectConstants& object, const game_constants::TerrainMaterialConstants& material) noexcept;

// On the render thread: the inputs last noted, once each.
std::optional<TerrainInputs> takeTerrain();

// The game's loaded textures with these path hashes (docs/DEVELOPMENT.md, "Scene objects", the
// texture manager), each held so it outlives the game's own release; those not loaded are left out.
// On the render thread.
struct LoadedTexture
{
    std::uint64_t path{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
};
std::vector<LoadedTexture> loadedTextures(const std::vector<std::uint64_t>& paths);

// For the log.
std::string status();
// Diagnostics: since the last call, where the characters' bones came from.
std::string poseTiming();
}
