// What the ray-traced scene takes of the game's model files; scene_description.hpp describes it.
#include "passes/ray_tracing/scene_description.hpp"

#include "passes/ray_tracing/shaders/gi_shared.h"

#include <algorithm>
#include <cstring>

namespace phantom_fx::scene_description
{
namespace
{
// An FMDL mesh's shadow bits (its second byte): bit 0, drawn in the main view only, never into the
// shadow maps; bit 1, drawn into the shadow maps only, a stand-in for what the main view shows.
constexpr std::uint8_t CastsNoShadow = 0x01, ShadowMapsOnly = 0x02;
// Its alpha bits (its first byte): glass, cut out (the shadow pass tests its alpha and draws both its
// faces), translucent (never drawn into the shadow maps) and decals (cast or not by the material's
// technique, one-sided as solid meshes are).
constexpr std::uint8_t GlassAlpha = 0x10, CutOutAlpha = 0x20, TranslucentAlpha = 0x40, DecalAlpha = 0x80;
// The StrCode64s of a material's texture parameters "Base_Tex_SRGB" (the colour, and a cut-out mesh's
// alpha) and "Mask_Tex_LIN" (a shadow-only mesh's mask, its only texture, often the visible mesh's
// colour: the shadow pass cuts by its alpha, as ModelForward_Shadow's pixel shader samples t0.w):
// Fox Engine's string hash, CityHash64 1.0.3 of the name and its terminating zero with seeds
// 0x9AE16A3B2F90404F and (first character << 16) + length, 48 bits kept.
constexpr std::uint64_t BaseTextureParameter = 0x41EA7BE81B61, MaskTextureParameter = 0x3ED9B75CD480;

// The alpha below which a cut-out mesh lets light through: the game's shadow pass discards below half its
// per-draw m_param.w (ModelForward_Shadow), measured 1 on cut-out meshes' draws (0 where it tests
// nothing); its G-buffer below m_param.w times a dither averaging a half.
constexpr float CutOutThreshold = 0.5f;

// A mesh's role by its alpha and shadow bits, or none for a mesh the scene drops. The scene takes the
// meshes the game draws, in its main view or its shadow maps, as its draws showed them mesh by mesh
// (docs/DEVELOPMENT.md, "Scene objects", "Ray-traced scene"; the cases measured are
// tests/scene_description_tests.cpp's):
// - Glass the main view alone shows is dropped: the game never casts it.
// - A character is taken as the main view shows it: its stand-ins dropped, its see-through meshes cut
//   out by their alpha, and those the game never casts with, its hair, let light through in part.
// - A rigid model is taken as the game casts: see-through meshes are dropped, and cut-out ones and
//   decals the main view alone shows. Every other decal is taken as cut out, until the game's own
//   per-mesh pass mask is read. A solid mesh the main view alone shows (a room's inner walls) stops
//   every ray but those towards lights.
// - A rigid model's solid stand-in is kept for the sun's rays alone: the game draws it into every sun
//   cascade, and the main-view-only meshes it stands in for pass them. A lamp's rays pass it: an offset,
//   simpler copy of the main view's meshes, it kept a lamp off a wall where nothing was drawn. Where the
//   main view's cut-out meshes are dropped, it is all the rays have of them: it is kept for every ray
//   towards a light. One the main view draws too is dropped (no ray would meet it). A cut-out stand-in
//   stays in (a pylon's lattice casts through one). Should the sun-only rule mislead too, the game's
//   per-draw pass masks say which shadow maps each mesh is drawn into (docs/DEVELOPMENT.md, "Ray-traced
//   shadows").
// - The object brush's bushes and grass cast no shadow in the game: their meshes are taken as leaves
//   all the same, for the rays' shadows; the brush's props are solid.
std::optional<MeshRole> roleOf(std::uint8_t alpha, std::uint8_t shadow, bool isCharacter, bool isVegetation, bool areMainViewCutOutsDropped)
{
    const bool isCutOut = (alpha & (CutOutAlpha | DecalAlpha)) != 0;
    const bool isSeeThrough = (alpha & TranslucentAlpha) != 0;
    const bool isMainViewOnly = (shadow & CastsNoShadow) && !isVegetation;
    const bool isStandIn = (shadow & ShadowMapsOnly) != 0;
    if (isMainViewOnly && (alpha & GlassAlpha)) return std::nullopt;

    if (isCharacter)
    {
        if (isStandIn) return std::nullopt;
        if (!isCutOut && !isSeeThrough) return MeshRole::Solid;

        return (isMainViewOnly || isSeeThrough) ? MeshRole::Hair : isVegetation ? MeshRole::Leaf : MeshRole::CutOut;
    }

    if (isSeeThrough || (isCutOut && isMainViewOnly)) return std::nullopt;
    if (isCutOut) return isVegetation ? MeshRole::Leaf : MeshRole::CutOut;
    if (isStandIn)
    {
        if (isMainViewOnly) return std::nullopt;

        return areMainViewCutOutsDropped ? MeshRole::ShadowOnly : MeshRole::SunShadowOnly;
    }

    return isMainViewOnly ? MeshRole::MainViewOnly : MeshRole::Solid;
}

// The game draws a mesh with the cut-out bit into its shadow maps two-sided, every other with its front
// faces culled, decals and Quiet's alpha-tested body too (four captures, 2026-10-29). Leaves and hair,
// thin cards the game never casts with, cast from both sides.
std::uint32_t flagsOf(MeshRole role, std::uint8_t alpha)
{
    switch (role)
    {
    case MeshRole::CutOut:
        return GI_GEOMETRY_CUT_OUT | ((alpha & CutOutAlpha) ? GI_GEOMETRY_TWO_SIDED : 0u);
    case MeshRole::Leaf:
        return GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_LEAF | GI_GEOMETRY_TWO_SIDED;
    case MeshRole::Hair:
        return GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_HAIR | GI_GEOMETRY_TWO_SIDED;
    case MeshRole::MainViewOnly:
        return GI_GEOMETRY_NO_SHADOW;
    case MeshRole::ShadowOnly:
        return GI_GEOMETRY_SHADOW_ONLY;
    case MeshRole::SunShadowOnly:
        return GI_GEOMETRY_SHADOW_ONLY | GI_GEOMETRY_SUN_ONLY;
    case MeshRole::Solid:
        break;
    }

    return 0;
}

// Little-endian fields of a model file, bounds-checked.
struct FileReader
{
    std::span<const std::uint8_t> bytes;

