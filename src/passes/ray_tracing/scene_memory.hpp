#pragma once

#include "passes/ray_tracing/scene_description.hpp"

#include <d3d11.h>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The scene memory: the GPU room the scene contents (scene_contents.hpp) take, behind the seam they
// reach it through. gi_scene::Scene is it on Direct3D 12; the tests have their own.
namespace phantom_fx::scene_contents
{
// The pool's writes a frame uploads at most: a model larger is never made.
constexpr std::uint64_t GeometryUploadBytes = 32ull << 20;

// A geometry of a bottom-level structure: where its triangles lie in the pool, and its row of the
// geometry table (GIGeometry).
struct Geometry
{
    std::uint64_t vertexOffset{}; // Bytes into the pool, 16 a vertex
    std::uint32_t vertexCount{};
    std::uint64_t indexOffset{};  // Bytes into the pool, 16-bit indices
    std::uint32_t indexCount{};   // 0: three vertices a triangle
    std::uint32_t flags{};        // GI_GEOMETRY_*; GI_GEOMETRY_CUT_OUT makes it non-opaque
    std::uint32_t texture{};      // As GIGeometry
    float alphaThreshold{};
    std::uint32_t specularTexture{}, normalTexture{}, materialIndex{}; // As GIGeometry
    std::array<float, 3> emission{};
    std::uint64_t normalOffset{}; // As GIGeometry
};

// How a bottom-level structure is built: once, then compacted (Static); or in place, again or refitted,
// as its vertices move (Refit).
enum class Use : std::uint8_t
{
    Static,
    Refit
};

using BlasId = std::uint32_t;

// One of the game's textures for the scene, by its path hash (ModelPart::baseTexture and the others): a shared copy
// of its levels of detail from the first no larger than the scene keeps (gi_textures.hpp), viewed as
// `viewFormat`.
struct SceneTexture
{
    std::uint64_t path{};
    ID3D11Texture2D* texture{};
    DXGI_FORMAT viewFormat{};
    std::uint32_t mips{};
};

class SceneMemory
{
public:
    virtual ~SceneMemory() = default;

    // ---- The geometry pool ----
    // `bytes` of it, 16-byte aligned; nullopt when it has no room.
    virtual std::optional<std::uint64_t> allocate(std::uint64_t bytes) = 0;
    // Gives a range back once the GPU is past this frame.
    virtual void release(std::uint64_t offset, std::uint64_t bytes) = 0;
    // What this frame may still upload, of GeometryUploadBytes.
    virtual std::uint64_t uploadRoom() const = 0;
    // Writes `data` at `offset` this frame; false when the frame's upload has no room left.
    virtual bool upload(std::uint64_t offset, std::span<const std::uint8_t> data) = 0;

    // ---- Bottom-level structures ----
    // With their memory for the largest build and their rows of the geometry table, in order; nullopt
    // without geometries or when either has no room. A static one is queued for building.
    virtual std::optional<BlasId> createBlas(Use use, const std::vector<Geometry>& geometries) = 0;
    // Gone from this frame's instances on; its memory and rows return once the GPU is past the frame.
    virtual void destroyBlas(BlasId id) = 0;
    // A refitted structure: built again this frame (its vertices moved far), or refitted (a little);
    // false when the frame's scratch has no room: not built again, it is not traced until it is.
    virtual bool rebuild(BlasId id) = 0;
    virtual bool refit(BlasId id) = 0;
    // A geometry's material: its flags but GI_GEOMETRY_CUT_OUT (its structure has it), textures and threshold.
    virtual void setMaterial(BlasId id, std::uint32_t geometry, const scene_description::Material& material) = 0;
    // After the frame's changes: the static structures this frame builds within its budget.
    virtual void planBuilds() = 0;
    // Traced this frame: built, or built in this frame's list (planBuilds).
    virtual bool ready(BlasId id) const = 0;

    // ---- The frame's instances ----
    // Under `transform` (object to world rows; null for identity), its InstanceID `kind` (GI_INSTANCE_*):
    // its index in the frame's instances, or nullopt past the instance cap or when not ready.
    virtual std::optional<std::uint32_t> addInstance(BlasId id, const float* transform, std::uint32_t kind) = 0;

    // ---- The game's textures ----
    // A slot of the scene's texture range (g_SceneTextures) viewing the copy; nullopt when none is free.
    virtual std::optional<std::uint32_t> openTexture(const SceneTexture& texture) = 0;
    // The slot is free again once the GPU is past this frame.
    virtual void closeTexture(std::uint32_t slot) = 0;
};
}
