#include "passes/ray_tracing/scene_contents.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace phantom_fx::scene_contents;
using phantom_fx::scene_description::Material;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// The scene memory without a GPU. Room is counted, not kept: the pool's bytes, the structures, the
// texture slots and the frame's instances, each up to a limit the test sets. A static structure is
// built at the next planBuilds, a refitted one when rebuilt; a closed slot is free again the next frame.
// It notes what the contents ask of it.
class FakeSceneMemory : public SceneMemory
{
public:
    struct Structure
    {
        Use use{};
        std::vector<Geometry> geometries;
        bool isLive{}, isBuilt{};
    };

    struct Instance
    {
        BlasId id{};
        std::uint32_t kind{};
    };

    struct MaterialChange
    {
        BlasId id{};
        std::uint32_t geometry{};
        Material material;
    };

    explicit FakeSceneMemory(std::uint32_t slots)
    {
        for (auto slot = slots; slot > 0; --slot) freeSlots.push_back(slot - 1);
    }

    // A new frame: the last one's instances, rebuilds and refits are cleared, and the slots it closed are
    // free again.
    void beginFrame()
    {
        instances.clear();
        rebuilt.clear();
        refitted.clear();
        uploaded = 0;
        freeSlots.insert(freeSlots.end(), closedSlots.begin(), closedSlots.end());
        closedSlots.clear();
    }

    // A structure whose build found no room in the frame's scratch: it is not ready until built again.
    void unbuild(BlasId id) { structures[id].isBuilt = false; }

    std::optional<std::uint64_t> allocate(std::uint64_t bytes) override
    {
        const auto size = (bytes + 15) / 16 * 16;
        if (poolUsed + size > poolRoom) return std::nullopt;

        const auto offset = poolEnd;
        poolEnd += size;
        poolUsed += size;

        return offset;
    }

    void release(std::uint64_t offset, std::uint64_t bytes) override
    {
        poolUsed -= (bytes + 15) / 16 * 16;
        releasedOffsets.push_back(offset);
    }

    std::uint64_t uploadRoom() const override { return GeometryUploadBytes - uploaded; }

    bool upload(std::uint64_t, std::span<const std::uint8_t> data) override
    {
        if (uploaded + data.size() > GeometryUploadBytes) return false;

        uploaded += data.size();

        return true;
    }

    std::optional<BlasId> createBlas(Use use, const std::vector<Geometry>& geometries) override
    {
        ++blasAttempts;
        const auto live = std::ranges::count_if(structures, &Structure::isLive);
        if (geometries.empty() || static_cast<std::size_t>(live) >= structureRoom) return std::nullopt;

        structures.push_back({use, geometries, true, false});

        return static_cast<BlasId>(structures.size() - 1);
    }

    void destroyBlas(BlasId id) override { structures[id].isLive = structures[id].isBuilt = false; }

    bool rebuild(BlasId id) override
    {
        structures[id].isBuilt = true;
        rebuilt.push_back(id);

        return true;
    }

    bool refit(BlasId id) override
    {
        if (!structures[id].isBuilt) return rebuild(id);

        refitted.push_back(id);

        return true;
    }

    void setMaterial(BlasId id, std::uint32_t geometry, const Material& material) override { materialChanges.push_back({id, geometry, material}); }

    void planBuilds() override
    {
        for (auto& structure : structures)
            if (structure.isLive && structure.use == Use::Static) structure.isBuilt = true;
    }

    bool ready(BlasId id) const override { return structures[id].isLive && structures[id].isBuilt; }

    std::optional<std::uint32_t> addInstance(BlasId id, const float*, std::uint32_t kind) override
    {
        if (instances.size() >= instanceCap || !ready(id)) return std::nullopt;

        instances.push_back({id, kind});

        return static_cast<std::uint32_t>(instances.size() - 1);
    }

    std::optional<std::uint32_t> openTexture(const SceneTexture&) override
    {
        if (freeSlots.empty()) return std::nullopt;

        const auto slot = freeSlots.back();
        freeSlots.pop_back();
        textureEvents.push_back("open " + std::to_string(slot));

        return slot;
    }

    void closeTexture(std::uint32_t slot) override
    {
        closedSlots.push_back(slot);
        textureEvents.push_back("close " + std::to_string(slot));
    }

