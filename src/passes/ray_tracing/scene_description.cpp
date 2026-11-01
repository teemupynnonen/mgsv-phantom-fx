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
// Its alpha bits (its first byte): drawn forward, after the G-buffer (glass, and most of all fox3DFW_ConstantSRGB), cut out
// (the shadow pass tests its alpha), translucent (never drawn into the shadow maps) and decals (cast or not by the
// material's technique).
constexpr std::uint8_t ForwardAlpha = 0x10, CutOutAlpha = 0x20, TranslucentAlpha = 0x40, DecalAlpha = 0x80;
// The StrCode64s of a material's texture parameters "Base_Tex_SRGB" (the colour, and a cut-out mesh's
// alpha) and "Mask_Tex_LIN" (a shadow-only mesh's mask, its only texture, often the visible mesh's
// colour: the shadow pass cuts by its alpha, as ModelForward_Shadow's pixel shader samples t0.w):
// Fox Engine's string hash, CityHash64 1.0.3 of the name and its terminating zero with seeds
// 0x9AE16A3B2F90404F and (first character << 16) + length, 48 bits kept.
constexpr std::uint64_t BaseTextureParameter = 0x41EA7BE81B61, MaskTextureParameter = 0x3ED9B75CD480;
// And "SpecularMap_Tex_LIN" (the G-buffer's g_tex_srm: r the specular rate, g the roughness), "NormalMap_Tex_NRM", and
// the vectors "MatParamIndex_0" (x the material table's row, the G-buffer's cb4[0].x), "SelfColor" and
// "SelfColorIntensity" (fox3dfw_constant*: the texture times the colour times the intensity's x).
constexpr std::uint64_t SpecularTextureParameter = 0x104D6B98B10E, NormalTextureParameter = 0xCC4305511AE0;
constexpr std::uint64_t MaterialIndexParameter = 0xCAB8E38487FF, SelfColorParameter = 0x557B9224571E, SelfColorIntensityParameter = 0xE40A3A2518CF;
// Glass: "GlassColor", which every glass technique's material gives and no other's (the game's files, 2026-10-31), and
// "Base_Tex_LIN" (the game's t0, how much of the surface reflects). A mirror is known by its technique,
// "fox3DFW_ParallaxReflection", as "HeightScale" is not its alone. Its Base_Tex_SRGB is a painted picture of the room, its mask
// is on the second texture coordinates, which the scene does not keep, and it has no row of the material table.
constexpr std::uint64_t GlassColorParameter = 0xBE61C83A81D, GlassMaskParameter = 0x679A3387A4DF, MirrorTechnique = 0x2EB4D1D33DF8;
// A mesh's layer, which its G-buffer shader lays over the base texture (fox3ddf_cloth*, fox3ddf_blin_*layer* and more, 52
// shaders): "Layer_Tex_SRGB", "LayerMask_Tex_LIN", and the layer's coordinates, (second coordinates + shift) x repeat, by
// "URepeat_UV", "VRepeat_UV", "UShift_UV" and "VShift_UV". It can be all of a mesh's colour: the base of Kaz's beige coat is white.
constexpr std::uint64_t LayerTextureParameter = 0xFB6E9826DBE6, LayerMaskTextureParameter = 0xD5C6F91A8B72;
constexpr std::uint64_t URepeatParameter = 0xE544FE2CA129, VRepeatParameter = 0xE7C7FA3697B2, UShiftParameter = 0x7B8B5E0E16EB, VShiftParameter = 0x38DA962E408E;
// The techniques known to use their layer, as no parameter tells: 58 F10 records, each mesh's technique by its pixel shader in
// the frame's G-buffer draws, 2026-11-01. A material can name a layer its technique may not read (a white base under a black
// layer, technique 0xC0CE81F00048, in no record): any other technique keeps its base texture. These multiply it (the pixel
// shaders' MulLayerTexture), and these lay it in the base texture's place (BlendLayerTexture, SRGBBlendLayerTexture).
constexpr std::array<std::uint64_t, 15> MultipliedLayerTechniques{0xE2C9B71532D1, 0xD41DF652A584, 0x74891B27D58C, 0x88C852A00C01, 0xB17EB6AEFBB4, 0xD5F0795801BA,
    0xE910A72719EF, 0x16C65FA108DD, 0xAA02EC2A2C1D, 0xC71F96A72DD9, 0x1B8D4DA0667B, 0xA83595F3456F, 0x8DF300329F91, 0x7E8ECE300722, 0x339701C2A10D};
