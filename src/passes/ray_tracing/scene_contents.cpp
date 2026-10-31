// The scene contents; scene_contents.hpp describes them.
#include "passes/ray_tracing/scene_contents.hpp"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <numbers>
#include <stdexcept>

namespace phantom_fx::scene_contents
{
namespace
{
using scene_description::isCutOut;
using scene_description::MeshRole;

// A model the game has not placed for this many frames is let go.
constexpr std::uint32_t ModelRetentionFrames = 600;
// A character's structure is refitted to each pose and built again this often; the frame's palettes
// take this much of the upload buffer at most (characters past it wait for the next frame).
constexpr std::uint32_t CharacterRebuildFrames = 30;
constexpr std::uint64_t MaxPaletteBytes = 2ull << 20;
// The meshes of a model the scene takes at most (GI_HIT_INSTANCE_BITS).
constexpr std::size_t MaxModelParts = 4096;
// The terrain's rings (TerrainGrid in gi_scene.hlsl), a structure each. Ring r's cells are
// TerrainCellSize x 4^r metres: 256 m, 1 km, 4 km and 16 km across. A ring's corner moves in steps of
// eight of its cells (two of the next ring's: the rings' edges stay on the outer ring's grid lines); a
// ring that moves, or whose inner ring moves, is made again and its structure built again; every
// TerrainRefreshFrames (staggered by ring) it is made again and refitted, as the game streams finer
// pages of its heightmap in.
constexpr float TerrainCellSize = 1.0f;
constexpr std::uint32_t TerrainRefreshFrames = 120;
constexpr std::uint32_t TerrainRingVertices = GI_TERRAIN_CELLS * GI_TERRAIN_CELLS * 6;
// The sea's plane: a square two units across, its four vertices and six indices (padded to 16 bytes, as the
// pool's ranges are), under an instance that stretches it 100 km across under the eye, past the horizon from
// a helicopter.
constexpr std::uint64_t SeaVertexBytes = 4 * 16, SeaIndexBytes = 16, SeaBytes = SeaVertexBytes + SeaIndexBytes;
constexpr float SeaHalfWidth = 50'000;

template<class Record> auto findVariant(Record& record, std::uint64_t hiddenGroups)
{
    return std::find_if(record.variants.begin(), record.variants.end(), [&](const auto& variant) { return variant.hiddenGroups == hiddenGroups; });
}

// A mesh's bytes in the pool, in this order and each from a 16-byte boundary: its vertices (16 bytes:
// the position, then the texture coordinates), its indices, a skinned model's skin (GISkinRecord:
// 16 bytes a vertex), and its vertices' normals and tangents (GIGeometry::normalOffset: 16 bytes a vertex).
struct PoolLayout
{
    std::uint64_t vertices{}, indices{}, skin{}, normals{};
};

// A self-lit mesh's diffuse light reaches as far as it gives this much, in the game's light units: a ten-thousandth of
// the sun's, which a shadow ray would not show.
constexpr float LeastEmitterLight = 2;
// The rays aim at self-lit meshes this near the eye, in metres. Its reach says nothing of this: a bulb lights what is
// around it, and the camera is rarely within its metre or two.
constexpr float MostEmitterDistance = 200;

std::uint64_t aligned(std::uint64_t value, std::uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

PoolLayout poolLayoutOf(const Model& model, const ModelPart& part)
{
    return {part.vertices.size() * 16, aligned(part.indices.size() * 2, 16), model.bones ? part.vertices.size() * 16 : 0, part.normals.size() * 16};
}

// The bytes of a model's meshes in the pool.
std::uint64_t modelBytes(const Model& model)
{
    std::uint64_t bytes{};
    for (std::size_t i = 0; i < model.parts.size() && i < MaxModelParts; ++i)
    {
        const auto layout = poolLayoutOf(model, model.parts[i]);
        bytes += layout.vertices + layout.indices + layout.skin + layout.normals;
    }

    return bytes;
}

// One of a character model's meshes (Model::parts) as its file has it: its group and that group's state,
// its alpha and shadow bytes, whether the character shows it and whether this frame's draws posed it.
std::string characterMesh(const ModelPart& part, const CharacterInstance& character, bool isShown, std::size_t partIndex)
{
    const bool isDrawn = partIndex < character.drawnParts.size() && character.drawnParts[partIndex];
    const auto groupState = part.group < character.groupStates.size() ? std::format("{:#04x}", character.groupStates[part.group]) : std::string("unread");
    return std::format("file mesh {} (group {} state {}, alpha {:#04x} shadow {:#04x}, technique {:x}, {}, {} triangles, {}, {})", part.mesh, part.group,
        groupState, part.alphaBits, part.shadowBits, part.technique, part.role == MeshRole::Hair ? "hair" : isCutOut(part.role) ? "cut-out" : "opaque",
        part.indices.size() / 3, isShown ? "shown" : "hidden", isDrawn ? "drawn" : "not drawn");
}
}

const InstancedModel* instancedAt(std::span<const InstancedModel> instanced, std::uint32_t instance)
{
    const auto placed = std::lower_bound(instanced.begin(), instanced.end(), instance,
        [](const InstancedModel& model, std::uint32_t value) { return model.instance < value; });
    return placed == instanced.end() || placed->instance != instance ? nullptr : &*placed;
}

SceneContents::SceneContents(SceneMemory& memory) : memory(memory)
{
    // The terrain's room in the pool and its structures, kept while the scene runs.
    for (auto& ring : terrain)
    {
        const auto vertices = memory.allocate(std::uint64_t{TerrainRingVertices} * 16);
        const auto blas = vertices ? memory.createBlas(Use::Refit, {{*vertices, TerrainRingVertices}}) : std::nullopt;
        if (!blas) throw std::runtime_error("The scene has no room for the terrain.");

        ring.poolOffset = *vertices;
        ring.blas = *blas;
    }
}

Update SceneContents::update(const GameScene& frame)
{
    Update update;
    updateModels(frame, update);
    updateCharacters(frame, update);
    if (frame.terrainEye) planTerrain(*frame.terrainEye, frame.frameIndex, update);
    if (frame.seaCentre) makeSea();

    memory.planBuilds();
    addInstances(frame, update);

    return update;
}

// A model mesh's material, with its textures once the game's are copied (scene_description::materialOf).
scene_description::Material SceneContents::materialOf(const ModelPart& part) const
{
    const auto slotOf = [&](std::uint64_t path) {
        const auto texture = textures.find(path);
        return texture != textures.end() ? texture->second.slot : std::nullopt;
    };

    return scene_description::materialOf(part, {slotOf(part.baseTexture), slotOf(part.specularTexture), slotOf(part.normalTexture)});
}

// The model's meshes laid out from `poolOffset` into `bytes`, and their geometries; a skinned model's
// skins' places into `skins`.
std::vector<Geometry> SceneContents::stageModel(const Model& model, std::uint64_t poolOffset, std::vector<std::uint8_t>& bytes,
    std::vector<std::uint64_t>& skins) const
{
    std::vector<Geometry> geometries;
    skins.clear();
    bytes.assign(modelBytes(model), 0);
    std::uint64_t at{};
    for (std::size_t i = 0; i < model.parts.size() && i < MaxModelParts; ++i)
    {
        const auto& part = model.parts[i];
        const auto layout = poolLayoutOf(model, part);
        const auto material = materialOf(part);
        Geometry geometry{.flags = material.flags,
            .texture = material.texture,
            .alphaThreshold = material.alphaThreshold,
            .specularTexture = material.specularTexture,
            .normalTexture = material.normalTexture,
            .materialIndex = material.materialIndex,
            .emission = material.emission};
        geometry.vertexOffset = poolOffset + at;
        geometry.vertexCount = static_cast<std::uint32_t>(part.vertices.size());
        for (std::size_t v = 0; v < part.vertices.size(); ++v)
        {
            const std::uint32_t uv = v < part.uvs.size() ? part.uvs[v] : 0;
            std::memcpy(bytes.data() + at + v * 16, part.vertices[v].data(), 12);
            std::memcpy(bytes.data() + at + v * 16 + 12, &uv, 4);
        }

        at += layout.vertices;
        geometry.indexOffset = poolOffset + at;
        geometry.indexCount = static_cast<std::uint32_t>(part.indices.size());
        std::memcpy(bytes.data() + at, part.indices.data(), part.indices.size() * 2);
        at += layout.indices;
        if (!part.normals.empty()) geometry.normalOffset = poolOffset + at + layout.skin;
        geometries.push_back(geometry);
        if (!model.bones)
        {
            if (!part.normals.empty()) std::memcpy(bytes.data() + at, part.normals.data(), layout.normals);
            at += layout.normals;
            continue;
        }

        skins.push_back(poolOffset + at);
        for (std::size_t v = 0; v < part.vertices.size(); ++v)
        {
            const std::uint64_t bones = v < part.bones.size() ? part.bones[v] : 0;
            const std::uint32_t weights = v < part.weights.size() ? part.weights[v] : 0xFF;
            std::memcpy(bytes.data() + at + v * 16, &bones, 8);
            std::memcpy(bytes.data() + at + v * 16 + 8, &weights, 4);
        }

        at += layout.skin;
        if (!part.normals.empty()) std::memcpy(bytes.data() + at, part.normals.data(), layout.normals);
        at += layout.normals;
    }

    return geometries;
}

// The game's textures copied since the last frame into the scene's texture range: a new slot each, the
// one before closed, and the meshes using them given theirs. Without a free slot the copy is dropped
// and the one before kept.
void SceneContents::updateTextures(const GameScene& frame)
{
    for (const auto& made : frame.newTextures)
    {
        const auto found = textures.find(made.path);
        if (found == textures.end()) continue;

        const auto slot = memory.openTexture(made);
        if (!slot)
        {
            ++texturesRefused;
            continue;
        }

        auto& record = found->second;
        if (record.slot) memory.closeTexture(*record.slot);
        else ++texturesHeld;

        record.slot = slot;
        for (const auto user : record.users)
        {
            const auto model = models.find(user);
            if (model == models.end()) continue;

            auto& modelRecord = model->second;
            const auto& parts = modelRecord.geometry->parts;
            for (std::uint32_t i = 0; i < parts.size() && i < MaxModelParts; ++i)
            {
                if (parts[i].baseTexture != made.path && parts[i].specularTexture != made.path && parts[i].normalTexture != made.path) continue;

                const auto material = materialOf(parts[i]);
                if (modelRecord.blas) memory.setMaterial(*modelRecord.blas, i, material);
                if (i >= modelRecord.parts.size()) continue;

                // Its variants' structures, and a skinned model's characters' structures, that have it among
                // the meshes they show.
                modelRecord.parts[i].flags = material.flags;
                modelRecord.parts[i].texture = material.texture;
                modelRecord.parts[i].alphaThreshold = material.alphaThreshold;
                modelRecord.parts[i].specularTexture = material.specularTexture;
                modelRecord.parts[i].normalTexture = material.normalTexture;
                for (const auto& variant : modelRecord.variants)
                {
                    const auto shown = std::find(variant.parts.begin(), variant.parts.end(), i);
                    if (variant.blas && shown != variant.parts.end())
                        memory.setMaterial(*variant.blas, static_cast<std::uint32_t>(shown - variant.parts.begin()), material);
                }

                for (const auto& [id, character] : characters)
                {
                    const auto shown = std::find(character.parts.begin(), character.parts.end(), i);
                    if (character.hash == user && shown != character.parts.end())
                        memory.setMaterial(character.blas, static_cast<std::uint32_t>(shown - character.parts.begin()), material);
                }
            }
        }
    }
}

// A model's meshes' textures, wanted (`isUsed`) or no longer: a texture no model uses is let go
// (Update::releasedTextures), its slot closed.
void SceneContents::useTextures(const Model& model, bool isUsed, Update& update)
{
    for (const auto& part : model.parts)
        for (const auto path : {part.baseTexture, part.specularTexture, part.normalTexture})
        {
            if (!path) continue;

            if (isUsed)
            {
                textures[path].users.insert(model.hash);
                continue;
            }

            const auto found = textures.find(path);
            if (found == textures.end()) continue;

            found->second.users.erase(model.hash);
            if (!found->second.users.empty()) continue;

            if (found->second.slot)
            {
                memory.closeTexture(*found->second.slot);
                --texturesHeld;
            }

            update.releasedTextures.push_back(found->first);
            textures.erase(found);
        }
}

// The frame's models: new ones registered, the placed ones' meshes into the pool and their structures
// made (as the frame's upload and the memory allow), those not placed for ModelRetentionFrames let go
// (Update::releasedModels).
void SceneContents::updateModels(const GameScene& frame, Update& update)
{
    for (const auto& model : frame.newModels)
    {
        if (!model || models.contains(model->hash)) continue;

        useTextures(*model, true, update);
        // Kept ModelRetentionFrames from now, placed or not: counted from frame 0, a model the frame's
        // lists did not place was let go as it came, and its file read again the next snapshot.
        auto& record = models[model->hash];
        record.geometry = model;
        record.lastPlaced = frame.frameIndex;
        record.bounds = {FLT_MAX, FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (const auto& part : model->parts)
            for (const auto& vertex : part.vertices)
                for (int axis = 0; axis < 3; ++axis)
                {
                    record.bounds[axis] = std::min(record.bounds[axis], vertex[axis]);
                    record.bounds[3 + axis] = std::max(record.bounds[3 + axis], vertex[axis]);
                }

        for (std::uint32_t i = 0; i < model->parts.size() && i < MaxModelParts; ++i)
            if (model->parts[i].role == MeshRole::Emissive) record.emitters.push_back(emitterOf(model->parts[i], i));
    }

    updateTextures(frame);

    if (frame.hasNewLists)
    {
        placements.assign(frame.placements.begin(), frame.placements.end());
        characterList.assign(frame.characters.begin(), frame.characters.end());
    }

    // Placements come grouped by model (an instance group's, a brush plugin's): one lookup a run.
    auto found = models.end();
    std::uint64_t lastHash{};
    for (const auto& placement : placements)
    {
        if (placement.hash != lastHash) found = models.find(lastHash = placement.hash);
        if (found != models.end()) found->second.lastPlaced = frame.frameIndex;
    }

    for (const auto& character : characterList)
        if (const auto model = models.find(character.hash); model != models.end()) model->second.lastPlaced = frame.frameIndex;

    modelsWaiting = 0;
    modelsTooLarge = 0;
    std::vector<std::uint8_t> bytes;
    for (auto it = models.begin(); it != models.end();)
    {
        auto& record = it->second;
        std::erase_if(record.variants, [&](const ModelRecord::Variant& variant) {
            if (frame.frameIndex - variant.lastUsed <= ModelRetentionFrames) return false;

            if (variant.blas) memory.destroyBlas(*variant.blas);
            return true;
        });
        if (frame.frameIndex - record.lastPlaced > ModelRetentionFrames)
        {
            if (record.blas) memory.destroyBlas(*record.blas);
            for (const auto& variant : record.variants)
                if (variant.blas) memory.destroyBlas(*variant.blas);
            if (record.poolOffset) memory.release(*record.poolOffset, record.poolBytes);
            useTextures(*record.geometry, false, update);
            update.releasedModels.push_back(it->first);
            it = models.erase(it);
            continue;
        }

        // Made once first placed; a model larger than a frame's upload never is. A skinned model is
        // only laid out in the pool: its characters make their own structures (updateCharacters).
        const bool isSkinned = record.geometry->bones != 0;
        const bool isMade = isSkinned ? record.poolOffset.has_value() : record.blas.has_value();
        const auto needed = modelBytes(*record.geometry);
        if (record.lastPlaced == frame.frameIndex && !isMade && needed && needed <= GeometryUploadBytes)
        {
            // What has no room this frame waits for the next; an upload whose structure has no room
            // lands in the range given back, which nothing uses before this frame's copy.
            const auto offset = needed <= memory.uploadRoom() ? memory.allocate(needed) : std::nullopt;
            std::vector<std::uint64_t> skins;
            auto geometries = offset ? stageModel(*record.geometry, *offset, bytes, skins) : std::vector<Geometry>{};
            const bool isUploaded = offset && memory.upload(*offset, bytes);
            const auto blas = isUploaded && !isSkinned ? memory.createBlas(Use::Static, geometries) : std::nullopt;
            if (isUploaded && (isSkinned || blas))
            {
                record.poolOffset = offset;
                record.poolBytes = needed;
                record.blas = blas;
                record.parts = std::move(geometries);
                record.skins = std::move(skins);
            }
            else
            {
                if (offset) memory.release(*offset, needed);
                ++modelsWaiting;
            }
        }
        else if (needed > GeometryUploadBytes && record.lastPlaced == frame.frameIndex && !isMade)
            ++modelsTooLarge;

        ++it;
    }

    // A placement that hides mesh groups (engine_scene's hiddenGroupsOf) takes its variant: a structure
    // of its model's other meshes, made once its model's is.
    found = models.end();
    lastHash = 0;
    for (const auto& placement : placements)
    {
        if (!placement.hiddenGroups) continue;
        if (placement.hash != lastHash) found = models.find(lastHash = placement.hash);
        if (found == models.end() || !found->second.blas) continue;

        auto& record = found->second;
        if (const auto variant = findVariant(record, placement.hiddenGroups); variant != record.variants.end())
        {
            variant->lastUsed = frame.frameIndex;
            continue;
        }

        auto& variant = record.variants.emplace_back();
        variant.hiddenGroups = placement.hiddenGroups;
        variant.lastUsed = frame.frameIndex;
        std::vector<Geometry> geometries;
        for (std::uint32_t i = 0; i < record.parts.size(); ++i)
        {
            const auto group = record.geometry->parts[i].group;
            if (group < 64 && ((placement.hiddenGroups >> group) & 1)) continue;

            variant.parts.push_back(i);
            geometries.push_back(record.parts[i]);
        }

        // One without room for its structure is not made again: its placements stay untraced until it is
        // let go.
        variant.blas = memory.createBlas(Use::Static, geometries);
        if (!variant.blas && !geometries.empty()) ++variantsWithoutRoom;
    }
}

// The frame's characters (GameScene::characters): each one's shown meshes posed into its own room of the
// pool (a record a mesh into Update::skins for Skin in gi_scene.hlsl, its palette onto Update::palettes),
// its structure refitted to the pose, or built again when new and every CharacterRebuildFrames (a refit's
// boxes loosen as limbs move). One that shows other meshes is made again; one gone from the list is
// let go. Without a newer list the last poses stand.
void SceneContents::updateCharacters(const GameScene& frame, Update& update)
{
    if (!frame.hasNewLists) return;

    auto& skins = update.skins;
    auto& palettes = update.palettes;
    std::unordered_set<std::uint64_t> seen;
    std::uint32_t vertices{};
    charactersWaiting = 0;
    for (const auto& character : characterList)
    {
        const auto model = models.find(character.hash);
        if (model != models.end() && model->second.geometry->parts.empty()) continue; // Nothing the scene takes

        const auto bones = std::uint64_t{character.palette.size() / 12};
        if (model == models.end() || model->second.parts.empty() || bones < model->second.geometry->bones
            || (palettes.size() + character.palette.size()) * sizeof(float) > MaxPaletteBytes)
        {
            // One made already keeps its room, out of this frame.
            if (const auto kept = characters.find(character.id); kept != characters.end())
            {
                kept->second.isTraced = false;
                seen.insert(character.id);
            }

            ++charactersWaiting;
            continue;
        }

        const auto& modelRecord = model->second;
        const auto& parts = modelRecord.geometry->parts;
        std::vector<std::uint32_t> shown;
        for (std::uint32_t i = 0; i < modelRecord.parts.size(); ++i)
            if (parts[i].group >= character.shownGroups.size() || character.shownGroups[parts[i].group]) shown.push_back(i);

        if (shown.empty()) continue;

        auto found = characters.find(character.id);
        if (found != characters.end() && (found->second.hash != character.hash || found->second.parts != shown))
        {
            memory.destroyBlas(found->second.blas);
            memory.release(found->second.poolOffset, found->second.poolBytes);
            characters.erase(found);
            found = characters.end();
        }

        const bool isNew = found == characters.end();
        if (isNew)
        {
            std::uint64_t bytes{};
            for (const auto i : shown) bytes += std::uint64_t{modelRecord.parts[i].vertexCount} * (modelRecord.parts[i].normalOffset ? 32 : 16);
            const auto offset = memory.allocate(bytes);
            std::vector<Geometry> geometries;
            std::uint64_t at{};
            for (const auto i : shown)
            {
                auto& geometry = geometries.emplace_back(modelRecord.parts[i]);
                geometry.vertexOffset = offset.value_or(0) + at;
                at += std::uint64_t{geometry.vertexCount} * 16;
                if (!geometry.normalOffset) continue;

                geometry.normalOffset = offset.value_or(0) + at;
                at += std::uint64_t{geometry.vertexCount} * 16;
            }

            const auto blas = offset ? memory.createBlas(Use::Refit, geometries) : std::nullopt;
            if (!blas)
            {
                if (offset) memory.release(*offset, bytes);
                ++charactersWaiting;
                continue;
            }

            found = characters.emplace(character.id, CharacterRecord{character.hash, std::move(shown), *offset, bytes, *blas}).first;
        }

        auto& record = found->second;
        seen.insert(character.id);
        record.transform = character.transform;
        const auto firstBone = static_cast<std::uint32_t>(palettes.size() / 12);
        palettes.insert(palettes.end(), character.palette.begin(), character.palette.end());
        std::uint64_t at{};
        for (const auto i : record.parts)
        {
            const auto& part = modelRecord.parts[i];
            const auto posedVertices = record.poolOffset + at;
            const std::uint64_t posedNormals = part.normalOffset ? posedVertices + std::uint64_t{part.vertexCount} * 16 : 0;
            skins.push_back({vertices, part.vertexCount, static_cast<std::uint32_t>(part.vertexOffset), static_cast<std::uint32_t>(modelRecord.skins[i]),
                static_cast<std::uint32_t>(posedVertices), firstBone, static_cast<std::uint32_t>(part.normalOffset), static_cast<std::uint32_t>(posedNormals)});
            vertices += part.vertexCount;
            at += std::uint64_t{part.vertexCount} * (part.normalOffset ? 32 : 16);
        }

        // Not refitted (the frame's scratch is full), the structure no longer matches the pose: left
        // out of the frame.
        const bool shouldRebuild = isNew || frame.frameIndex - record.built >= CharacterRebuildFrames;
        record.isTraced = shouldRebuild ? memory.rebuild(record.blas) : memory.refit(record.blas);
        if (shouldRebuild && record.isTraced) record.built = frame.frameIndex;
    }

    for (auto it = characters.begin(); it != characters.end();)
    {
        if (seen.contains(it->first))
        {
            ++it;
            continue;
        }

        memory.destroyBlas(it->second.blas);
        memory.release(it->second.poolOffset, it->second.poolBytes);
        it = characters.erase(it);
    }
}

// The frame's terrain rings around `eye`: where each lies, and which to make again with a new structure,
// or with the old one refitted, this frame (Update::Ring::isRemade). Each ring's place goes into the
// frame's constants.
void SceneContents::planTerrain(const std::array<float, 3>& eye, std::uint32_t frameIndex, Update& update)
{
    bool hasInnerMoved{};
    for (std::uint32_t r = 0; r < GI_TERRAIN_RINGS; ++r)
    {
        auto& ring = terrain[r];
        auto& placed = update.terrain[r];
        const float cell = TerrainCellSize * std::pow(4.0f, static_cast<float>(r));
        const float step = cell * 8;
        const std::array<float, 2> corner{std::floor((eye[0] - cell * GI_TERRAIN_CELLS / 2) / step) * step,
            std::floor((eye[2] - cell * GI_TERRAIN_CELLS / 2) / step) * step};
        const bool hasMoved = !ring.isPlaced || corner != ring.corner || hasInnerMoved || !memory.ready(ring.blas);
        if (hasMoved)
        {
            ring.corner = corner;
            ring.isPlaced = true;
            ring.refreshed = frameIndex;
            placed.isRemade = memory.rebuild(ring.blas);
        }
        else if (frameIndex - ring.refreshed > TerrainRefreshFrames + r * TerrainRefreshFrames / GI_TERRAIN_RINGS)
        {
            ring.refreshed = frameIndex;
            placed.isRemade = memory.refit(ring.blas);
        }

        placed.corner = ring.corner;
        placed.cellSize = cell;
        placed.poolOffset = ring.poolOffset;
        hasInnerMoved = hasMoved;
    }
}

// The sea's plane, once: its corners as TerrainGrid lays its cells out, clockwise seen from above, so its front
// faces up. Without room it waits for a later frame.
void SceneContents::makeSea()
{
    if (sea) return;

    const auto offset = memory.allocate(SeaBytes);
    if (!offset) return;

    const float vertices[16]{-1, 0, -1, 1, -1, 0, 1, 1, 1, 0, -1, 1, 1, 0, 1, 1};
    const std::uint16_t indices[6]{0, 1, 2, 2, 1, 3};
    std::vector<std::uint8_t> bytes(SeaBytes);
    std::memcpy(bytes.data(), vertices, sizeof(vertices));
    std::memcpy(bytes.data() + SeaVertexBytes, indices, sizeof(indices));
    if (memory.upload(*offset, bytes)) sea = memory.createBlas(Use::Static, {{*offset, 4, *offset + SeaVertexBytes, 6}});
    if (!sea) memory.release(*offset, SeaBytes);
}

// A self-lit mesh's sphere and area, in its model's space.
SceneContents::ModelRecord::Emitter SceneContents::emitterOf(const ModelPart& part, std::uint32_t index)
{
    std::array<float, 3> lowest{FLT_MAX, FLT_MAX, FLT_MAX}, highest{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const auto& vertex : part.vertices)
        for (int axis = 0; axis < 3; ++axis)
        {
            lowest[axis] = std::min(lowest[axis], vertex[axis]);
            highest[axis] = std::max(highest[axis], vertex[axis]);
        }

    ModelRecord::Emitter emitter{.part = index};
    float squared{};
    for (int axis = 0; axis < 3; ++axis)
    {
        emitter.centre[axis] = (lowest[axis] + highest[axis]) / 2;
        const float half = (highest[axis] - lowest[axis]) / 2;
        squared += half * half;
    }

    emitter.radius = std::sqrt(squared);
    for (std::size_t i = 0; i + 2 < part.indices.size(); i += 3)
    {
        const auto& a = part.vertices[part.indices[i]];
        const auto& b = part.vertices[part.indices[i + 1]];
        const auto& c = part.vertices[part.indices[i + 2]];
        const std::array u{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const std::array cross{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        emitter.area += std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]) / 2;
    }

    return emitter;
}

// The self-lit meshes of a placement near the eye, onto `emitters`.
void SceneContents::addEmitters(const ModelRecord& record, const ModelInstance& placement, const ModelRecord::Variant* variant, std::uint32_t instance,
    const std::array<float, 3>& eye, std::vector<GIEmitter>& emitters)
{
    const auto& t = placement.transform;
    const float scale = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    for (const auto& emitter : record.emitters)
    {
        auto geometry = emitter.part;
        if (variant)
        {
            const auto shown = std::find(variant->parts.begin(), variant->parts.end(), emitter.part);
            if (shown == variant->parts.end()) continue;

            geometry = static_cast<std::uint32_t>(shown - variant->parts.begin());
        }

        const auto& [x, y, z] = emitter.centre;
        const std::array centre{t[0] * x + t[1] * y + t[2] * z + t[3], t[4] * x + t[5] * y + t[6] * z + t[7], t[8] * x + t[9] * y + t[10] * z + t[11]};
        const auto& emission = record.parts[emitter.part].emission;
        const float spread = emitter.area * scale * scale / std::numbers::pi_v<float>;
        const float strongest = std::max({emission[0], emission[1], emission[2]}) * spread;
        const float reach = std::sqrt(strongest / LeastEmitterLight) + emitter.radius * scale;
        const float offset[]{centre[0] - eye[0], centre[1] - eye[1], centre[2] - eye[2]};
        if (offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2] > MostEmitterDistance * MostEmitterDistance) continue;

        emitters.push_back({{centre[0], centre[1], centre[2], emitter.radius * scale}, {emission[0] * spread, emission[1] * spread, emission[2] * spread, reach},
            {instance, geometry, record.parts[emitter.part].indexCount / 3, 0}});
    }
}

// The frame's instances: the game's models where the game has them (on screen or not), its characters,
// its terrain and the sea.
void SceneContents::addInstances(const GameScene& frame, Update& update)
{
    modelInstanceCount = 0;
    modelTriangles = 0;
    unreceivedPlacements = modelsPastCap = charactersPastCap = ringsPastCap = 0;
    auto found = models.end();
    std::uint64_t lastHash{};
    for (const auto& placement : placements)
    {
        if (placement.hash != lastHash) found = models.find(lastHash = placement.hash);
        if (found == models.end())
        {
            ++unreceivedPlacements;
            continue;
        }

        if (!found->second.blas) continue;

        // A placement that hides mesh groups takes its variant's structure: it is not traced while that is
        // not built, nor ever when the variant has none (no room for it, or every mesh hidden).
        const auto& record = found->second;
        const auto variant = placement.hiddenGroups ? findVariant(record, placement.hiddenGroups) : record.variants.end();
        const bool isTrimmed = variant != record.variants.end();
        const auto blas = isTrimmed ? variant->blas : record.blas;
        if (!blas) continue;

        // Not traced: its structure is not built yet, or the frame is past the instance cap.
        const auto instance = memory.addInstance(*blas, placement.transform.data(), GI_INSTANCE_MODEL);
        if (!instance)
        {
            if (memory.ready(*blas)) ++modelsPastCap;
            continue;
        }

        if (frame.shouldListInstances)
            update.instanced.push_back(
                {*instance, placement.hash, {placement.transform[3], placement.transform[7], placement.transform[11]}, 0, isTrimmed ? placement.hiddenGroups : 0});

        if (!record.emitters.empty()) addEmitters(record, placement, isTrimmed ? &*variant : nullptr, *instance, frame.eye, update.emitters);

        ++modelInstanceCount;
        const auto& parts = record.geometry->parts;
        if (isTrimmed)
            for (const auto i : variant->parts) modelTriangles += parts[i].indices.size() / 3;
        else
            for (const auto& part : parts) modelTriangles += part.indices.size() / 3;
    }

    // The strongest at the eye are the rays' to aim at: the others' light comes through the rays that meet them.
    emittersPlaced = static_cast<std::uint32_t>(update.emitters.size());
    if (update.emitters.size() > GI_MAX_EMITTERS)
    {
        const auto strength = [&](const GIEmitter& emitter) {
            const float offset[]{emitter.sphere.x - frame.eye[0], emitter.sphere.y - frame.eye[1], emitter.sphere.z - frame.eye[2]};
            const float squared = offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2];
            return std::max({emitter.light.x, emitter.light.y, emitter.light.z}) / std::max(squared, 0.01f);
        };

        std::nth_element(update.emitters.begin(), update.emitters.begin() + GI_MAX_EMITTERS, update.emitters.end(),
            [&](const GIEmitter& a, const GIEmitter& b) { return strength(a) > strength(b); });
        update.emitters.resize(GI_MAX_EMITTERS);
    }