    // The test's limits; the pool's bytes in use.
    std::uint64_t poolRoom = 1ull << 40, poolUsed{};
    std::size_t structureRoom = SIZE_MAX, instanceCap = SIZE_MAX;

    // What the contents asked: every structure by id; this frame's instances, rebuilds and refits; every
    // material change, slot opened and closed ("open 0", "close 0"), pool range given back and structure
    // asked for.
    std::vector<Structure> structures;
    std::vector<Instance> instances;
    std::vector<BlasId> rebuilt, refitted;
    std::vector<MaterialChange> materialChanges;
    std::vector<std::string> textureEvents;
    std::vector<std::uint64_t> releasedOffsets;
    std::uint32_t blasAttempts{};

private:
    std::uint64_t poolEnd{}, uploaded{};
    std::vector<std::uint32_t> freeSlots, closedSlots;
};

// What the game's scene gives one frame, kept for the spans GameScene points into. By default a newer list
// that places nothing.
struct GameFrame
{
    std::vector<std::shared_ptr<const Model>> newModels;
    bool hasNewLists = true;
    std::vector<ModelInstance> placements;
    std::vector<CharacterInstance> characters;
    std::vector<SceneTexture> textures;
    std::optional<std::array<float, 3>> terrainEye;
};

// The scene contents over a fake memory, a frame at a time. The terrain rings' structures are the first
// four (ring r's id is r).
class TestScene
{
public:
    explicit TestScene(std::uint32_t slots = 8) : memory(slots), contents(memory) {}

    Update run(const GameFrame& game = {})
    {
        memory.beginFrame();
        return contents.update({.frameIndex = ++frameIndex,
            .newModels = game.newModels,
            .hasNewLists = game.hasNewLists,
            .placements = game.placements,
            .characters = game.characters,
            .newTextures = game.textures,
            .terrainEye = game.terrainEye,
            .shouldListInstances = true});
    }

    // The last structure made with `geometries` geometries, of `use`, that is not a terrain ring.
    BlasId structureWith(Use use, std::size_t geometries) const
    {
        for (auto id = memory.structures.size(); id > GI_TERRAIN_RINGS; --id)
        {
            const auto& structure = memory.structures[id - 1];
            if (structure.use == use && structure.geometries.size() == geometries) return static_cast<BlasId>(id - 1);
        }

        throw std::runtime_error("No such structure was made.");
    }

    bool says(const std::string& text) const { return contents.status().find(text) != std::string::npos; }

    FakeSceneMemory memory;
    SceneContents contents;
    std::uint32_t frameIndex{};
};

// Model hashes, and texture path hashes.
constexpr std::uint64_t Building = 1, Soldier = 2, Crate = 3;
constexpr std::uint64_t Paint = 0x100, Rust = 0x200;

// Mesh `index`: one triangle in mesh group `group`, with texture coordinates and the base texture
// `texture` (0 for none).
ModelPart mesh(std::uint32_t index, std::uint32_t group, std::uint64_t texture = 0)
{
    ModelPart part;
    part.mesh = index;
    part.group = group;
    part.baseTexture = texture;
    part.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    part.uvs = {0, 0, 0};
    part.indices = {0, 1, 2};

    return part;
}

// A rigid model, or a skinned one with `bones` bones.
std::shared_ptr<const Model> model(std::uint64_t hash, std::vector<ModelPart> parts, std::uint32_t bones = 0)
{
    return std::make_shared<const Model>(Model{hash, std::move(parts), bones, {}});
}

constexpr Transform Identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

ModelInstance placed(std::uint64_t hash, std::uint64_t hiddenGroups = 0)
{
    return {hash, Identity, hiddenGroups};
}

// A character `id` of the skinned model `hash` with a palette of `bones` bones, showing its mesh groups
// `shownGroups` (every one past its end).
CharacterInstance listed(std::uint64_t id, std::uint64_t hash, std::vector<bool> shownGroups = {}, std::size_t bones = 1)
{
    CharacterInstance character;
    character.id = id;
    character.hash = hash;
    character.transform = Identity;
    character.palette.assign(bones * 12, 0.0f);
    character.shownGroups = std::move(shownGroups);

    return character;
}

