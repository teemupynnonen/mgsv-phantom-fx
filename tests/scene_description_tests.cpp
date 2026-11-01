#include "passes/ray_tracing/scene_description.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

namespace
{
using namespace phantom_fx::scene_description;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// The StrCode64s of the texture parameters a material names its textures by (Base_Tex_SRGB, Mask_Tex_LIN, Base_Tex_LIN),
// and the path hash of mesh i's texture, TexturePath + i.
constexpr std::uint64_t BaseTexture = 0x41EA7BE81B61, MaskTexture = 0x3ED9B75CD480, LinearBaseTexture = 0x679A3387A4DF, TexturePath = 0x5000;
// And of the vectors a material gives (SelfColor, SelfColorIntensity, MatParamIndex_0, GlassColor), and a mirror's technique
// (fox3DFW_ParallaxReflection).
constexpr std::uint64_t SelfColor = 0x557B9224571E, SelfColorIntensity = 0xE40A3A2518CF, MaterialIndex = 0xCAB8E38487FF, GlassColor = 0xBE61C83A81D,
                        ParallaxReflection = 0x2EB4D1D33DF8;
// And a layer's (Layer_Tex_SRGB, LayerMask_Tex_LIN, URepeat_UV, VRepeat_UV, UShift_UV, VShift_UV), and the path hashes of mesh i's
// layer and mask, LayerPath + i and LayerMaskPath + i.
constexpr std::uint64_t LayerTexture = 0xFB6E9826DBE6, LayerMaskTexture = 0xD5C6F91A8B72, URepeat = 0xE544FE2CA129, VRepeat = 0xE7C7FA3697B2, UShift = 0x7B8B5E0E16EB,
                        VShift = 0x38DA962E408E, LayerPath = 0x6000, LayerMaskPath = 0x7000;
// Techniques by how they lay their layer and read its mask, in red on the base's coordinates unless named so: multiplying
// (fox3ddf_cloth_dirty), blending on the encoded values (fox3ddf_blin_layer_sbl_mu, the mask on the third coordinates), in linear
// light (fox3ddf_blin_layer_bl), with the mask in blue (fox3ddf_blin_ly_mul_2subnorm), in blue on the third coordinates
// (fox3ddf_blin_ly_sbl_2snm_nu), on the fourth (fox3ddf_blin_layer_bl_mu_nu), on the second, unmapped
// (fox3ddf_blin_layersb_snm_nlmu), and one not known to read its layer (in no record).
constexpr std::uint64_t Multiplying = 0xE2C9B71532D1, BlendingOnThirdUvs = 0x3FBC9A12FB36, BlendingLinearly = 0x54214F48645B, MaskInBlue = 0xAA02EC2A2C1D,
                        MaskInBlueOnThirdUvs = 0xCDDF56CB51E3, MaskOnFourthUvs = 0xECC9C23CA8E4, MaskOnSecondUvs = 0xD8DE23B18660, UnknownToReadItsLayer = 0xC0CE81F00048;

// A mesh of a test model file: its alpha and shadow bytes, whether its vertices have texture coordinates,
// its material's one texture parameter (BaseTexture, MaskTexture, LinearBaseTexture, or 0 for none), and with an intensity its
// vectors: that SelfColorIntensity, a SelfColor of (1, 0.5, 0.25) and a MatParamIndex_0 of `materialRow`; a GlassColor
// vector, or the mirror's technique. With a `layerRepeat`, a layer and its mask too, repeating so often across and twice that
// up, shifted by (0.25, 0.5), of its `layerTechnique`, on second coordinates of its vertices with `hasSecondUvs`. `hasThirdUvs` and
// `hasFourthUvs` give it those coordinates too.
struct TestMesh
{
    std::uint8_t alpha{}, shadow{};
    bool hasUvs{};
    std::uint64_t textureParameter{};
    float selfIntensity{}, materialRow{};
    bool isGlass{}, isMirror{};
    float layerRepeat{};
    std::uint64_t layerTechnique{};
    bool hasSecondUvs = true;
    bool hasThirdUvs{}, hasFourthUvs{};
};

// A test file's vertices' coordinates: the first, (1, 1) as halves, and the bytes a rigid mesh's further ones share with the
// skin it has no use for. Its fourth share the first's.
constexpr std::uint32_t FirstUvs = 0x3C003C00, SecondUvs = 0xFF, ThirdUvs = 0;

template<class T> void put(std::vector<std::uint8_t>& bytes, T value)
{
    const auto at = bytes.size();
    bytes.resize(at + sizeof(T));
    std::memcpy(bytes.data() + at, &value, sizeof(T));
}

template<class T> void putAt(std::vector<std::uint8_t>& bytes, std::size_t at, T value)
{
    std::memcpy(bytes.data() + at, &value, sizeof(T));
}

// A model file as the game keeps one (FMDL 2.0x; docs/DEVELOPMENT.md, "Scene objects") with no more in it
// than describeModel reads: each mesh one triangle with its own material instance and texture. A skinned
// file has two bones and one bone group, through which each vertex's first bone is the model's bone 1.
std::vector<std::uint8_t> modelFile(const std::vector<TestMesh>& meshes, bool isSkinned = false)
{
    struct Block
    {
        std::uint16_t count{};
        std::vector<std::uint8_t> entries;
    };
    std::map<std::uint16_t, Block> blocks;
    const auto block = [&](std::uint16_t type, std::size_t count, std::size_t entrySize) -> std::vector<std::uint8_t>& {
        blocks[type] = {static_cast<std::uint16_t>(count), std::vector<std::uint8_t>(count * entrySize)};
        return blocks[type].entries;
    };

    // The buffer data: every mesh's indices, then its positions, then its second stream (texture
    // coordinates, bone numbers and weights), 12 bytes a vertex.
    const auto vertices = static_cast<std::uint16_t>(3 * meshes.size());
    std::vector<std::uint8_t> data;
    for (std::uint16_t v = 0; v < vertices; ++v) put(data, static_cast<std::uint16_t>(v % 3));

    const auto positionsAt = static_cast<std::uint32_t>(data.size());
    for (std::uint16_t v = 0; v < vertices; ++v)
        for (const float value : {v % 3 == 1 ? 1.0f : 0.0f, v % 3 == 2 ? 1.0f : 0.0f, static_cast<float>(v / 3)}) put(data, value);

    const auto streamAt = static_cast<std::uint32_t>(data.size());
    for (std::uint16_t v = 0; v < vertices; ++v)
    {
        put<std::uint32_t>(data, FirstUvs);
        put<std::uint32_t>(data, ThirdUvs);  // Each bone number the bone group's first
        put<std::uint32_t>(data, SecondUvs); // All the weight on the first
    }

    auto& meshBlock = block(3, meshes.size(), 48);
    auto& materialInstances = block(4, meshes.size(), 16);
    auto& textures = block(6, 3 * meshes.size(), 4);
    auto& parameters = block(7, 12 * meshes.size(), 4);
    auto& materials = block(8, 2 + meshes.size(), 4);
    auto& assignments = block(9, meshes.size(), 8);
    auto& meshFormats = block(10, 2 * meshes.size(), 8);
    auto& faceInfos = block(17, meshes.size(), 8);
    auto& paths = block(21, 3 * meshes.size(), 8);
    auto& names = block(22, 2 * meshes.size() + 11, 8);
    std::vector<std::uint8_t> vertexFormats;
    const auto format = [&](std::uint8_t usage, std::uint8_t dataType, std::uint16_t element) {
        put(vertexFormats, usage);
        put(vertexFormats, dataType);
        put(vertexFormats, element);
    };
    for (std::uint16_t i = 0; i < meshes.size(); ++i)
    {
        const auto& mesh = meshes[i];
        meshBlock[48 * i] = mesh.alpha;
        meshBlock[48 * i + 1] = mesh.shadow;
        putAt<std::uint16_t>(meshBlock, 48 * i + 4, i);  // Material instance
        putAt<std::uint16_t>(meshBlock, 48 * i + 10, 3); // Vertices
        putAt<std::uint32_t>(meshBlock, 48 * i + 16, 3u * i);
        putAt<std::uint64_t>(meshBlock, 48 * i + 24, i); // Face info
        // The material after the first has the mirror's technique, and each mesh has its own after that for its layer's.
        putAt<std::uint16_t>(materialInstances, 16 * i + 4, static_cast<std::uint16_t>(mesh.isMirror ? 1 : mesh.layerTechnique ? 2 + i : 0));
        materialInstances[16 * i + 6] = mesh.textureParameter != 0;
        putAt<std::uint16_t>(materialInstances, 16 * i + 8, i);
        // Its four vectors' parameters, after every mesh's texture parameter.
        const auto firstVector = static_cast<std::uint16_t>(meshes.size() + 4 * i);
        materialInstances[16 * i + 7] = mesh.isGlass ? 4 : mesh.selfIntensity > 0 ? 3 : 0;
        putAt<std::uint16_t>(materialInstances, 16 * i + 10, firstVector);
        for (std::uint16_t k = 0; k < 4; ++k)
        {
            putAt<std::uint16_t>(parameters, 4 * (firstVector + k), static_cast<std::uint16_t>(meshes.size() + k));
            putAt<std::uint16_t>(parameters, 4 * (firstVector + k) + 2, static_cast<std::uint16_t>(4 * i + k));
        }

        // A layered mesh's own parameters, after the others': its three textures, then its layer's four vectors.
        const auto firstOwn = static_cast<std::uint16_t>(5 * meshes.size() + 7 * i);
        const auto layerName = static_cast<std::uint16_t>(meshes.size() + 5);
        if (mesh.layerRepeat > 0)
        {
            materialInstances[16 * i + 6] = 3;
            putAt<std::uint16_t>(materialInstances, 16 * i + 8, firstOwn);

            // Glass keeps its GlassColor, which makes it glass.
            if (!mesh.isGlass)
            {
                materialInstances[16 * i + 7] = 4;
                putAt<std::uint16_t>(materialInstances, 16 * i + 10, static_cast<std::uint16_t>(firstOwn + 3));
            }

            putAt<std::uint16_t>(parameters, 4 * firstOwn, i);
            putAt<std::uint16_t>(parameters, 4 * firstOwn + 2, i);

            for (std::uint16_t k = 0; k < 2; ++k)
            {
                const auto texture = static_cast<std::uint16_t>(meshes.size() + 2 * i + k);
                putAt<std::uint16_t>(parameters, 4 * (firstOwn + 1 + k), static_cast<std::uint16_t>(layerName + k));
                putAt<std::uint16_t>(parameters, 4 * (firstOwn + 1 + k) + 2, texture);
                putAt<std::uint16_t>(textures, 4 * texture + 2, texture);
                putAt<std::uint64_t>(paths, 8 * texture, (k ? LayerMaskPath : LayerPath) + i);
            }

            for (std::uint16_t k = 0; k < 4; ++k)
            {
                putAt<std::uint16_t>(parameters, 4 * (firstOwn + 3 + k), static_cast<std::uint16_t>(layerName + 2 + k));
                putAt<std::uint16_t>(parameters, 4 * (firstOwn + 3 + k) + 2, static_cast<std::uint16_t>(4 * meshes.size() + 4 * i + k));
            }
        }

        putAt<std::uint16_t>(textures, 4 * i + 2, i);
        putAt<std::uint16_t>(parameters, 4 * i, i);
        putAt<std::uint16_t>(parameters, 4 * i + 2, i);
        putAt<std::uint32_t>(faceInfos, 8 * i + 4, 3);
        putAt<std::uint64_t>(paths, 8 * i, TexturePath + i);
        putAt<std::uint64_t>(names, 8 * i, mesh.textureParameter);
        putAt<std::uint64_t>(names, 8 * (meshes.size() + 11 + i), mesh.layerTechnique);
        putAt<std::uint16_t>(materials, 4 * (2 + i) + 2, static_cast<std::uint16_t>(meshes.size() + 11 + i));

        // Its streams: the positions, then the rest.
        const auto firstFormat = static_cast<std::uint16_t>(vertexFormats.size() / 4);
        format(0, 1, 0);                  // Positions, three floats
        if (mesh.hasUvs) format(8, 7, 0); // Texture coordinates, two halves
        if (mesh.layerRepeat > 0 && mesh.hasSecondUvs) format(9, 7, 8);
        if (mesh.hasThirdUvs) format(10, 7, 4);
        if (mesh.hasFourthUvs) format(11, 7, 0);
        if (isSkinned) format(7, 9, 4);   // Bone numbers, four bytes
        if (isSkinned) format(1, 8, 8);   // Weights, four unorms

        assignments[8 * i] = 2;
        putAt<std::uint16_t>(assignments, 8 * i + 4, static_cast<std::uint16_t>(2 * i));
        putAt<std::uint16_t>(assignments, 8 * i + 6, firstFormat);
        meshFormats[16 * i + 1] = 1;
        meshFormats[16 * i + 2] = 12;
        putAt<std::uint32_t>(meshFormats, 16 * i + 4, positionsAt + 36u * i);
        meshFormats[16 * i + 9] = static_cast<std::uint8_t>(vertexFormats.size() / 4 - firstFormat - 1);
        meshFormats[16 * i + 10] = 12;
        putAt<std::uint32_t>(meshFormats, 16 * i + 12, streamAt + 36u * i);
    }

    putAt<std::uint64_t>(names, 8 * meshes.size(), SelfColor);
    putAt<std::uint64_t>(names, 8 * (meshes.size() + 1), SelfColorIntensity);
    putAt<std::uint64_t>(names, 8 * (meshes.size() + 2), MaterialIndex);
    putAt<std::uint64_t>(names, 8 * (meshes.size() + 3), GlassColor);
    putAt<std::uint64_t>(names, 8 * (meshes.size() + 4), ParallaxReflection);
    putAt<std::uint64_t>(names, 8 * (meshes.size() + 5), LayerTexture);
    putAt<std::uint64_t>(names, 8 * (meshes.size() + 6), LayerMaskTexture);
    putAt<std::uint64_t>(names, 8 * (meshes.size() + 7), URepeat);
    putAt<std::uint64_t>(names, 8 * (meshes.size() + 8), VRepeat);
    putAt<std::uint64_t>(names, 8 * (meshes.size() + 9), UShift);
    putAt<std::uint64_t>(names, 8 * (meshes.size() + 10), VShift);
    putAt<std::uint16_t>(materials, 4 * 1 + 2, static_cast<std::uint16_t>(meshes.size() + 4));
    blocks[11] = {static_cast<std::uint16_t>(vertexFormats.size() / 4), vertexFormats};
    block(14, 1, 16); // One buffer, from the buffer data's start

    if (isSkinned)
    {
        block(0, 2, 48);
        auto& boneGroups = block(5, 1, 68);
        putAt<std::uint16_t>(boneGroups, 2, 1);
        putAt<std::uint16_t>(boneGroups, 4, 1);
    }

    // The header, the section info (each block's type, count and offset into section 0, then section 1's
    // buffer data), section 0's blocks and section 1.
    const auto section0 = 64 + 8 * blocks.size() + 24;
    std::vector<std::uint8_t> file(section0);
    putAt<std::uint32_t>(file, 0, 0x4C444D46);
    putAt<std::uint64_t>(file, 8, 64);
    putAt<std::uint32_t>(file, 32, static_cast<std::uint32_t>(blocks.size()));
    putAt<std::uint32_t>(file, 36, 2);
    putAt<std::uint32_t>(file, 40, static_cast<std::uint32_t>(section0));

    std::size_t info = 64;
    for (const auto& [type, entries] : blocks)
    {
        putAt<std::uint16_t>(file, info, type);
        putAt<std::uint16_t>(file, info + 2, entries.count);
        putAt<std::uint32_t>(file, info + 4, static_cast<std::uint32_t>(file.size() - section0));
        file.insert(file.end(), entries.entries.begin(), entries.entries.end());
        info += 8;
    }

    // Section 1's other part: the materials' vectors, after the buffer data.
    putAt<std::uint32_t>(file, info, 2);
    putAt<std::uint32_t>(file, info + 12, 0);
    putAt<std::uint32_t>(file, info + 16, static_cast<std::uint32_t>(data.size()));
    for (const auto& mesh : meshes)
        for (const float value : {1.0f, 0.5f, 0.25f, 1.0f, mesh.selfIntensity, 0.0f, 0.0f, 0.0f, mesh.materialRow, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f})
            put(data, value);

    for (const auto& mesh : meshes)
        for (const float value : {mesh.layerRepeat, 0.0f, 0.0f, 0.0f, 2 * mesh.layerRepeat, 0.0f, 0.0f, 0.0f, 0.25f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f}) put(data, value);

    putAt<std::uint32_t>(file, 48, static_cast<std::uint32_t>(file.size()));
    file.insert(file.end(), data.begin(), data.end());

    return file;
}

std::shared_ptr<const Model> modelOf(const std::vector<TestMesh>& meshes, Source source, bool isSkinned = false)
{
    const auto model = describeModel(1, modelFile(meshes, isSkinned), source);
    expect(model != nullptr, "The test's model file reads.");

    return model;
}

const ModelPart& partOf(const Model& model, std::uint32_t mesh)
{
    const auto part = std::ranges::find(model.parts, mesh, &ModelPart::mesh);
    expect(part != model.parts.end(), "The mesh is taken.");

    return *part;
}

using Roles = std::vector<std::optional<MeshRole>>;
constexpr auto Dropped = std::nullopt;

// Each mesh's role, or Dropped.
Roles rolesOf(const std::vector<TestMesh>& meshes, Source source, bool isSkinned = false)
{
    const auto model = modelOf(meshes, source, isSkinned);
    Roles roles;
    for (std::uint32_t mesh = 0; mesh < meshes.size(); ++mesh)
    {
        const auto part = std::ranges::find(model->parts, mesh, &ModelPart::mesh);
        const bool isDropped = std::ranges::find(model->dropped, mesh, &DroppedMesh::mesh) != model->dropped.end();
        expect((part != model->parts.end()) != isDropped, "Each mesh is taken or dropped.");
        roles.push_back(isDropped ? std::nullopt : std::optional(part->role));
    }

    return roles;
}

Roles rigid(const std::vector<TestMesh>& meshes)
{
    return rolesOf(meshes, Source::VisibilityManager);
}

Roles character(const std::vector<TestMesh>& meshes)
{
    return rolesOf(meshes, Source::ModelList, true);
}

// Mother Base's scaffold tarps (2026-10-27): the sheets the main view alone shows are cut out and dropped,
// so their stand-in, drawn into every sun cascade, is all the rays have of them. Met by every ray, a sheet's
// rays would meet its inside 16 cm away.
void tarpSheetsCastThroughStandIn()
{
    expect(rigid({{32, 1}, {0, 2}, {0, 0}}) == Roles{Dropped, MeshRole::ShadowOnly, MeshRole::Solid},
        "A tarp's stand-in is kept for rays towards lights alone, as its sheets are dropped.");
}

// Serak Power Plant (2026-10-27): a room's walls and a cave's rock overhang the main view alone shows let
// the sun in, where the game cast it with their stand-ins, drawn into every sun cascade. The prologue
// hospital's building (2026-10-26): its stand-in kept 80 to 100% of a room's lamp off a wall.
void solidStandInsCastTheSunAlone()
{
    expect(rigid({{0, 0}, {0, 1}, {0, 2}}) == Roles{MeshRole::Solid, MeshRole::MainViewOnly, MeshRole::SunShadowOnly},
        "A building's stand-in casts the sun for the walls the main view alone shows.");
    expect(rigid({{0, 1}, {0, 2}}) == Roles{MeshRole::MainViewOnly, MeshRole::SunShadowOnly}, "A rock's stand-in casts the sun.");
    expect(rigid({{0, 2}, {16, 5}}) == Roles{MeshRole::SunShadowOnly, Dropped}, "Beside glass, a stand-in casts the sun alone, not a lamp.");
}

void pylonLattice()
{
    expect(rigid({{32, 1}, {128, 66}}) == Roles{Dropped, MeshRole::CutOut}, "A pylon's lattice casts through its cut-out stand-in.");
}

// Alpha bit 4 is no glass: it marks a mesh drawn forward, after the G-buffer, most of them fox3DFW_ConstantSRGB (the game's
// files, 2026-10-31). Those the main view alone shows are dropped; those cast are solid.
void forwardMeshesWithoutGlassMaterial()
{
    expect(rigid({{16, 5}, {16, 4}}) == Roles{Dropped, MeshRole::Solid}, "A forward mesh without a glass material is no glass.");
}

// Windows, visors and lenses, whatever their bytes (the game's files, 2026-10-31): glass is the material with a GlassColor.
void glassIsItsMaterial()
{
    const auto pane = [](std::uint8_t alpha, std::uint8_t shadow) { return TestMesh{.alpha = alpha, .shadow = shadow, .isGlass = true}; };
    expect(rigid({pane(16, 5), pane(80, 133), pane(16, 4), pane(80, 132), pane(49, 5), pane(17, 5)}) == Roles(6, MeshRole::Glass),
        "A rigid model's glass is glass, seen or cast.");
    expect(character({pane(16, 5), pane(80, 133)}) == Roles(2, MeshRole::Glass), "A character's glasses and visors are glass.");
    expect(rigid({{.alpha = 16, .shadow = 5, .hasUvs = true, .textureParameter = BaseTexture, .selfIntensity = 80, .isGlass = true}}) == Roles{MeshRole::Emissive},
        "A self-lit mesh is taken for its light first.");
}

// The hospital's mirrors (2026-10-31): fox3DFW_ParallaxReflection, a painted picture of the room, not a reflection.
void mirrorsAreGlass()
{
    expect(rigid({{.alpha = 16, .shadow = 36, .isMirror = true}, {.alpha = 80, .shadow = 133, .isMirror = true}}) == Roles(2, MeshRole::Mirror),
        "A mirror is taken for what it is, whatever its bytes.");
}

void glassKeepsItsMask()
{
    const auto model = modelOf({{.alpha = 16, .shadow = 5, .hasUvs = true, .textureParameter = LinearBaseTexture, .isGlass = true},
                                   {.alpha = 16, .shadow = 36, .hasUvs = true, .textureParameter = MaskTexture, .isMirror = true}},
        Source::VisibilityManager);
    expect(partOf(*model, 0).baseTexture == TexturePath, "Glass's mask is its Base_Tex_LIN.");
    expect(partOf(*model, 1).baseTexture == 0, "A mirror has none: neither its painted picture nor a mask on coordinates the scene keeps.");

    const auto material = materialOf(partOf(*model, 0), {.base = 6});
    expect(material.flags == (GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_GLASS | GI_GEOMETRY_UV) && material.texture == 7 && material.alphaThreshold == 0.0f,
        "Glass is non-opaque for the rays to choose, and tests no alpha.");
    expect(materialOf(partOf(*model, 1), {}).flags == (GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_GLASS | GI_GEOMETRY_MIRROR | GI_GEOMETRY_UV), "A mirror is glass that reflects all.");
}

// Sky domes kilometres wide around the camera blocked the sky for the global illumination.
void skyDomeDropped()
{
    expect(rigid({{80, 5}, {64, 0}}) == Roles{Dropped, Dropped}, "A rigid model's see-through meshes are dropped.");
}

// A room's inner walls (2026-10-25): without them shadow rays left the walls from the building's shell
// 7 cm behind them.
void innerWallsMainViewOnly()
{
    expect(rigid({{0, 1}}) == Roles{MeshRole::MainViewOnly}, "A solid mesh the main view alone shows is taken.");
}

// Crossed cards 44 cm above a road cast a moon shadow where the game showed nothing (2026-10-25).
void crossedCardsDropped()
{
    expect(rigid({{32, 1}, {160, 65}}) == Roles{Dropped, Dropped}, "Cut-out meshes and decals the main view alone shows are dropped.");
}

// A trench's grating and a tree's twigs cast in the game (2026-10-23).
void decalsCast()
{
    expect(rigid({{160, 64}, {128, 64}}) == Roles{MeshRole::CutOut, MeshRole::CutOut}, "Decals are taken as cut out.");
}

// Taken as both main view only and shadow only, no ray would meet it.
void shadowByteThreeDropped()
{
    expect(rigid({{32, 1}, {0, 3}, {0, 2}}) == Roles{Dropped, Dropped, MeshRole::ShadowOnly},
        "A stand-in the main view shows too is dropped beside main-view cut-outs.");
    expect(rigid({{0, 3}}) == Roles{Dropped}, "A stand-in the main view shows too is dropped.");
}

// A woman's hair (2026-10-24): without it the rays saw her bald head's outline across it.
void hairTaken()
{
    expect(character({{32, 1}, {64, 0}, {32, 0}}) == Roles{MeshRole::Hair, MeshRole::Hair, MeshRole::CutOut},
        "A character's cut-out meshes the game never casts with, and its see-through ones, are hair.");
}

// A 40-triangle stand-in of a woman's outfit lay on her skin and blocked her chest's sun rays (2026-10-24).
void characterStandInDropped()
{
    expect(character({{0, 2}, {32, 2}}) == Roles{Dropped, Dropped}, "A character's stand-ins are dropped.");
}

void characterMainViewSolid()
{
    expect(character({{0, 1}}) == Roles{MeshRole::Solid}, "A character's solid meshes cast, even those the game never casts with.");
}

void bushesAreLeaves()
{
    expect(rolesOf({{160, 65}, {160, 64}, {0, 0}}, Source::ObjectBrush) == Roles{MeshRole::Leaf, MeshRole::Leaf, MeshRole::Solid},
        "The object brush's bushes and grass are leaves, though the game never casts them; its props are solid.");
}

void sameFileDependsOnSource()
{
    expect(rolesOf({{160, 65}, {160, 64}}, Source::VisibilityManager) == Roles{Dropped, MeshRole::CutOut},
        "A bush's file from a visibility manager takes a rigid model's roles.");
}

// What each role tells the rays (StopsShadowRay, IsShadowOnly, PassHair, HitDirectLight).
void materialPerRole()
{
    const auto flagsOf = [](const Model& model, std::uint32_t mesh) { return materialOf(partOf(model, mesh), {}).flags; };
    const auto building = modelOf({{0, 0}, {128, 64}, {0, 1}, {32, 1}, {0, 2}}, Source::VisibilityManager);
    expect(flagsOf(*building, 0) == 0, "A solid mesh stops every ray.");
    expect(flagsOf(*building, 1) == GI_GEOMETRY_CUT_OUT, "A cut-out mesh is tested by its alpha.");
    expect(flagsOf(*building, 2) == GI_GEOMETRY_NO_SHADOW, "Rays towards lights pass a main-view-only mesh.");
    expect(flagsOf(*building, 4) == GI_GEOMETRY_SHADOW_ONLY, "Only rays towards lights meet a tarp's stand-in.");

    const auto rock = modelOf({{0, 1}, {0, 2}}, Source::VisibilityManager);
    expect(flagsOf(*rock, 1) == (GI_GEOMETRY_SHADOW_ONLY | GI_GEOMETRY_SUN_ONLY), "Only the sun's rays meet a rock's stand-in.");

    const auto bush = modelOf({{160, 64}}, Source::ObjectBrush);
    expect(flagsOf(*bush, 0) == (GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_LEAF), "Leaves are cut out and let light through.");

    const auto person = modelOf({{64, 0}}, Source::ModelList, true);
    expect(flagsOf(*person, 0) == (GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_HAIR), "Hair is cut out and lets light through in part.");
}

void cutOutWaitsForTexture()
{
    const auto model = modelOf({{32, 0, true, BaseTexture}, {128, 66, true, MaskTexture}, {32, 0, false, BaseTexture}, {0, 0, true, BaseTexture}},
        Source::VisibilityManager);
    const auto& cutOut = partOf(*model, 0);
    expect(cutOut.baseTexture == TexturePath && !cutOut.textureIsMask, "A mesh's base texture is its Base_Tex_SRGB's path hash.");

    const auto waiting = materialOf(cutOut, {});
    expect(waiting.flags == (GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_UV) && waiting.texture == 0 && waiting.alphaThreshold == 0.0f,
        "A cut-out mesh tests no alpha before its texture is copied.");

    const auto textured = materialOf(cutOut, {.base = 6});
    expect(textured.flags == (GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_UV | GI_GEOMETRY_ALPHA_TEXTURE) && textured.texture == 7 && textured.alphaThreshold == 0.5f,
        "A cut-out mesh with its texture tests its alpha against a half, as the game's shadow pass does.");

    const auto& standIn = partOf(*model, 1);
    expect(standIn.baseTexture == TexturePath + 1 && standIn.textureIsMask && (materialOf(standIn, {.base = 6}).flags & GI_GEOMETRY_MASK),
        "A mesh without a base texture cuts by its mask, whose colour is not its own.");

    const auto untextured = materialOf(partOf(*model, 2), {.base = 6});
    expect(untextured.flags == GI_GEOMETRY_CUT_OUT && untextured.texture == 0, "A mesh without texture coordinates has no texture.");

    const auto solid = materialOf(partOf(*model, 3), {.base = 6});
    expect(solid.flags == GI_GEOMETRY_UV && solid.texture == 7 && solid.alphaThreshold == 0.0f, "A solid mesh takes its texture's colour, not its alpha.");
}

// Fox Engine's self-lit meshes carry the forward bit and the main view's alone (alpha 0x10, shadow 0x25, a lamp's bulb):
// taken for their light where other such meshes are dropped. A glow card, additive (alpha 0x11), is no surface; the sky's
// dome and the moon have an intensity of 1, and a mesh painted black less: they stay what they were.
void selfLitMeshes()
{
    const auto model = modelOf({{0x10, 0x25, true, BaseTexture, 80, 18}}, Source::VisibilityManager);
    const auto& bulb = partOf(*model, 0);
    expect(bulb.role == MeshRole::Emissive, "A bulb is taken for its light.");
    expect(bulb.emission == std::array{80.0f, 40.0f, 20.0f}, "Its light is its self colour times its intensity.");

    const auto material = materialOf(bulb, {.base = 6});
    expect((material.flags & (GI_GEOMETRY_EMISSIVE | GI_GEOMETRY_NO_SHADOW)) == (GI_GEOMETRY_EMISSIVE | GI_GEOMETRY_NO_SHADOW),
        "It gives light, and rays towards lights pass it.");
    expect(material.emission == bulb.emission && material.materialIndex == 18 && material.texture == 7, "Its material carries its light, row and colour.");

    expect(rigid({{0x11, 0x25, true, BaseTexture, 4000}, {0x10, 0x25, true, BaseTexture, 1}}) == Roles{Dropped, Dropped},
        "Glow cards and what barely glows stay dropped.");
    expect(rigid({{0, 0, true, BaseTexture, 1, 7}}) == Roles{MeshRole::Solid} && partOf(*modelOf({{0, 0, true, BaseTexture, 1, 7}}, Source::VisibilityManager), 0).materialIndex == 7,
        "An ordinary mesh keeps its role, with its row of the material table.");
}

// A mesh's layer (the white base of Kaz's beige coat, whose colour is all in its layer): what a hit needs of it, once its
// textures are copied. Without second coordinates there is no knowing where it lies, glass reflects by its mask alone, and a
// technique not known to read its layer keeps its base: a layer it may not read must not paint the mesh.
void layers()
{
    const auto layered = [](std::uint64_t technique) {
        return TestMesh{.hasUvs = true, .textureParameter = BaseTexture, .layerRepeat = 5, .layerTechnique = technique, .hasThirdUvs = true};
    };

    auto withoutSecondUvs = layered(Multiplying), withoutThirdUvs = layered(BlendingOnThirdUvs), glass = layered(Multiplying), unnamed = layered(Multiplying);
    withoutSecondUvs.hasSecondUvs = false;
    withoutThirdUvs.hasThirdUvs = false;
    glass.alpha = 16;
    glass.shadow = 5;
    glass.textureParameter = LinearBaseTexture;
    glass.isGlass = true;
    unnamed.layerRepeat = 0;
    auto onFourthUvs = layered(MaskOnFourthUvs);
    onFourthUvs.hasFourthUvs = true;

    const auto model = modelOf({layered(Multiplying), layered(BlendingOnThirdUvs), withoutSecondUvs, glass, unnamed, layered(UnknownToReadItsLayer), layered(BlendingLinearly),
                                   withoutThirdUvs, layered(MaskInBlue), layered(MaskInBlueOnThirdUvs), onFourthUvs, layered(MaskOnSecondUvs)},
        Source::VisibilityManager);

    const auto layerOf = [&](std::uint32_t mesh) -> const Layer& { return *partOf(*model, mesh).layer; };
    const auto& multiplied = partOf(*model, 0);
    expect(layerOf(0).texture == LayerPath && layerOf(0).maskTexture == LayerMaskPath, "A layered mesh names its layer and the layer's mask.");
    expect(layerOf(0).mapping == std::array{5.0f, 10.0f, 0.25f, 0.5f}, "Its layer's repeat and shift are its material's.");
    expect(layerOf(0).uvs == std::vector<std::uint32_t>(3, SecondUvs), "It keeps its vertices' second coordinates.");
    expect(layerOf(0).blend == LayerBlend::Multiply && layerOf(1).blend == LayerBlend::BlendEncoded && layerOf(6).blend == LayerBlend::BlendLinear,
        "Its technique says how the layer lies.");

    expect(!partOf(*model, 2).layer, "Without second coordinates a mesh has no layer.");
    expect(!partOf(*model, 3).layer, "Glass has none.");
    expect(!partOf(*model, 4).layer, "Nor has a mesh without one.");
    expect(!partOf(*model, 5).layer, "A technique not known to read its layer keeps its base texture.");

    // Most of the shaders read the mask on the base's coordinates, in red.
    expect(layerOf(0).maskUvs.empty() && !layerOf(0).isMaskInBlue, "A mask on the base's coordinates needs none of its own.");
    expect(layerOf(1).maskUvs == std::vector<std::uint32_t>(3, ThirdUvs) && !layerOf(1).isMaskInBlue, "A technique's mask can lie on the third coordinates.");
    expect(!partOf(*model, 7).layer, "Without them there is no knowing where the layer lies.");
    expect(layerOf(8).maskUvs.empty() && layerOf(8).isMaskInBlue, "It can be in blue.");
    expect(layerOf(9).maskUvs == std::vector<std::uint32_t>(3, ThirdUvs) && layerOf(9).isMaskInBlue, "Or both.");
    expect(layerOf(10).maskUvs == std::vector<std::uint32_t>(3, FirstUvs), "It can lie on the fourth coordinates.");
    expect(layerOf(11).maskUvs == std::vector<std::uint32_t>(3, SecondUvs), "Or on the second, unmapped.");

    constexpr MaterialTextures Copied{.base = 6, .layer = 7, .layerMask = 8};
    const auto material = materialOf(partOf(*model, 1), Copied);
    expect(material.layerTexture == 8 && material.layerMaskTexture == 9, "Its copies are its material's.");
    expect((material.flags & GI_GEOMETRY_LAYER_BLEND) && !(material.flags & GI_GEOMETRY_LAYER_LINEAR), "A blend on the encoded values is one flag.");
    const auto linearFlags = materialOf(partOf(*model, 6), Copied).flags;
    expect((linearFlags & GI_GEOMETRY_LAYER_BLEND) && (linearFlags & GI_GEOMETRY_LAYER_LINEAR), "One in linear light is both.");
    expect(!(materialOf(multiplied, Copied).flags & (GI_GEOMETRY_LAYER_BLEND | GI_GEOMETRY_LAYER_LINEAR)), "A layer that multiplies needs neither.");
    expect((materialOf(partOf(*model, 8), Copied).flags & GI_GEOMETRY_LAYER_MASK_BLUE) && !(material.flags & GI_GEOMETRY_LAYER_MASK_BLUE), "A mask in blue has its flag.");

    const auto beforeLayer = materialOf(partOf(*model, 1), {.base = 6, .layerMask = 8});
    expect(beforeLayer.layerTexture == 0 && !(beforeLayer.flags & GI_GEOMETRY_LAYER_BLEND), "Before its copy comes a mesh is its base texture.");
    const auto beforeMask = materialOf(partOf(*model, 1), {.base = 6, .layer = 7});
    expect(beforeMask.layerTexture == 0 && !(beforeMask.flags & GI_GEOMETRY_LAYER_BLEND), "So it is before its mask's: the layer would lie on all of it.");

    auto maskless = multiplied;
    maskless.layer->maskTexture = 0;
    expect(materialOf(maskless, {.base = 6, .layer = 7}).layerTexture == 8, "A layer naming no mask waits for none.");
}

void unreadableFiles()
{
    const auto file = modelFile({{0, 0}});
    expect(fileKindOf(file) == ModelFile::Rigid && fileKindOf(modelFile({{0, 0}}, true)) == ModelFile::Skinned, "A model file is rigid or skinned.");

    const std::vector<std::uint8_t> notFmdl(file.size(), 0x58);
    expect(fileKindOf({}) == ModelFile::Unreadable && fileKindOf(notFmdl) == ModelFile::Unreadable, "Bytes that are no FMDL are unreadable.");
    expect(!describeModel(1, {}, Source::VisibilityManager) && !describeModel(1, notFmdl, Source::VisibilityManager), "Bytes that are no FMDL describe no model.");

    const std::vector<std::uint8_t> cut(file.begin(), file.begin() + file.size() / 2);
    expect(!describeModel(1, cut, Source::VisibilityManager), "A file cut short describes no model.");
}

void skinThroughBoneGroup()
{
    const auto model = modelOf({{0, 0}}, Source::ModelList, true);
    const auto& part = partOf(*model, 0);
    expect(model->bones == 2 && part.boneGroup == std::vector<std::uint16_t>{1}, "A skinned mesh keeps its bone group.");
    expect(std::ranges::all_of(part.bones, [](std::uint64_t bones) { return bones == 0x0001000100010001; }) && part.weights == std::vector<std::uint32_t>(3, 0xFF),
        "A vertex's bone numbers index its mesh's bone group, the model's bones.");

    const auto rigidModel = modelOf({{0, 0}}, Source::VisibilityManager);
    expect(rigidModel->bones == 0 && rigidModel->parts[0].bones.empty(), "A rigid model has no skin.");
}
}