    emittersAimedAt = static_cast<std::uint32_t>(update.emitters.size());
    update.triangles += modelTriangles;
    charactersTraced = 0;
    characterVertices = 0;
    characterTriangles = 0;
    for (const auto& [id, character] : characters)
    {
        if (!character.isTraced) continue;

        const auto instance = memory.addInstance(character.blas, character.transform.data(), GI_INSTANCE_MODEL | GI_INSTANCE_CHARACTER);
        if (!instance)
        {
            if (memory.ready(character.blas)) ++charactersPastCap;
            continue;
        }

        if (frame.shouldListInstances)
            update.instanced.push_back({*instance, character.hash, {character.transform[3], character.transform[7], character.transform[11]}, id});

        ++charactersTraced;
        const auto& parts = models.at(character.hash).parts;
        for (const auto i : character.parts)
        {
            characterVertices += parts[i].vertexCount;
            characterTriangles += parts[i].indexCount / 3;
        }
    }

    update.triangles += characterTriangles;
    terrainRings = 0;
    for (std::uint32_t r = 0; r < GI_TERRAIN_RINGS && frame.terrainEye; ++r)
    {
        if (!memory.addInstance(terrain[r].blas, nullptr, GI_INSTANCE_TERRAIN))
        {
            if (memory.ready(terrain[r].blas)) ++ringsPastCap;
            continue;
        }

        update.triangles += TerrainRingVertices / 3;
        ++terrainRings;
    }