// The game places a model, then stops: it is let go 600 frames after it was last placed, its structure and
// pool range given back. A model that comes without being placed is kept 600 frames from its arrival.
void retention()
{
    TestScene scene;
    scene.run({.newModels = {model(Building, {mesh(0, 0)}), model(Crate, {mesh(0, 0)})}, .placements = {placed(Building), placed(Crate)}});
    const auto released = scene.memory.releasedOffsets.size();
    const GameFrame crateOnly{.placements = {placed(Crate)}};
    for (int frame = 2; frame <= 601; ++frame) expect(scene.run(crateOnly).releasedModels.empty(), "A model unplaced for 600 frames is kept.");

    const auto letGo = scene.run(crateOnly);
    expect(letGo.releasedModels == std::vector<std::uint64_t>{Building}, "A model unplaced for 601 frames is let go.");
    expect(std::ranges::count_if(scene.memory.structures, &FakeSceneMemory::Structure::isLive) == GI_TERRAIN_RINGS + 1,
        "Its structure is destroyed, the placed model's kept.");
    expect(scene.memory.releasedOffsets.size() == released + 1, "Its pool range is given back.");

    TestScene arrival;
    arrival.run({.newModels = {model(Crate, {mesh(0, 0)})}});
    for (int frame = 2; frame <= 601; ++frame) expect(arrival.run().releasedModels.empty(), "A model never placed is kept 600 frames from its arrival.");

    expect(arrival.run().releasedModels == std::vector<std::uint64_t>{Crate}, "Then it is let go.");
}

// Without room in the pool a placed model waits, and is made the next frame there is room. One larger than
// a frame's upload is never made. A placement of a model the scene never received is not traced either.
void waitingForRoom()
{
    TestScene scene;
    scene.memory.poolRoom = scene.memory.poolUsed;
    expect(scene.run({.newModels = {model(Building, {mesh(0, 0)})}, .placements = {placed(Building)}}).instanced.empty(),
        "A model without room in the pool is not traced.");

    scene.memory.poolRoom = 1ull << 40;
    const auto made = scene.run({.placements = {placed(Building)}});
    expect(made.instanced.size() == 1 && made.instanced[0].hash == Building, "It is made and traced the next frame there is room.");

    TestScene large;
    auto huge = mesh(0, 0);
    huge.vertices.resize(GeometryUploadBytes / 16 + 1);
    const auto poolUsed = large.memory.poolUsed;
    large.run({.newModels = {model(Building, {huge})}, .placements = {placed(Building)}});
    for (int frame = 0; frame < 3; ++frame) expect(large.run({.placements = {placed(Building)}}).instanced.empty(), "A model larger than a frame's upload is not traced.");

    expect(large.memory.poolUsed == poolUsed && large.memory.structures.size() == GI_TERRAIN_RINGS, "It is never made.");

    TestScene unread;
    expect(unread.run({.placements = {placed(Crate)}}).instanced.empty() && unread.says("1 placements of models not received"),
        "A placement of a model not received is not traced, and counted.");
}

// A placement that hides mesh group 2 is traced through its variant, a structure of the model's other
// meshes, through which a hit names the right mesh. The variant goes once no placement has asked for it for
// 600 frames.
void variant()
{
    TestScene scene;
    const auto update = scene.run({.newModels = {model(Building, {mesh(0, 1), mesh(1, 2), mesh(2, 1)})}, .placements = {placed(Building, 1u << 2)}});
    expect(update.instanced.size() == 1 && update.instanced[0].hiddenGroups == 1u << 2, "The placement is traced through its variant.");

    const auto variantId = scene.memory.instances[0].id;
    const auto& whole = scene.memory.structures[scene.structureWith(Use::Static, 3)].geometries;
    const auto& shown = scene.memory.structures[variantId].geometries;
    expect(shown.size() == 2 && shown[0].vertexOffset == whole[0].vertexOffset && shown[1].vertexOffset == whole[2].vertexOffset,
        "The variant holds meshes 0 and 2, not mesh 1 of group 2.");

    const phantom_fx::gi::uint4 hit{update.instanced[0].instance + 1, 1, GI_INSTANCE_MODEL, std::bit_cast<std::uint32_t>(2.0f)};
    expect(scene.contents.describeHit(update.instanced, hit).find(", mesh 2 (") != std::string::npos, "A hit on the variant's second geometry is on mesh 2.");

    const GameFrame placedWhole{.placements = {placed(Building)}};
    for (int frame = 2; frame <= 601; ++frame) scene.run(placedWhole);

    expect(scene.memory.structures[variantId].isLive, "A variant no placement asks for is kept 600 frames.");

    scene.run(placedWhole);
    expect(!scene.memory.structures[variantId].isLive, "Then it is let go.");
}