// A debug case's revealed scene: a mesh the rules drop is kept as solid and marked, and still listed as dropped.
void revealedSceneKeepsDroppedMeshes()
{
    const auto file = modelFile({{16, 5}, {0, 0}}, false);
    const auto revealed = describeModel(1, file, Source::VisibilityManager, true);
    expect(revealed != nullptr && revealed->parts.size() == 2, "A revealed model keeps the mesh the rules drop.");
    expect(revealed->parts[0].isDropped && revealed->parts[0].role == MeshRole::Solid, "The kept mesh is solid and marked.");
    expect(!revealed->parts[1].isDropped, "A mesh the rules take is not marked.");
    expect(revealed->dropped.size() == 1, "The kept mesh is still listed as dropped.");
    expect(describeModel(1, file, Source::VisibilityManager)->parts.size() == 1, "Without it the scene takes what it always took.");
}

int main()
{
    try
    {
        revealedSceneKeepsDroppedMeshes();
        tarpSheetsCastThroughStandIn();
        solidStandInsCastTheSunAlone();
        pylonLattice();
        forwardMeshesWithoutGlassMaterial();
        glassIsItsMaterial();
        mirrorsAreGlass();
        glassKeepsItsMask();
        skyDomeDropped();
        innerWallsMainViewOnly();
        crossedCardsDropped();
        decalsCast();
        shadowByteThreeDropped();
        hairTaken();
        characterStandInDropped();
        characterMainViewSolid();
        bushesAreLeaves();
        sameFileDependsOnSource();
        materialPerRole();
        cutOutWaitsForTexture();
        selfLitMeshes();
        layers();
        unreadableFiles();
        skinThroughBoneGroup();
        std::cout << "Scene description tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
