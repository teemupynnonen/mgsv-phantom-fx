#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// What the ray-traced scene takes of the game's model files (docs/DEVELOPMENT.md, "Scene objects",
// "Ray-traced scene"): a model file's meshes, each with its mesh role, from the file's bytes and where
// the model was found; and what a mesh's role makes of its row of the geometry table. The renderer
// uploads what it is given.
namespace phantom_fx::scene_description
{
using Transform = std::array<float, 12>; // Object to world: three rows applied as dot(row, (v, 1))
using Vertex = std::array<float, 3>;

// The most bones a model is posed with (Model::bones).
constexpr std::uint32_t MaxBones = 1024;

// Where the scene found a model. Only the object brush changes its meshes' roles; whether a model is
// skinned comes from its file.
enum class Source : std::uint8_t
{
    VisibilityManager,
    ObjectBrush,
    ModelList
};

// What a mesh is to the scene's rays (GI_GEOMETRY_* in shaders/gi_shared.h).
enum class MeshRole : std::uint8_t
{
    Solid,
    CutOut,       // Its texture cuts holes in it
    Leaf,         // The object brush's bushes and grass: cut out, and thin, they let light through
    Hair,         // A character's see-through mesh the game never casts with: cut out, and rays towards lights pass it in part
    Glass,        // Every ray passes it but a reflection's, which it reflects by its mask
    Mirror,       // Every ray passes it but a reflection's, which it reflects
    MainViewOnly, // A rigid model's solid mesh the game never draws into its shadow maps: rays towards lights pass it
    Emissive,     // A self-lit mesh (a bulb, a sign): it gives light, takes none, and rays towards lights pass it
    ShadowOnly,   // A rigid model's stand-in for main-view cut-outs the scene drops (a tarp's): only rays towards lights meet it
    SunShadowOnly // Any other rigid model's solid stand-in: of the rays towards lights, only the sun's meet it
};

// Why the scene leaves a model out. A revealed scene (engine_scene::Reveal) takes it all the same, with
// this reason, so a debug case can name what the game drew and the scene lacks.
enum class Rejection : std::uint8_t
{
    None,
    HiddenFromEveryPass,
    FadedOut,
    MeshGroupsHidden
};

enum class ModelFile : std::uint8_t
{
    Unreadable, // Not an FMDL 2.0x the scene reads, or not all of it yet
    Rigid,
    Skinned
};

// How a mesh's layer lies over its base texture, by the game's pixel shaders: it multiplies the base (MulLayerTexture), or takes
// its place on the textures' encoded values (SRGBBlendLayerTexture) or in linear light (BlendLayerTexture).
enum class LayerBlend : std::uint8_t
{
    Multiply,
    BlendEncoded,
    BlendLinear
};

// A mesh's layer, for every hit's colour: it lies over the base texture and can be all of a mesh's colour. Its textures' path
// hashes (Layer_Tex_SRGB, LayerMask_Tex_LIN, 0 without), its coordinates (GIGeometry::layerMapping; each vertex's second ones,
// packed as ModelPart::uvs), how it lies, its mask's channel, and each vertex's coordinates on its mask where those are not
// its base texture's.
struct Layer
{
    std::uint64_t texture{}, maskTexture{};
    std::array<float, 4> mapping{};
    LayerBlend blend{};
    bool isMaskInBlue{};
    std::vector<std::uint32_t> uvs, maskUvs;
};

// A mesh of a model: its index in the model file and its mesh group, its role, its material's base
// texture (its colour, and the alpha a cut-out mesh cuts with: the path hash of its Base_Tex_SRGB, 0
// without) or, for a mesh with none, its mask (Mask_Tex_LIN, cutting with its alpha); glass's mask (Base_Tex_LIN, how much
// of it reflects); a mirror has none; its vertices in
// object space and, when it has them, their texture coordinates (two halves packed in a uint, a vertex
// each); its most detailed triangles, three indices each. A skinned model's mesh also has each vertex's
// four bones (the model's bone numbers, 16 bits each, the first lowest) and their weights (8-bit unorms,
// the first lowest). For path tracing's hits: its SRM and normal textures' path hashes (SpecularMap_Tex_LIN,
// NormalMap_Tex_NRM, 0 without), its row of the game's material table (MatParamIndex_0), each vertex's normal
// and tangent (four halves each, as the file has them) and a self-lit mesh's light (SelfColor x
// SelfColorIntensity, in the game's units, none on any other). Its layer, unless it has none, its technique is not known to
// read one or its vertices lack the coordinates that takes.
struct ModelPart
{
    std::uint32_t mesh{};
    std::uint32_t group{};
    MeshRole role{};
    std::uint64_t baseTexture{};
    bool textureIsMask{};
    std::uint64_t specularTexture{}, normalTexture{};
    std::optional<Layer> layer;
    std::uint32_t materialIndex{};
    std::array<float, 3> emission{};
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> uvs;
    std::vector<std::array<std::uint16_t, 8>> normals;
    std::vector<std::uint16_t> indices;
    std::vector<std::uint64_t> bones;
    std::vector<std::uint32_t> weights;
    // Where the game's draw of these triangles starts in the model's index buffer and how many it draws;
    // and a skinned mesh's palette slots, the model's bone in each (VSBones' g_vsBone, gi_inputs::DrawnBones).
    std::uint32_t firstIndex{}, indexCount{};
    std::vector<std::uint16_t> boneGroup;
    // Diagnostics (the shadow probe): the FMDL mesh's alpha and shadow bytes, and its material's technique.
    std::uint8_t alphaBits{}, shadowBits{};
    std::uint64_t technique{};
    bool isDropped{}; // A mesh the scene drops, held only by a revealed scene (describeModel's keepsDropped)
};

// Diagnostics (the shadow probe): a mesh of a model file the scene drops, with its alpha and shadow bytes.
struct DroppedMesh
{
    std::uint32_t mesh{};
    std::uint8_t alphaBits{}, shadowBits{};
    std::uint64_t technique{};
};

// A model the game has loaded (engine_scene.hpp): its meshes the scene takes; a skinned model's bones.
struct Model
{
    std::uint64_t hash{}; // The model file's path hash, the same for every copy of the model
    std::vector<ModelPart> parts;
    std::uint32_t bones{}; // 0: rigid, placed as a whole (ModelInstance); else posed (CharacterInstance)
    std::vector<DroppedMesh> dropped;
};

// One placement of a model, by its hash, and its mesh groups (ModelPart::group, the first 64) the game
// hides, a bit each.
struct ModelInstance
{
    std::uint64_t hash{};
    Transform transform{};
    std::uint64_t hiddenGroups{};
    Rejection rejection{};
};

// One of the game's characters (engine_scene.hpp): a skinned model in the pose the game gives it this
// frame. `id` stays the same while the game keeps it; `palette` has a 3x4 matrix (three rows of four)
// for each of the model's bones; `shownGroups` says which of its mesh groups the game draws.
struct CharacterInstance
{
    std::uint64_t id{};
    std::uint64_t hash{};
    Transform transform{};
    std::vector<float> palette;
    std::vector<bool> shownGroups;
    Rejection rejection{};