// Today's outcome when a variant finds no room for its structure: its placements are not traced, and it is
// not made again while it is kept, even once there is room. The status line's count of variants made
// without room measures how often this happens in the game.
void variantWithoutRoomNeverTraced()
{
    TestScene scene;
    scene.memory.structureRoom = GI_TERRAIN_RINGS + 1;
    const GameFrame hiding{.placements = {placed(Building, 1u << 2)}};
    auto first = hiding;
    first.newModels = {model(Building, {mesh(0, 1), mesh(1, 2)})};
    expect(scene.run(first).instanced.empty(), "A placement whose variant has no structure is not traced.");

    scene.memory.structureRoom = SIZE_MAX;
    const auto attempts = scene.memory.blasAttempts;
    expect(scene.run(hiding).instanced.empty() && scene.memory.blasAttempts == attempts, "The variant is not made again once there is room.");
    expect(scene.says("1 variants made without room for their structure"), "The status line counts it.");
}

// A texture copy arriving gives its meshes their texture: in the model's structure, its variant's and its
// characters'.
void textureArrival()
{
    TestScene scene;
    GameFrame lists{.placements = {placed(Building, 1u << 2)}, .characters = {listed(7, Soldier)}};
    auto first = lists;
    first.newModels = {model(Building, {mesh(0, 1, Paint), mesh(1, 2, Paint), mesh(2, 1)}), model(Soldier, {mesh(0, 0, Paint)}, 1)};
    scene.run(first);
    expect(scene.memory.materialChanges.empty(), "Meshes keep their material until their texture comes.");

    lists.textures = {{Paint}};
    scene.run(lists);
    const auto whole = scene.structureWith(Use::Static, 3), trimmed = scene.structureWith(Use::Static, 2), character = scene.structureWith(Use::Refit, 1);
    std::set<std::pair<BlasId, std::uint32_t>> changed;
    for (const auto& change : scene.memory.materialChanges)
    {
        expect(change.material.texture == 1 && change.material.flags == GI_GEOMETRY_UV, "A mesh takes its texture's slot.");
        changed.insert({change.id, change.geometry});
    }

    expect(changed == std::set<std::pair<BlasId, std::uint32_t>>{{whole, 0}, {whole, 1}, {trimmed, 0}, {character, 0}},
        "The model's meshes with the texture take it, in its variant and its character too.");
}

// A finer copy of a texture opens a new slot before the old one closes. Without a free slot the copy is
// dropped and the meshes keep the old one.
void textureReplacement()
{
    TestScene scene(2);
    const GameFrame placedBuilding{.placements = {placed(Building)}};
    auto copy = [&](std::uint64_t texture) {
        auto frame = placedBuilding;
        frame.textures = {{texture}};
        return frame;
    };

    auto first = copy(Paint);
    first.newModels = {model(Building, {mesh(0, 0, Paint), mesh(1, 0, Rust)})};
    scene.run(first);
    scene.run(copy(Paint));
    expect(scene.memory.textureEvents == std::vector<std::string>{"open 0", "open 1", "close 0"}, "The new slot opens before the old one closes.");
    expect(scene.memory.materialChanges.size() == 1 && scene.memory.materialChanges[0].material.texture == 2, "The mesh takes the new slot.");

    scene.run(copy(Rust));
    scene.run(copy(Paint));
    expect(scene.memory.textureEvents == std::vector<std::string>{"open 0", "open 1", "close 0", "open 0"}, "Without a free slot nothing opens or closes.");
    expect(scene.memory.materialChanges.size() == 2, "The meshes keep the old copy.");
    expect(scene.says("(1 copies refused for want of a slot)"), "The status line counts the copy refused.");
}

