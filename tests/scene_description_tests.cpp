#include "passes/ray_tracing/scene_description.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"

#include <algorithm>
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

// The StrCode64s of the texture parameters a material names its textures by (Base_Tex_SRGB, Mask_Tex_LIN),
// and the path hash of mesh i's texture, TexturePath + i.
constexpr std::uint64_t BaseTexture = 0x41EA7BE81B61, MaskTexture = 0x3ED9B75CD480, TexturePath = 0x5000;

// A mesh of a test model file: its alpha and shadow bytes, whether its vertices have texture coordinates,
// and its material's one texture parameter (BaseTexture, MaskTexture, or 0 for none).
struct TestMesh
{
    std::uint8_t alpha{}, shadow{};
    bool hasUvs{};
    std::uint64_t textureParameter{};
};

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
        put<std::uint32_t>(data, 0x3C003C00); // (1, 1) as halves
        put<std::uint32_t>(data, 0);          // Each bone number the bone group's first
        put<std::uint32_t>(data, 0xFF);       // All the weight on the first
    }

    auto& meshBlock = block(3, meshes.size(), 48);
    auto& materialInstances = block(4, meshes.size(), 16);
    auto& textures = block(6, meshes.size(), 4);
    auto& parameters = block(7, meshes.size(), 4);
    auto& assignments = block(9, meshes.size(), 8);
    auto& meshFormats = block(10, 2 * meshes.size(), 8);
    auto& faceInfos = block(17, meshes.size(), 8);
    auto& paths = block(21, meshes.size(), 8);
    auto& names = block(22, meshes.size(), 8);
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
        materialInstances[16 * i + 6] = mesh.textureParameter != 0;
        putAt<std::uint16_t>(materialInstances, 16 * i + 8, i);
        putAt<std::uint16_t>(textures, 4 * i + 2, i);
        putAt<std::uint16_t>(parameters, 4 * i, i);
        putAt<std::uint16_t>(parameters, 4 * i + 2, i);
        putAt<std::uint32_t>(faceInfos, 8 * i + 4, 3);
        putAt<std::uint64_t>(paths, 8 * i, TexturePath + i);
        putAt<std::uint64_t>(names, 8 * i, mesh.textureParameter);

        // Its streams: the positions, then the rest.
        const auto firstFormat = static_cast<std::uint16_t>(vertexFormats.size() / 4);
        format(0, 1, 0);                  // Positions, three floats
        if (mesh.hasUvs) format(8, 7, 0); // Texture coordinates, two halves
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
    const auto section0 = 64 + 8 * blocks.size() + 12;
    std::vector<std::uint8_t> file(section0);
    putAt<std::uint32_t>(file, 0, 0x4C444D46);
    putAt<std::uint64_t>(file, 8, 64);
    putAt<std::uint32_t>(file, 32, static_cast<std::uint32_t>(blocks.size()));
    putAt<std::uint32_t>(file, 36, 1);
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

    putAt<std::uint32_t>(file, info, 2);
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

// A helicopter's windows kept the sun out of its cabin (2026-10-25); glass drawn into the shadow maps casts
// solid, untested.
void mainViewGlassDropped()
{
    expect(rigid({{16, 5}, {16, 4}}) == Roles{Dropped, MeshRole::Solid}, "Glass the main view alone shows is dropped; cast glass is solid.");
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

// The helicopter's windows (2026-10-25).
void characterGlassDropped()
{
    expect(character({{16, 5}}) == Roles{Dropped}, "A character's glass the main view alone shows is dropped.");
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
    const auto flagsOf = [](const Model& model, std::uint32_t mesh) { return materialOf(partOf(model, mesh), std::nullopt).flags; };
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

    const auto waiting = materialOf(cutOut, std::nullopt);
    expect(waiting.flags == (GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_UV) && waiting.texture == 0 && waiting.alphaThreshold == 0.0f,
        "A cut-out mesh tests no alpha before its texture is copied.");

    const auto textured = materialOf(cutOut, 6);
    expect(textured.flags == (GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_UV | GI_GEOMETRY_ALPHA_TEXTURE) && textured.texture == 7 && textured.alphaThreshold == 0.5f,
        "A cut-out mesh with its texture tests its alpha against a half, as the game's shadow pass does.");

    const auto& standIn = partOf(*model, 1);
    expect(standIn.baseTexture == TexturePath + 1 && standIn.textureIsMask && (materialOf(standIn, 6).flags & GI_GEOMETRY_MASK),
        "A mesh without a base texture cuts by its mask, whose colour is not its own.");

    const auto untextured = materialOf(partOf(*model, 2), 6);
    expect(untextured.flags == GI_GEOMETRY_CUT_OUT && untextured.texture == 0, "A mesh without texture coordinates has no texture.");

    const auto solid = materialOf(partOf(*model, 3), 6);
    expect(solid.flags == GI_GEOMETRY_UV && solid.texture == 7 && solid.alphaThreshold == 0.0f, "A solid mesh takes its texture's colour, not its alpha.");
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
        mainViewGlassDropped();
        skyDomeDropped();
        innerWallsMainViewOnly();
        crossedCardsDropped();
        decalsCast();
        shadowByteThreeDropped();
        hairTaken();
        characterStandInDropped();
        characterMainViewSolid();
        characterGlassDropped();
        bushesAreLeaves();
        sameFileDependsOnSource();
        materialPerRole();
        cutOutWaitsForTexture();
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