    isSeaTraced = false;
    if (frame.seaCentre && sea)
    {
        const auto& [x, y, z] = *frame.seaCentre;
        const float transform[12]{SeaHalfWidth, 0, 0, x, 0, 1, 0, y, 0, 0, SeaHalfWidth, z};
        isSeaTraced = memory.addInstance(*sea, transform, GI_INSTANCE_SEA).has_value();
        if (isSeaTraced) update.triangles += 2;
    }
}

std::string SceneContents::status() const
{
    return "the game's models: " + std::to_string(modelInstanceCount) + " instances (" + std::to_string(modelTriangles / 1000) + "k triangles) of "
        + std::to_string(models.size()) + " models, " + std::to_string(modelsWaiting) + " waiting for room"
        + (modelsTooLarge ? ", " + std::to_string(modelsTooLarge) + " too large" : "") + ", " + std::to_string(unreceivedPlacements)
        + " placements of models not received, " + std::to_string(variantsWithoutRoom) + " variants made without room for their structure, "
        + std::to_string(texturesHeld) + " of the game's textures held (" + std::to_string(texturesRefused) + " copies refused for want of a slot); characters: "
        + std::to_string(charactersTraced) + " traced (" + std::to_string(characterVertices / 1000) + "k vertices posed, "
        + std::to_string(characterTriangles / 1000) + "k triangles), " + std::to_string(charactersWaiting) + " waiting; terrain rings "
        + std::to_string(terrainRings) + " of " + std::to_string(GI_TERRAIN_RINGS) + "; past the instance cap: " + std::to_string(modelsPastCap)
        + " models, " + std::to_string(charactersPastCap) + " characters, " + std::to_string(ringsPastCap) + " terrain rings"
        + (isSeaTraced ? "; the sea's plane traced" : "") + "; self-lit meshes: " + std::to_string(emittersPlaced) + " near the eye, "
        + std::to_string(emittersAimedAt) + " aimed at";
}

// The character `id`'s shown mesh `shownIndex` (a geometry of its structure) as its model file has it.
std::string SceneContents::describeCharacterMesh(std::uint64_t id, std::uint32_t shownIndex) const
{
    const auto record = characters.find(id);
    const auto listed = std::ranges::find(characterList, id, &CharacterInstance::id);
    if (record == characters.end() || listed == characterList.end() || shownIndex >= record->second.parts.size()) return "";

    const auto model = models.find(record->second.hash);
    const auto partIndex = record->second.parts[shownIndex];
    if (model == models.end() || partIndex >= model->second.geometry->parts.size()) return "";

    return ", " + characterMesh(model->second.geometry->parts[partIndex], *listed, true, partIndex);
}

std::string SceneContents::charactersAt(const std::array<float, 3>& position) const
{
    std::string text;
    for (const auto& character : characterList)
    {
        const auto& t = character.transform;
        if (std::abs(t[3] - position[0]) > 0.02f || std::abs(t[7] - position[1]) > 0.02f || std::abs(t[11] - position[2]) > 0.02f) continue;

        const auto model = models.find(character.hash);
        const auto record = characters.find(character.id);
        text += std::format("\n  character {:x} (id {:x}){}:", character.hash, character.id,
            record == characters.end() ? ", not in the scene" : record->second.isTraced ? "" : ", not traced this frame");
        if (model == models.end())
        {
            text += " its model not read yet";
            continue;
        }

        const auto& parts = model->second.geometry->parts;
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            const bool isShown = record != characters.end() && std::ranges::find(record->second.parts, static_cast<std::uint32_t>(i)) != record->second.parts.end();
            text += (i ? "; " : " ") + characterMesh(parts[i], character, isShown, i);
        }

        for (const auto& mesh : model->second.geometry->dropped)
            text += std::format("; file mesh {} left out (alpha {:#04x} shadow {:#04x})", mesh.mesh, mesh.alphaBits, mesh.shadowBits);
    }

