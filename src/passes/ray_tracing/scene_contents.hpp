#pragma once

#include "passes/ray_tracing/scene_description.hpp"
#include "passes/ray_tracing/scene_memory.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// The scene contents (docs/DEVELOPMENT.md, "Ray-traced scene"): the game's models, characters, terrain
// and textures the ray-traced scene holds, made as the scene memory has room, and when each is let go;
// and each frame's instances of them. Everything here runs on the game's render thread.
namespace phantom_fx::scene_contents
{
using gi::GIEmitter;
using gi::GISkinRecord;
using gi::uint4;
using scene_description::CharacterInstance;
using scene_description::Model;
using scene_description::ModelInstance;
using scene_description::ModelPart;
using scene_description::Transform;

// What the game's scene gave this frame.
struct GameScene
{
    std::uint32_t frameIndex{};
    std::array<float, 3> eye{}; // The self-lit meshes nearest it are the rays' to aim at
    // The models first seen since the last frame.
    std::span<const std::shared_ptr<const Model>> newModels;
    // A newer list came: `placements` and `characters` replace the last, even when empty. Without one
    // the last placements and poses stand.
    bool hasNewLists{};
    std::span<const ModelInstance> placements;
    std::span<const CharacterInstance> characters;
    // Copied, or copied again as the game streamed more of it in, since the last frame.
    std::span<const SceneTexture> newTextures;
    // Where the terrain rings centre, once the game's terrain draws give its inputs; without it the
    // terrain is left out of the frame.
    std::optional<std::array<float, 3>> terrainEye;
    // The sea's level under the eye (sea.hpp), where its plane lies this frame; without it the frame has no sea.
    std::optional<std::array<float, 3>> seaCentre;
    // Diagnostics (the probes): the frame's instances of the game's models listed (Update::instanced).
    bool shouldListInstances{};
};

// Diagnostics (the probes): one of a frame's instances of the game's models, and where it is.
struct InstancedModel
{
    std::uint32_t instance{};
    std::uint64_t hash{};
    std::array<float, 3> position{};
    std::uint64_t character{};    // A character's id (CharacterInstance::id); 0 for a placed model
    std::uint64_t hiddenGroups{}; // A placed model's (ModelInstance::hiddenGroups)
};

// The frame's instance `instance` of one of the game's models in `instanced` (by instance), or null.
const InstancedModel* instancedAt(std::span<const InstancedModel> instanced, std::uint32_t instance);

// What a frame's update leaves the renderer to do.
struct Update
{
    // A terrain ring as TerrainGrid (gi_scene.hlsl) makes it: its lowest corner (world x, z), its cells'
    // size and where its vertices lie in the pool; whether it is made again this frame.
    struct Ring
    {
        std::array<float, 2> corner{};
        float cellSize{};
        std::uint64_t poolOffset{};
        bool isRemade{};
    };

    // The characters' meshes to pose (Skin in gi_scene.hlsl), a record a mesh, and their palettes.
    std::vector<GISkinRecord> skins;
    std::vector<float> palettes;
    // The self-lit meshes placed this frame the rays aim at, the strongest at the eye, at most GI_MAX_EMITTERS.
    std::vector<GIEmitter> emitters;
    std::array<Ring, GI_TERRAIN_RINGS> terrain{};
    // Diagnostics: the frame's instances of the game's models when asked for (GameScene::shouldListInstances),
    // by instance; the triangles of every instance.
    std::vector<InstancedModel> instanced;
    std::uint64_t triangles{};
    // Models let go after a while unplaced, to read again when the game places them again, and the
    // textures no model left uses.
    std::vector<std::uint64_t> releasedModels, releasedTextures;
};

class SceneContents
{
public:
    // Makes the terrain rings; throws when the memory has no room for them.
    explicit SceneContents(SceneMemory& memory);

    // The frame's contents: its models, textures, characters and terrain, the memory's builds planned,
    // then its instances. Once a frame, between the memory's frame start and its recording.
    Update update(const GameScene& frame);

    // ---- Diagnostics ----
    // For the overlay and the log: the game's models, textures, characters and terrain rings held and traced.
    std::string status() const;
    // A hit a probe lists (GIShadowProbe::lightNearest) as text: what it hit, which side, how far; its
    // frame's instances `instanced` say what each instance is.
    std::string describeHit(std::span<const InstancedModel> instanced, const uint4& hit) const;
    // Every character the game has within 2 cm of `position` (a character's parts share one place),
    // each with all its model's meshes.
    std::string charactersAt(const std::array<float, 3>& position) const;
    // When each terrain ring was last made, and the game's models whose world bounds the ray from
    // `from` along `direction`, `length` long, crosses: whether the scene has them, and each mesh. With
    // the nearest one's hash, 0 for none.
    std::pair<std::string, std::uint64_t> describeRay(const std::array<float, 3>& from, const std::array<float, 3>& direction, float length) const;
    // Developer, while F10 records a frame: every model the scene holds and where it is placed, each mesh
    // with the index range the game draws it with and its material's technique, into models.txt in
    // `folder`: a draw in the frame log is matched to its mesh by that range (2026-10-26: which technique
    // the game's glass draws use).
    void dumpModels(const std::filesystem::path& folder) const;

private:
    // One of the game's models on the GPU side: its meshes in the pool from `poolOffset`, made once it is
    // first placed; the frame it was last placed. A rigid model has a structure, a geometry a mesh; a
    // skinned model's characters pose its meshes (`parts`: where each one's bind pose and indices lie, and
    // its material; `skins`: where each one's skin lies) into their own structures.
    struct ModelRecord
    {
        // A variant: a structure of the model's meshes but those in the mesh groups its placements hide
        // (ModelInstance::hiddenGroups): the meshes it holds (`parts`, in the model's order), none when
        // they hide every one; the last frame a placement asked for it.
        struct Variant
        {
            std::uint64_t hiddenGroups{};
            std::optional<BlasId> blas;
            std::vector<std::uint32_t> parts;
            std::uint32_t lastUsed{};
        };