constexpr std::array<std::uint64_t, 14> BlendedLayerTechniques{0x3FBC9A12FB36, 0xCDDF56CB51E3, 0x4E818B32A91D, 0x7B049948758F, 0xE14611544E45, 0xEF52ACD48A68,
    0xCB4EF239308D, 0xD1B559F90CB6, 0xC887CFC09D78, 0x54214F48645B, 0xD8DE23B18660, 0x9BF9366EE4C1, 0xE7C991D84F41, 0x203D1676BC8E};
// Its alpha bits' additive blend: a glow card around a lamp (four vertices), no surface.
constexpr std::uint8_t AdditiveAlpha = 0x01;
// A self colour times intensity below this lights nothing: the sky's dome and the moon have 1, a mesh painted black
// 0.16. The sun's light is about 20,000, and a lamp's bulb 80 and up.
constexpr float LeastEmission = 10;

// The alpha below which a cut-out mesh lets light through: the game's shadow pass discards below half its
// per-draw m_param.w (ModelForward_Shadow), measured 1 on cut-out meshes' draws (0 where it tests
// nothing); its G-buffer below m_param.w times a dither averaging a half.
constexpr float CutOutThreshold = 0.5f;

// A mesh's role by its alpha and shadow bits, or none for a mesh the scene drops. The scene takes the
// meshes the game draws, in its main view or its shadow maps, as its draws showed them mesh by mesh
// (docs/DEVELOPMENT.md, "Scene objects", "Ray-traced scene"; the cases measured are
// tests/scene_description_tests.cpp's):
// - Glass and mirrors are taken for their material, whatever their bytes, for the reflections: every other ray passes them.
// - Any other mesh drawn forward that the main view alone shows is dropped: the game never casts it.
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
// - A self-lit mesh is taken for its light, whatever else it is, but for an additive glow card, which is no surface.
std::optional<MeshRole> roleOf(std::uint8_t alpha, std::uint8_t shadow, bool isCharacter, bool isVegetation, bool areMainViewCutOutsDropped, bool isSelfLit,
    std::optional<MeshRole> materialRole)
{
    if (isSelfLit && !(alpha & AdditiveAlpha)) return MeshRole::Emissive;
    if (materialRole) return materialRole;

    const bool isCutOut = (alpha & (CutOutAlpha | DecalAlpha)) != 0;
    const bool isSeeThrough = (alpha & TranslucentAlpha) != 0;
    const bool isMainViewOnly = (shadow & CastsNoShadow) && !isVegetation;
    const bool isStandIn = (shadow & ShadowMapsOnly) != 0;
    if (isMainViewOnly && (alpha & ForwardAlpha)) return std::nullopt;

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

std::uint32_t flagsOf(MeshRole role)
{
    switch (role)
    {
    case MeshRole::CutOut:
        return GI_GEOMETRY_CUT_OUT;
    case MeshRole::Leaf:
        return GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_LEAF;
    case MeshRole::Hair:
        return GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_HAIR;
    case MeshRole::Glass:
        return GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_GLASS;
    case MeshRole::Mirror:
        return GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_GLASS | GI_GEOMETRY_MIRROR;
    case MeshRole::MainViewOnly:
        return GI_GEOMETRY_NO_SHADOW;
    case MeshRole::Emissive:
        return GI_GEOMETRY_NO_SHADOW | GI_GEOMETRY_EMISSIVE;
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
    std::uint64_t vectors{};    // Section 1's material parameters: four floats each
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
        if (type == 0) layout.vectors = std::uint64_t{offset1} + offset;
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

// A mesh's vector for a parameter: its material instance's (block 4: vector count at +7, first vector parameter at
// +10) parameter of that name (block 7: the name's StrCode64 index into block 22, the vector's index into section 1's
// vectors). Nullopt when the mesh has none.
std::optional<std::array<float, 4>> vectorOf(const FileReader& file, const FileLayout& layout, std::uint16_t materialInstance, std::uint64_t parameter)
{
    const auto& b = layout.blocks;
    constexpr int MaterialInstances = 4, Parameters = 7, StrCodes = 22;
    std::uint8_t count{};
    std::uint16_t first{};
    if (!layout.vectors || materialInstance >= b[MaterialInstances].count || !file.at(b[MaterialInstances].offset + 16ull * materialInstance + 7, count)
        || !file.at(b[MaterialInstances].offset + 16ull * materialInstance + 10, first))
        return std::nullopt;

    for (std::uint32_t i = first; i < std::uint32_t{first} + count && i < b[Parameters].count; ++i)
    {
        std::uint16_t name{}, vector{};
        std::uint64_t nameHash{};
        if (!file.at(b[Parameters].offset + 4ull * i, name) || !file.at(b[Parameters].offset + 4ull * i + 2, vector) || name >= b[StrCodes].count
            || !file.at(b[StrCodes].offset + 8ull * name, nameHash) || nameHash != parameter)
            continue;

        std::array<float, 4> value{};
        if (file.at(layout.vectors + 16ull * vector, value)) return value;
    }

    return std::nullopt;
}

// A self-lit mesh's light in the game's units; all zero on any other mesh (LeastEmission).
std::array<float, 3> emissionOf(const FileReader& file, const FileLayout& layout, std::uint16_t materialInstance)
{
    const auto color = vectorOf(file, layout, materialInstance, SelfColorParameter);
    const auto intensity = vectorOf(file, layout, materialInstance, SelfColorIntensityParameter);
    if (!color || !intensity) return {};

    const std::array emission{(*color)[0] * (*intensity)[0], (*color)[1] * (*intensity)[0], (*color)[2] * (*intensity)[0]};
    return std::max({emission[0], emission[1], emission[2]}) >= LeastEmission ? emission : std::array<float, 3>{};
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
    constexpr std::uint8_t Position = 0, BoneWeights = 1, Normal = 2, BoneNumbers = 7, Uv0 = 8, Uv1 = 9, Tangent = 14, ThreeFloats = 1, FourHalves = 6, TwoHalves = 7,
                           FourUnorms = 8, FourBytes = 9;
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

        const auto emission = emissionOf(file, *layout, materialInstance);
        const bool isSelfLit = emission != std::array<float, 3>{};
        const auto technique = techniqueOf(file, *layout, materialInstance);
        std::optional<MeshRole> materialRole;
        if (technique == MirrorTechnique) materialRole = MeshRole::Mirror;
        else if (vectorOf(file, *layout, materialInstance, GlassColorParameter)) materialRole = MeshRole::Glass;

        const auto role = roleOf(alpha, shadow, model->bones != 0, isVegetation, areMainViewCutOutsDropped, isSelfLit, materialRole);
        if (!role)
        {
            model->dropped.push_back({mesh, alpha, shadow, technique});
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

        std::optional<Stream> positions, uvs, secondUvs, weights, numbers, normals, tangents;
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
                if (usage == Uv1 && !secondUvs) secondUvs = stream;
                if (usage == BoneWeights && !weights) weights = stream;
                if (usage == BoneNumbers && !numbers) numbers = stream;
                if (usage == Normal && !normals) normals = stream;
                if (usage == Tangent && !tangents) tangents = stream;
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
        part.technique = technique;
        const bool isGlass = *role == MeshRole::Glass, isMirror = *role == MeshRole::Mirror;
        part.baseTexture = isMirror ? 0 : textureOf(file, *layout, materialInstance, isGlass ? GlassMaskParameter : BaseTextureParameter);
        if (!part.baseTexture && !isGlass && !isMirror)
        {
            part.baseTexture = textureOf(file, *layout, materialInstance, MaskTextureParameter);
            part.textureIsMask = part.baseTexture != 0;
        }

        part.specularTexture = textureOf(file, *layout, materialInstance, SpecularTextureParameter);
        part.normalTexture = textureOf(file, *layout, materialInstance, NormalTextureParameter);
        if (const auto index = vectorOf(file, *layout, materialInstance, MaterialIndexParameter))
            part.materialIndex = static_cast<std::uint32_t>(std::clamp((*index)[0], 0.0f, 255.0f));
        part.emission = emission;

        const bool textured = uvs && uvs->dataType == TwoHalves;
        const bool hasNormals = normals && tangents && normals->dataType == FourHalves && tangents->dataType == FourHalves;
        const bool isLayerBlended = std::ranges::find(BlendedLayerTechniques, technique) != BlendedLayerTechniques.end();
        const bool usesLayer = isLayerBlended || std::ranges::find(MultipliedLayerTechniques, technique) != MultipliedLayerTechniques.end();
        const auto layer = usesLayer ? textureOf(file, *layout, materialInstance, LayerTextureParameter) : 0;
        const bool isLayered = layer && !isGlass && !isMirror && textured && secondUvs && secondUvs->dataType == TwoHalves;
        if (isLayered)
        {
            const auto mappingBy = [&](std::uint64_t parameter, float otherwise) {
                const auto vector = vectorOf(file, *layout, materialInstance, parameter);
                return vector ? (*vector)[0] : otherwise;
            };

            part.layerTexture = layer;
            part.layerMaskTexture = textureOf(file, *layout, materialInstance, LayerMaskTextureParameter);
            part.layerMapping = {mappingBy(URepeatParameter, 1), mappingBy(VRepeatParameter, 1), mappingBy(UShiftParameter, 0), mappingBy(VShiftParameter, 0)};
            part.isLayerBlended = isLayerBlended;
            part.layerUvs.resize(vertexCount);
        }

        part.vertices.resize(vertexCount);
        if (textured) part.uvs.resize(vertexCount);
        if (hasNormals) part.normals.resize(vertexCount);
        for (std::uint32_t v = 0; v < vertexCount; ++v)
        {
            if (!file.at(positions->at + std::uint64_t{v} * positions->stride, part.vertices[v])
                || (textured && !file.at(uvs->at + std::uint64_t{v} * uvs->stride, part.uvs[v]))
                || (isLayered && !file.at(secondUvs->at + std::uint64_t{v} * secondUvs->stride, part.layerUvs[v])))
                return nullptr;

            if (!hasNormals) continue;

            std::array<std::uint16_t, 4> normal{}, tangent{};
            if (!file.at(normals->at + std::uint64_t{v} * normals->stride, normal) || !file.at(tangents->at + std::uint64_t{v} * tangents->stride, tangent))
                return nullptr;

            std::copy(normal.begin(), normal.end(), part.normals[v].begin());
            std::copy(tangent.begin(), tangent.end(), part.normals[v].begin() + 4);
        }

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

Material materialOf(const ModelPart& part, const MaterialTextures& textures)
{
    Material material{.flags = flagsOf(part.role) | (part.uvs.empty() ? 0u : GI_GEOMETRY_UV), .materialIndex = part.materialIndex, .emission = part.emission};
    if (part.uvs.empty()) return material;

    if (textures.specular) material.specularTexture = *textures.specular + 1;
    if (textures.normal) material.normalTexture = *textures.normal + 1;
    if (!textures.base) return material;

    material.texture = *textures.base + 1;
    if (part.textureIsMask) material.flags |= GI_GEOMETRY_MASK;
    if (textures.layer)
    {
        material.layerTexture = *textures.layer + 1;
        if (part.isLayerBlended) material.flags |= GI_GEOMETRY_LAYER_BLEND;
    }

    if (textures.layerMask) material.layerMaskTexture = *textures.layerMask + 1;
    if (isCutOut(part.role))
    {
        material.flags |= GI_GEOMETRY_ALPHA_TEXTURE;
        material.alphaThreshold = CutOutThreshold;
    }

    return material;
}
}