    return text;
}

// The model's mesh (an index into Model::parts) a placement's structure holds as its geometry
// `geometry`: the same, unless the placement hides mesh groups (its variant's).
std::uint32_t SceneContents::partOfGeometry(const ModelRecord& record, std::uint64_t hiddenGroups, std::uint32_t geometry)
{
    const auto variant = hiddenGroups ? findVariant(record, hiddenGroups) : record.variants.end();
    if (variant == record.variants.end()) return geometry;

    return geometry < variant->parts.size() ? variant->parts[geometry] : UINT32_MAX;
}

std::string SceneContents::describeHit(std::span<const InstancedModel> instanced, const uint4& hit) const
{
    if (!hit.x) return "nothing";

    const auto instance = hit.x - 1, geometry = hit.y & 0x7FFFFFFFu, kind = hit.z;
    std::string what = (kind & GI_INSTANCE_SEA) ? "the sea" : "the terrain";
    if (kind & GI_INSTANCE_MODEL)
    {
        const auto placed = instancedAt(instanced, instance);
        if (!placed) what = "one of the game's models";
        else if (kind & GI_INSTANCE_CHARACTER)
            // A character's geometries are the meshes it shows, not its model's.
            what = std::format("character {:x} at ({:.1f} {:.1f} {:.1f}), its shown mesh {}{}", placed->hash, placed->position[0], placed->position[1],
                placed->position[2], geometry, describeCharacterMesh(placed->character, geometry));
        else
        {
            what = std::format("model {:x} placed at ({:.1f} {:.1f} {:.1f})", placed->hash, placed->position[0], placed->position[1], placed->position[2]);
            const auto found = models.find(placed->hash);
            const auto partIndex = found != models.end() ? partOfGeometry(found->second, placed->hiddenGroups, geometry) : UINT32_MAX;
            if (found != models.end() && partIndex < found->second.geometry->parts.size())
            {
                const auto& part = found->second.geometry->parts[partIndex];
                what += std::format(", mesh {} ({}, {} triangles, alpha {:#04x} shadow {:#04x}, technique {:x})", part.mesh,
                    part.role == MeshRole::Hair ? "hair" : part.role == MeshRole::Leaf ? "leaves" : part.role == MeshRole::Glass ? "glass" : part.role == MeshRole::Mirror ? "mirror" : isCutOut(part.role) ? "cut-out" : "opaque",
                    part.indices.size() / 3, part.alphaBits, part.shadowBits, part.technique);
            }
        }
    }

    return std::format("{} at {:.2f} m, its {} face (instance {})", what, std::bit_cast<float>(hit.w), (hit.y >> 31) ? "front" : "back", instance);
}