        std::shared_ptr<const Model> geometry;
        std::optional<std::uint64_t> poolOffset;
        std::uint64_t poolBytes{};
        std::optional<BlasId> blas;
        // A self-lit mesh of the model (ModelRecord::emitters): which of its meshes, around where in object space, and
        // how much of it there is.
        struct Emitter
        {
            std::uint32_t part{};
            std::array<float, 3> centre{};
            float radius{}, area{};
        };

        std::vector<Geometry> parts; // Each mesh's place in the pool and material, once laid out
        std::vector<std::uint64_t> skins;
        std::vector<Variant> variants;
        std::uint32_t lastPlaced{};
        std::array<float, 6> bounds{}; // Diagnostics (the shadow probe): object space, lowest then highest
        std::vector<Emitter> emitters;
    };

    // One of the game's characters on the GPU side: its model's meshes it shows (`parts`, in the model's
    // order), posed into the pool from `poolOffset` in that order, a geometry each of its structure; where
    // it is this frame; whether its structure matches its pose this frame, and the frame it was last built.
    struct CharacterRecord
    {
        std::uint64_t hash{};
        std::vector<std::uint32_t> parts;
        std::uint64_t poolOffset{}, poolBytes{};
        BlasId blas{};
        Transform transform{};
        bool isTraced{};
        std::uint32_t built{};
    };

    // A ring of the terrain: its vertices in the pool and its structure, kept while the scene runs.
    struct TerrainRing
    {
        std::uint64_t poolOffset{};
        BlasId blas{};
        bool isPlaced{};
        std::array<float, 2> corner{};
        std::uint32_t refreshed{};
    };

    // One of the game's textures the models' meshes use: its slot in the scene's texture range once
    // copied; the models whose meshes use it.
    struct TextureRecord
    {
        std::optional<std::uint32_t> slot;
        std::unordered_set<std::uint64_t> users;
    };

    scene_description::Material materialOf(const ModelPart& part) const;
    std::vector<Geometry> stageModel(const Model& model, std::uint64_t poolOffset, std::vector<std::uint8_t>& bytes, std::vector<std::uint64_t>& skins) const;
    void updateTextures(const GameScene& frame);
    void useTextures(const Model& model, bool isUsed, Update& update);
    void updateModels(const GameScene& frame, Update& update);
    void updateCharacters(const GameScene& frame, Update& update);
    void planTerrain(const std::array<float, 3>& eye, std::uint32_t frameIndex, Update& update);
    void makeSea();
    void addInstances(const GameScene& frame, Update& update);
    static ModelRecord::Emitter emitterOf(const ModelPart& part, std::uint32_t index);
    static void addEmitters(const ModelRecord& record, const ModelInstance& placement, const ModelRecord::Variant* variant, std::uint32_t instance,
        const std::array<float, 3>& eye, std::vector<GIEmitter>& emitters);
    std::string describeCharacterMesh(std::uint64_t id, std::uint32_t shownIndex) const;
    static std::uint32_t partOfGeometry(const ModelRecord& record, std::uint64_t hiddenGroups, std::uint32_t geometry);

    SceneMemory& memory;

    // The game's models (engine_scene.hpp) by hash, and the latest placements; the characters by id,
    // and the latest list; the terrain's rings.
    std::unordered_map<std::uint64_t, ModelRecord> models;
    std::vector<ModelInstance> placements;
    std::unordered_map<std::uint64_t, CharacterRecord> characters;
    std::vector<CharacterInstance> characterList;
    std::array<TerrainRing, GI_TERRAIN_RINGS> terrain;
    // The sea's plane, made the first frame with a sea and kept.
    std::optional<BlasId> sea;
    // The game's textures the models' meshes use, by path hash.
    std::unordered_map<std::uint64_t, TextureRecord> textures;

    // Diagnostics: the frame's model instances and their triangles, the models waiting for room, and
    // models too large for a frame's upload; the textures with a slot.
    std::uint32_t modelInstanceCount{}, modelsWaiting{}, modelsTooLarge{}, terrainRings{}, texturesHeld{};
    std::uint64_t modelTriangles{};
    // Diagnostics: the frame's characters traced, those waiting for their model or for room, and their
    // posed vertices and triangles.
    std::uint32_t charactersTraced{}, charactersWaiting{};
    std::uint64_t characterVertices{}, characterTriangles{};
    // Diagnostics: the frame's placements of models the scene was never given (not read yet, or their file
    // failed), and its instances past the instance cap by kind; since the start, the variants made without
    // room for their structure (never traced) and the texture copies refused for want of a free slot.
    std::uint32_t unreceivedPlacements{}, modelsPastCap{}, charactersPastCap{}, ringsPastCap{};
    std::uint32_t variantsWithoutRoom{}, texturesRefused{};
    // Diagnostics: the frame has the sea's plane; the self-lit meshes placed near the eye, and those the rays aim at.
    bool isSeaTraced{};
    std::uint32_t emittersPlaced{}, emittersAimedAt{};
};
}