    // Diagnostics (the shadow probe): each mesh group's state byte as read, and which of the model's
    // meshes (Model::parts) this frame's draws posed (engine_scene::take).
    std::vector<std::uint8_t> groupStates;
    std::vector<bool> drawnParts;
};

// A mesh's row of the geometry table but its place in the pool (GIGeometry): its GI_GEOMETRY_* flags, its
// textures (1 + their index in the scene's textures, 0 for none), where the rays test its texture's alpha
// the alpha below which light passes, its row of the game's material table and its light.
struct Material
{
    std::uint32_t flags{}, texture{};
    float alphaThreshold{};
    std::uint32_t specularTexture{}, normalTexture{}, materialIndex{};
    std::array<float, 3> emission{};
    std::uint32_t layerTexture{}, layerMaskTexture{};
};

// The game's textures a mesh's material uses, each in the scene's textures once copied.
struct MaterialTextures
{
    std::optional<std::uint32_t> base, specular, normal, layer, layerMask;
};

ModelFile fileKindOf(std::span<const std::uint8_t> file);

// The model in `file` as the scene takes it, found at `source`; null when the bytes are no FMDL 2.0x
// the scene reads. With `keepsDropped`, for a revealed scene, the meshes the scene drops are kept as
// solid ones, marked ModelPart::isDropped.
std::shared_ptr<const Model> describeModel(std::uint64_t hash, std::span<const std::uint8_t> file, Source source, bool keepsDropped = false);

// Cut-out, leaf and hair meshes: the rays test their texture's alpha.
bool isCutOut(MeshRole role);

// The path hashes of the game's textures a mesh's material uses, in MaterialTextures' order, 0 for those it has not.
std::array<std::uint64_t, 5> texturePathsOf(const ModelPart& part);

// The mesh's material, with its textures once the game's are copied into the scene's textures: its base
// texture's colour and, with its mask, its layer for the hits' albedo, a cut-out mesh's alpha for the shadow rays' test,
// and the SRM and normal textures path tracing's hits read.
Material materialOf(const ModelPart& part, const MaterialTextures& textures);
}