    template<class T> bool at(std::uint64_t offset, T& out) const noexcept
    {
        if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) return false;

        std::memcpy(&out, bytes.data() + offset, sizeof(T));
        return true;
    }
};

// Section 0's blocks (FMDL 2.0x, as the community's FmdlTool reads them): where each starts and how
// many entries it has.
struct Block
{
    std::uint64_t offset{};
    std::uint32_t count{};
};

struct FileLayout
{
    std::array<Block, 32> blocks{};
    std::uint64_t bufferData{}; // Section 1's vertex and index data
    bool hasBones{};
};

std::optional<FileLayout> layoutOf(const FileReader& file) noexcept
{
    std::uint32_t magic{}, count0{}, count1{}, offset0{}, offset1{};
    std::uint64_t sectionInfo{};
    if (!file.at(0, magic) || magic != 0x4C444D46 || !file.at(8, sectionInfo) || !file.at(32, count0) || !file.at(36, count1) || !file.at(40, offset0)
        || !file.at(48, offset1) || count0 > 64 || count1 > 64)
        return std::nullopt;

    FileLayout layout;
    for (std::uint32_t i = 0; i < count0; ++i)
    {
        std::uint16_t type{}, count{};
        std::uint32_t offset{};
        if (!file.at(sectionInfo + 8 * i, type) || !file.at(sectionInfo + 8 * i + 2, count) || !file.at(sectionInfo + 8 * i + 4, offset)) return std::nullopt;

        if (type == 0) layout.hasBones = true;
        if (type < layout.blocks.size()) layout.blocks[type] = {std::uint64_t{offset0} + offset, count};
    }

    for (std::uint32_t i = 0; i < count1; ++i)
    {
        std::uint32_t type{}, offset{};
        const auto at = sectionInfo + 8ull * count0 + 12ull * i;
        if (!file.at(at, type) || !file.at(at + 4, offset)) return std::nullopt;

        if (type == 2) layout.bufferData = std::uint64_t{offset1} + offset;
    }

    if (!layout.bufferData) return std::nullopt;

    return layout;
}

// A vertex stream of a mesh: where its first vertex's element starts, the stride, and the element's type.
struct Stream
{
    std::uint64_t at{};
    std::uint32_t stride{};
    std::uint8_t dataType{};
};

// Each mesh's group (block 2, 32 bytes: the group at +4, the count of its meshes at +6, the first at +8).
std::optional<std::vector<std::uint32_t>> meshGroupsOf(const FileReader& file, const FileLayout& layout)
{
    constexpr int GroupMeshes = 2, Meshes = 3;
    const auto& b = layout.blocks;
    std::vector<std::uint32_t> groups(b[Meshes].count);
    for (std::uint32_t i = 0; i < b[GroupMeshes].count; ++i)
    {
        const auto at = b[GroupMeshes].offset + 32ull * i;
        std::uint16_t group{}, count{}, first{};
        if (!file.at(at + 4, group) || !file.at(at + 6, count) || !file.at(at + 8, first)) return std::nullopt;

        for (std::uint32_t mesh = first; mesh < std::uint32_t{first} + count && mesh < groups.size(); ++mesh) groups[mesh] = group;
    }

    return groups;
}

// The path hash of a mesh's texture for a parameter: its material instance's (block 4: texture count at
// +6, first texture parameter at +8) texture parameter of that name (block 7: the name's StrCode64
// index into block 22, the texture's into block 6), whose texture (block 6: its path's index into
// block 21) has that path hash. 0 when the mesh has none.
std::uint64_t textureOf(const FileReader& file, const FileLayout& layout, std::uint16_t materialInstance, std::uint64_t parameter)
{
    const auto& b = layout.blocks;
    constexpr int MaterialInstances = 4, Textures = 6, Parameters = 7, PathCodes = 21, StrCodes = 22;
    std::uint8_t count{};
    std::uint16_t first{};
    if (materialInstance >= b[MaterialInstances].count || !file.at(b[MaterialInstances].offset + 16ull * materialInstance + 6, count)
        || !file.at(b[MaterialInstances].offset + 16ull * materialInstance + 8, first))
        return 0;

    for (std::uint32_t i = first; i < std::uint32_t{first} + count && i < b[Parameters].count; ++i)
    {
        std::uint16_t name{}, texture{}, path{};
        std::uint64_t nameHash{}, pathHash{};
        if (!file.at(b[Parameters].offset + 4ull * i, name) || !file.at(b[Parameters].offset + 4ull * i + 2, texture) || name >= b[StrCodes].count
            || !file.at(b[StrCodes].offset + 8ull * name, nameHash) || nameHash != parameter)
            continue;

        if (texture < b[Textures].count && file.at(b[Textures].offset + 4ull * texture + 2, path) && path < b[PathCodes].count
            && file.at(b[PathCodes].offset + 8ull * path, pathHash))
            return pathHash;
    }

    return 0;
}

// Diagnostics: the StrCode64 of a mesh's material's technique (as fox3DDF_Blin_NC): its material
// instance's material (block 4: material at +4), whose type (block 8: name at +0, type at +2) indexes
// block 22. 0 when the file does not say.
std::uint64_t techniqueOf(const FileReader& file, const FileLayout& layout, std::uint16_t materialInstance)
{
    const auto& b = layout.blocks;
    constexpr int MaterialInstances = 4, Materials = 8, StrCodes = 22;
    std::uint16_t material{}, type{};
    std::uint64_t technique{};
    if (materialInstance >= b[MaterialInstances].count || !file.at(b[MaterialInstances].offset + 16ull * materialInstance + 4, material)
        || material >= b[Materials].count || !file.at(b[Materials].offset + 4ull * material + 2, type) || type >= b[StrCodes].count
        || !file.at(b[StrCodes].offset + 8ull * type, technique))
        return 0;

    return technique;
}

// A skinned mesh's vertices' bones and weights (ModelPart::bones, weights): its four bone numbers a
// vertex index its bone group (block 5, 68 bytes: the count at +2, then 32 of the model's bone numbers;
// the mesh's group at +6), as the game's palette for the mesh is the model's through the group.
// Without numbers a vertex follows the group's first bone; without weights, its first number alone.
bool readSkin(const FileReader& file, const Block& boneGroups, std::uint64_t meshAt, std::uint32_t bones, std::optional<Stream> numbers,
    std::optional<Stream> weights, ModelPart& part)
{
    std::uint16_t group{}, count{};
    if (!file.at(meshAt + 6, group) || group >= boneGroups.count || !file.at(boneGroups.offset + 68ull * group + 2, count)) return false;

    std::array<std::uint16_t, 32> members{};
    for (std::uint32_t i = 0; i < std::min<std::uint32_t>(count, 32); ++i)
        if (!file.at(boneGroups.offset + 68ull * group + 4 + 2ull * i, members[i])) return false;

    part.boneGroup.assign(members.begin(), members.begin() + std::min<std::uint32_t>(count, 32));

    const auto vertexCount = part.vertices.size();
    part.bones.resize(vertexCount);
    part.weights.resize(vertexCount);
    for (std::size_t v = 0; v < vertexCount; ++v)
    {
        std::array<std::uint8_t, 4> local{};
        std::uint32_t weight = 0xFF;
        if ((numbers && !file.at(numbers->at + v * numbers->stride, local)) || (weights && !file.at(weights->at + v * weights->stride, weight))) return false;

        std::uint64_t packed{};
        for (int k = 0; k < 4; ++k)
        {
            const std::uint64_t bone = local[k] < std::min<std::uint32_t>(count, 32) && members[local[k]] < bones ? members[local[k]] : 0;
            packed |= bone << (16 * k);
        }

        part.bones[v] = packed;
        part.weights[v] = weight;
    }

    return true;
}
}