// A model let go takes the textures no other model uses with it, their slots closed; a texture another model
// still uses stays.
void textureRelease()
{
    TestScene scene;
    scene.run({.newModels = {model(Building, {mesh(0, 0, Paint), mesh(1, 0, Rust)}), model(Crate, {mesh(0, 0, Paint)})},
        .placements = {placed(Building), placed(Crate)},
        .textures = {{Paint}, {Rust}}});

    const GameFrame crateOnly{.placements = {placed(Crate)}};
    Update letGo;
    while (letGo.releasedModels.empty() && scene.frameIndex < 1000) letGo = scene.run(crateOnly);

    expect(letGo.releasedTextures == std::vector<std::uint64_t>{Rust}, "Only the texture no model left uses is let go.");
    expect(scene.memory.textureEvents == std::vector<std::string>{"open 0", "open 1", "close 1"}, "Its slot closes; the shared texture's stays open.");
}

// A character is built when new and every 30 frames, refitted between (a refit's boxes loosen as limbs
// move); made again when it shows other mesh groups; let go when gone from the list. Characters past the
// frame's palette budget wait.
void characters()
{
    TestScene scene;
    const auto soldier = model(Soldier, {mesh(0, 0), mesh(1, 1)}, 1);
    GameFrame both{.newModels = {soldier}, .characters = {listed(7, Soldier, {true, true})}};
    scene.run(both);
    const auto first = scene.structureWith(Use::Refit, 2);
    expect(scene.memory.rebuilt == std::vector<BlasId>{first}, "A new character is built.");

    both.newModels.clear();
    for (int frame = 2; frame <= 30; ++frame)
    {
        scene.run(both);
        expect(scene.memory.refitted == std::vector<BlasId>{first} && scene.memory.rebuilt.empty(), "It is refitted to each pose.");
    }

    scene.run(both);
    expect(scene.memory.rebuilt == std::vector<BlasId>{first}, "It is built again 30 frames after it was built.");

    const auto released = scene.memory.releasedOffsets.size();
    scene.run({.characters = {listed(7, Soldier, {true, false})}});
    const auto shown = scene.structureWith(Use::Refit, 1);
    expect(!scene.memory.structures[first].isLive && scene.memory.rebuilt == std::vector<BlasId>{shown}, "Showing other mesh groups makes it again.");
    expect(scene.memory.releasedOffsets.size() == released + 1, "Its old pose's room is given back.");

    scene.run();
    expect(!scene.memory.structures[shown].isLive && scene.memory.releasedOffsets.size() == released + 2, "Gone from the list, it is let go.");

    // 1.2 MB of palette each: two do not fit the frame's 2 MB.
    TestScene crowd;
    constexpr std::size_t Bones = 25'000;
    const auto update = crowd.run({.newModels = {soldier}, .characters = {listed(7, Soldier, {}, Bones), listed(8, Soldier, {}, Bones)}});
    expect(update.instanced.size() == 1 && update.instanced[0].character == 7 && update.palettes.size() == Bones * 12,
        "A character past the frame's palette budget waits.");
}

// Without a newer list the last placements and poses stand: the same instances, no character posed or
// refitted again.
void noNewerLists()
{
    TestScene scene;
    const auto first = scene.run({.newModels = {model(Building, {mesh(0, 0)}), model(Soldier, {mesh(0, 0)}, 1)},
        .placements = {placed(Building)},
        .characters = {listed(7, Soldier)}});
    expect(first.instanced.size() == 2 && !first.skins.empty(), "The model and the character are traced.");

    const auto standing = scene.run({.hasNewLists = false});
    expect(standing.instanced.size() == 2, "Their instances stand.");
    expect(standing.skins.empty() && standing.palettes.empty() && scene.memory.rebuilt.empty() && scene.memory.refitted.empty(),
        "The character is not posed or refitted again.");
}

// Past the instance cap the frame's models come first, then its characters, then its terrain.
void instanceCap()
{
    TestScene scene;
    scene.memory.instanceCap = 3;
    GameFrame lists{.newModels = {model(Building, {mesh(0, 0)}), model(Soldier, {mesh(0, 0)}, 1)},
        .placements = {placed(Building), placed(Building)},
        .characters = {listed(7, Soldier)},
        .terrainEye = std::array<float, 3>{}};
    scene.run(lists);
    const auto instanceKinds = [&] {
        std::vector<std::uint32_t> kinds;
        for (const auto& instance : scene.memory.instances) kinds.push_back(instance.kind);

        return kinds;
    };
    expect(instanceKinds() == std::vector<std::uint32_t>{GI_INSTANCE_MODEL, GI_INSTANCE_MODEL, GI_INSTANCE_MODEL | GI_INSTANCE_CHARACTER},
        "The models, then the character, fill the cap.");
    expect(scene.says("past the instance cap: 0 models, 0 characters, 4 terrain rings"), "The terrain rings past it are counted.");

    lists.newModels.clear();
    scene.memory.instanceCap = 1;
    scene.run(lists);
    expect(instanceKinds() == std::vector<std::uint32_t>{GI_INSTANCE_MODEL}, "A model before everything else.");
    expect(scene.says("past the instance cap: 1 models, 1 characters, 4 terrain rings"), "Each kind past it is counted.");
}