std::pair<std::string, std::uint64_t> SceneContents::describeRay(const std::array<float, 3>& from, const std::array<float, 3>& direction, float length) const
{
    std::string text = "the terrain rings last made at frames";
    for (const auto& ring : terrain) text += " " + std::to_string(ring.refreshed);
    text += "\n";

    // The game's models whose world bounds the ray crosses: whether the scene has them, and each mesh.
    struct Crossed
    {
        float enter;
        const ModelInstance* placement;
    };
    std::vector<Crossed> crossed;
    std::uint32_t unknown{};
    for (const auto& placement : placements)
    {
        const auto found = models.find(placement.hash);
        if (found == models.end())
        {
            ++unknown;
            continue;
        }

        // A model all of whose meshes were left out has nothing to cross.
        if (found->second.geometry->parts.empty()) continue;

        // The box's corners through the transform, then the slab test.
        const auto& b = found->second.bounds;
        float lowest[3]{FLT_MAX, FLT_MAX, FLT_MAX}, highest[3]{-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (int corner = 0; corner < 8; ++corner)
        {
            const float local[3]{b[(corner & 1) ? 3 : 0], b[(corner & 2) ? 4 : 1], b[(corner & 4) ? 5 : 2]};
            for (int row = 0; row < 3; ++row)
            {
                const auto* t = &placement.transform[row * 4];
                const float world = t[0] * local[0] + t[1] * local[1] + t[2] * local[2] + t[3];
                lowest[row] = std::min(lowest[row], world);
                highest[row] = std::max(highest[row], world);
            }
        }

        float enter = 0, leave = length;
        for (int axis = 0; axis < 3 && enter <= leave; ++axis)
        {
            if (std::abs(direction[axis]) < 1e-8f)
            {
                if (from[axis] < lowest[axis] || from[axis] > highest[axis]) enter = leave + 1;
                continue;
            }

            float t0 = (lowest[axis] - from[axis]) / direction[axis], t1 = (highest[axis] - from[axis]) / direction[axis];
            if (t0 > t1) std::swap(t0, t1);
            enter = std::max(enter, t0);
            leave = std::min(leave, t1);
        }

        if (enter <= leave) crossed.push_back({enter, &placement});
    }

    std::sort(crossed.begin(), crossed.end(), [](const Crossed& a, const Crossed& b) { return a.enter < b.enter; });
    text += std::format("the game's models whose bounds the ray crosses: {}{}", crossed.size(),
        unknown ? std::format(" ({} placements of models not read yet)", unknown) : "");
    for (std::size_t i = 0; i < crossed.size() && i < 8; ++i)
    {
        const auto& record = models.at(crossed[i].placement->hash);
        text += std::format("{} {:x} at {:.1f} m ({}):", i ? ";" : ":", crossed[i].placement->hash, crossed[i].enter,
            !record.blas ? "waiting for room" : !memory.ready(*record.blas) ? "waiting for its structure" : "in the scene");
        for (const auto& part : record.geometry->parts)
        {
            const auto texture = textures.find(part.baseTexture);
            const bool isTextured = !part.uvs.empty() && texture != textures.end() && texture->second.slot;
            text += std::format(" mesh {} {} (alpha {:#04x} shadow {:#04x}), {} {} triangles", part.mesh, isCutOut(part.role) ? "cut-out" : "opaque",
                part.alphaBits, part.shadowBits, !part.baseTexture ? "no base texture" : isTextured ? "its texture copied" : "its texture not copied yet",
                part.indices.size() / 3);
        }

        for (const auto& mesh : record.geometry->dropped)
            text += std::format(" mesh {} left out (alpha {:#04x} shadow {:#04x})", mesh.mesh, mesh.alphaBits, mesh.shadowBits);
    }

    return {text, crossed.empty() ? 0 : crossed.front().placement->hash};
}

void SceneContents::dumpModels(const std::filesystem::path& folder) const
{
    std::unordered_map<std::uint64_t, std::vector<const ModelInstance*>> placed;
    for (const auto& placement : placements) placed[placement.hash].push_back(&placement);

    std::ofstream file(folder / "models.txt", std::ios::trunc);
    for (const auto& [hash, record] : models)
    {
        const auto found = placed.find(hash);
        const auto count = found == placed.end() ? std::size_t{} : found->second.size();
        file << std::format("model {:x}, placed {} times", hash, count);
        for (std::size_t i = 0; i < count && i < 4; ++i)
        {
            const auto& placement = *found->second[i];
            const auto& t = placement.transform;
            file << std::format(" ({:.1f} {:.1f} {:.1f})", t[3], t[7], t[11]);
            if (placement.hiddenGroups) file << std::format(" hiding groups {:#x}", placement.hiddenGroups);
        }

        file << '\n';
        for (const auto& part : record.geometry->parts)
            file << std::format("  mesh {} group {} first {} count {} technique {:x} alpha {:#04x} shadow {:#04x}{}\n", part.mesh, part.group, part.firstIndex,
                part.indexCount, part.technique, part.alphaBits, part.shadowBits, isCutOut(part.role) ? " cut-out" : "");
        for (const auto& mesh : record.geometry->dropped)
            file << std::format("  mesh {} left out, technique {:x} alpha {:#04x} shadow {:#04x}\n", mesh.mesh, mesh.technique, mesh.alphaBits, mesh.shadowBits);
    }
}
}