ModelFile fileKindOf(std::span<const std::uint8_t> file)
{
    const auto layout = layoutOf(FileReader{file});
    if (!layout) return ModelFile::Unreadable;

    return layout->hasBones ? ModelFile::Skinned : ModelFile::Rigid;
}

// Each mesh the scene takes (roleOf) with its vertices and their texture coordinates, its mesh group,
// and the indices of its most detailed triangles, as the game keeps them; a skinned model's vertices
// also with their bones (readSkin).
std::shared_ptr<const Model> describeModel(std::uint64_t hash, std::span<const std::uint8_t> bytes, Source source)
{
    const FileReader file{bytes};
    const auto layout = layoutOf(file);
    if (!layout) return nullptr;

    const auto& b = layout->blocks;
    constexpr int Bones = 0, Meshes = 3, BoneGroups = 5, Assignments = 9, MeshFormats = 10, VertexFormats = 11, BufferOffsets = 14, FaceInfos = 17;
    // Vertex format usages and data types.
    constexpr std::uint8_t Position = 0, BoneWeights = 1, BoneNumbers = 7, Uv0 = 8, ThreeFloats = 1, TwoHalves = 7, FourUnorms = 8, FourBytes = 9;
    if (!b[BufferOffsets].count) return nullptr;

    std::uint32_t facesOffset{};
    const auto faceBuffer = std::min<std::uint32_t>(b[BufferOffsets].count - 1, 2);
    if (!file.at(b[BufferOffsets].offset + 16ull * faceBuffer + 8, facesOffset)) return nullptr;

    auto model = std::make_shared<Model>();
    model->hash = hash;
    model->bones = layout->hasBones ? std::min(b[Bones].count, MaxBones) : 0;
    const auto meshGroups = meshGroupsOf(file, *layout);
    if (!meshGroups) return nullptr;

    // Whether the main view shows cut-out meshes the scene drops: the solid stand-ins are then kept (roleOf).
    const bool isVegetation = source == Source::ObjectBrush;
    bool areMainViewCutOutsDropped{};
    for (std::uint32_t mesh = 0; mesh < b[Meshes].count && !isVegetation; ++mesh)
    {
        std::uint8_t alpha{}, shadow{};
        if (file.at(b[Meshes].offset + 48ull * mesh, alpha) && file.at(b[Meshes].offset + 48ull * mesh + 1, shadow))
            areMainViewCutOutsDropped |= (shadow & CastsNoShadow) && (alpha & (CutOutAlpha | DecalAlpha));
    }

    for (std::uint32_t mesh = 0; mesh < b[Meshes].count; ++mesh)
    {
        const auto meshAt = b[Meshes].offset + 48ull * mesh;
        std::uint8_t alpha{}, shadow{};
        std::uint16_t materialInstance{}, vertexCount{};
        std::uint32_t firstFace{};
        std::uint64_t firstFaceInfo{};
        if (!file.at(meshAt, alpha) || !file.at(meshAt + 1, shadow) || !file.at(meshAt + 4, materialInstance) || !file.at(meshAt + 10, vertexCount)
            || !file.at(meshAt + 16, firstFace) || !file.at(meshAt + 24, firstFaceInfo))
            return nullptr;

        const auto role = roleOf(alpha, shadow, model->bones != 0, isVegetation, areMainViewCutOutsDropped);
        if (!role)
        {
            model->dropped.push_back({mesh, alpha, shadow, techniqueOf(file, *layout, materialInstance)});
            continue;
        }

        // The mesh's streams: its mesh formats in order, each followed by as many of its vertex
        // formats as it says.
        const auto assignmentAt = b[Assignments].offset + 8ull * mesh;
        std::uint8_t meshFormatCount{};
        std::uint16_t firstMeshFormat{}, firstVertexFormat{};
        if (mesh >= b[Assignments].count || !file.at(assignmentAt, meshFormatCount) || !file.at(assignmentAt + 4, firstMeshFormat)
            || !file.at(assignmentAt + 6, firstVertexFormat))
            return nullptr;

        std::optional<Stream> positions, uvs, weights, numbers;
        std::uint32_t vertexFormat = firstVertexFormat;
        for (std::uint32_t i = firstMeshFormat; i < std::uint32_t{firstMeshFormat} + meshFormatCount && i < b[MeshFormats].count; ++i)
        {
            const auto at = b[MeshFormats].offset + 8ull * i;
            std::uint8_t bufferOffset{}, formats{}, stride{};
            std::uint32_t offset{}, buffer{};
            if (!file.at(at, bufferOffset) || !file.at(at + 1, formats) || !file.at(at + 2, stride) || !file.at(at + 4, offset)) return nullptr;
            if (bufferOffset >= b[BufferOffsets].count || !file.at(b[BufferOffsets].offset + 16ull * bufferOffset + 8, buffer)) return nullptr;

            for (std::uint32_t k = 0; k < formats && vertexFormat < b[VertexFormats].count; ++k, ++vertexFormat)
            {
                const auto formatAt = b[VertexFormats].offset + 4ull * vertexFormat;
                std::uint8_t usage{}, dataType{};
                std::uint16_t element{};
                if (!file.at(formatAt, usage) || !file.at(formatAt + 1, dataType) || !file.at(formatAt + 2, element)) return nullptr;

                const Stream stream{layout->bufferData + buffer + offset + element, stride, dataType};
                if (usage == Position && !positions) positions = stream;
                if (usage == Uv0 && !uvs) uvs = stream;
                if (usage == BoneWeights && !weights) weights = stream;
                if (usage == BoneNumbers && !numbers) numbers = stream;
            }
        }

        if (!positions || positions->dataType != ThreeFloats || firstFaceInfo >= b[FaceInfos].count) return nullptr;

        std::uint32_t lodFirst{}, lodCount{};
        if (!file.at(b[FaceInfos].offset + 8 * firstFaceInfo, lodFirst) || !file.at(b[FaceInfos].offset + 8 * firstFaceInfo + 4, lodCount)) return nullptr;

        // A mesh without two-half texture coordinates has neither alpha nor albedo of its own: a cut-out
        // one is left to the shadow map, any one takes its mean albedo from the screen.
        auto& part = model->parts.emplace_back();
        part.mesh = mesh;
        part.group = (*meshGroups)[mesh];
        part.role = *role;
        part.alphaBits = alpha;
        part.shadowBits = shadow;
        part.technique = techniqueOf(file, *layout, materialInstance);
        part.baseTexture = textureOf(file, *layout, materialInstance, BaseTextureParameter);
        if (!part.baseTexture)
        {
            part.baseTexture = textureOf(file, *layout, materialInstance, MaskTextureParameter);
            part.textureIsMask = part.baseTexture != 0;
        }

        const bool textured = uvs && uvs->dataType == TwoHalves;
        part.vertices.resize(vertexCount);
        if (textured) part.uvs.resize(vertexCount);
        for (std::uint32_t v = 0; v < vertexCount; ++v)
            if (!file.at(positions->at + std::uint64_t{v} * positions->stride, part.vertices[v])
                || (textured && !file.at(uvs->at + std::uint64_t{v} * uvs->stride, part.uvs[v])))
                return nullptr;

        const auto indices = layout->bufferData + facesOffset + 2ull * (std::uint64_t{firstFace} + lodFirst);
        part.firstIndex = firstFace + lodFirst;
        part.indexCount = lodCount;
        part.indices.resize(lodCount / 3 * 3);
        for (std::uint32_t i = 0; i < part.indices.size(); ++i)
            if (!file.at(indices + 2ull * i, part.indices[i]) || part.indices[i] >= vertexCount) return nullptr;

        if (model->bones && !readSkin(file, b[BoneGroups], meshAt, model->bones, numbers && numbers->dataType == FourBytes ? numbers : std::nullopt,
                                weights && weights->dataType == FourUnorms ? weights : std::nullopt, part))
            return nullptr;

        if (part.indices.empty()) model->parts.pop_back();
    }

    return model;
}

bool isCutOut(MeshRole role)
{
    return role == MeshRole::CutOut || role == MeshRole::Leaf || role == MeshRole::Hair;
}

Material materialOf(const ModelPart& part, std::optional<std::uint32_t> textureIndex)
{
    Material material{.flags = flagsOf(part.role, part.alphaBits) | (part.uvs.empty() ? 0u : GI_GEOMETRY_UV)};
    if (!textureIndex || part.uvs.empty()) return material;

    material.texture = *textureIndex + 1;
    if (part.textureIsMask) material.flags |= GI_GEOMETRY_MASK;
    if (isCutOut(part.role))
    {
        material.flags |= GI_GEOMETRY_ALPHA_TEXTURE;
        material.alphaThreshold = CutOutThreshold;
    }

    return material;
}
}