// A ring made again takes the rings outside it along (a ring's edges lie on the next one's grid), not those
// inside. Each is refreshed by itself every 120 frames, ring r 30 r frames after ring 0.
// A bulb lights what is around it, not the camera: the rays aim at it from anywhere near the eye, however short its
// light's own reach. One a kilometre off gives its light through the rays that meet it.
void selfLitMeshesAimedAt()
{
    auto bulb = mesh(0, 0);
    bulb.role = phantom_fx::scene_description::MeshRole::Emissive;
    bulb.emission = {600, 480, 240};
    const auto bulbs = model(Building, {bulb});
    const Transform nearby{1, 0, 0, 30, 0, 1, 0, 0, 0, 0, 1, 0};
    const Transform distant{1, 0, 0, 1000, 0, 1, 0, 0, 0, 0, 1, 0};

    TestScene scene;
    scene.run({.newModels = {bulbs}, .placements = {{Building, nearby}, {Building, distant}}});
    const auto update = scene.run({.placements = {{Building, nearby}, {Building, distant}}});
    expect(update.emitters.size() == 1, "The bulb near the eye is aimed at, and the one a kilometre off is not.");

    const auto& emitter = update.emitters[0];
    expect(emitter.sphere.x > 30 && emitter.sphere.x < 31 && emitter.place.z == 1, "It is where it is placed, with its triangles.");
    expect(emitter.light.w > 0 && emitter.light.w < 30, "Its own reach stays what its light gives: a bulb's is far shorter than the distance it is aimed at from.");
}

void terrainRings()
{
    TestScene scene;
    const auto remade = [](const Update& update) {
        std::vector<std::uint32_t> rings;
        for (std::uint32_t r = 0; r < GI_TERRAIN_RINGS; ++r)
            if (update.terrain[r].isRemade) rings.push_back(r);

        return rings;
    };

    const GameFrame atOrigin{.terrainEye = std::array<float, 3>{0, 0, 0}};
    const auto first = scene.run(atOrigin);
    expect(remade(first) == std::vector<std::uint32_t>{0, 1, 2, 3}, "Every ring is made at first.");
    expect(first.terrain[0].corner == std::array<float, 2>{-128, -128} && first.terrain[3].cellSize == 64, "Each ring is centred on the eye.");
    expect(remade(scene.run(atOrigin)).empty(), "A ring that stays is not made again.");

    scene.memory.unbuild(2);
    expect(remade(scene.run(atOrigin)) == std::vector<std::uint32_t>{2, 3}, "A ring made again takes those outside it along, not those inside.");

    const GameFrame moved{.terrainEye = std::array<float, 3>{8, 0, 0}};
    expect(remade(scene.run(moved)) == std::vector<std::uint32_t>{0, 1, 2, 3}, "A step of the innermost ring makes every ring again.");

    const auto madeAt = scene.frameIndex;
    std::array<std::uint32_t, GI_TERRAIN_RINGS> refreshedAt{};
    while (scene.frameIndex < madeAt + 300)
    {
        const auto update = scene.run(moved);
        for (const auto r : remade(update))
            if (!refreshedAt[r] && std::ranges::find(scene.memory.refitted, r) != scene.memory.refitted.end()) refreshedAt[r] = scene.frameIndex - madeAt;
    }

    expect(refreshedAt == std::array<std::uint32_t, GI_TERRAIN_RINGS>{121, 151, 181, 211}, "The rings are refitted every 120 frames, staggered by 30.");
}
}

int main()
{
    try
    {
        retention();
        selfLitMeshesAimedAt();
        waitingForRoom();
        variant();
        variantWithoutRoomNeverTraced();
        textureArrival();
        textureReplacement();
        textureRelease();
        characters();
        noNewerLists();
        instanceCap();
        terrainRings();
        std::cout << "Scene contents tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
