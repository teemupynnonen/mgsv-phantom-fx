#pragma once

#include "passes/ray_tracing/gi_d3d12.hpp"
#include "passes/ray_tracing/scene_memory.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"
#include "range_allocator.hpp"

#include <d3d12.h>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The ray-traced scene on the Direct3D 12 side (docs/DEVELOPMENT.md, "Ray-traced scene"), laid out as
// DXR scenes are: every mesh's triangles in one geometry pool (indexed where the game's are), a
// bottom-level structure per model with a geometry per mesh (opaque geometry flagged so: only cut-out
// geometry runs the alpha test), compacted once built; a table of the geometries the rays reach through
// their instance's first geometry; the game's textures' slots; and the frame's top level of instances.
// The scene contents reach it as their scene memory (scene_memory.hpp). Everything here runs on the
// game's render thread.
namespace phantom_fx::gi_scene
{
using gi_d3d12::ComPtr;
using gi::GIGeometry;
using gi::GIInstanceDesc;
using scene_contents::BlasId;
using scene_contents::Geometry;
using scene_contents::GeometryUploadBytes;
using scene_contents::SceneTexture;
using scene_contents::Use;

constexpr std::uint32_t FramesInFlight = 3;
constexpr std::uint32_t MaxInstances = 1u << 17;
constexpr std::uint32_t MaxGeometries = 1u << 17;

class Scene : public scene_contents::SceneMemory
{
public:
    // The game's textures' slots are GI_MAX_TEXTURES descriptors from `textures` in the frame's
    // shader-visible heap, `descriptorSize` apart.
    Scene(ID3D12Device5* device, D3D12_CPU_DESCRIPTOR_HANDLE textures, UINT descriptorSize);

    // ---- The frame, in this order ----
    // Once the GPU is past the slot's last frame: gives back what the GPU is done with (`completedFence`),
    // texture slots too, and reads the sizes the slot's last builds compact to; `signalValue` ends this
    // frame.
    void beginFrame(std::uint32_t slot, std::uint64_t completedFence, std::uint64_t signalValue);
    // Then the frame's changes (below). Then the static structures this frame builds within its budget,
    // and their compactions: those built are traced this frame.
    void planBuilds() override;
    // Then the frame's instances (addInstance), then the recording: the pool's and table's writes,
    // after which kernels may write the pool (it is in the unordered access state); the bottom levels;
    // the instances' copy, after which kernels may change them (unordered access too); the top level.
    // Between frames the pool, the table and the instances are in the non-pixel shader resource state.
    void recordWrites(ID3D12GraphicsCommandList4& list);
    void recordBottomLevels(ID3D12GraphicsCommandList4& list);
    void recordInstances(ID3D12GraphicsCommandList4& list);
    void recordTopLevel(ID3D12GraphicsCommandList4& list);

    // ---- The geometry pool ----
    // `bytes` of it, 16-byte aligned; nullopt when it has no room.
    std::optional<std::uint64_t> allocate(std::uint64_t bytes) override;
    // Gives a range back once the GPU is past this frame.
    void release(std::uint64_t offset, std::uint64_t bytes) override;
    // Writes `data` at `offset` this frame; false when the frame's upload has no room left.
    bool upload(std::uint64_t offset, std::span<const std::uint8_t> data) override;
    std::uint64_t uploadRoom() const override;

    // ---- Bottom-level structures ----
    // With their memory for the largest build and their rows of the geometry table, in order; nullopt
    // when either has no room. A static one is queued for building.
    std::optional<BlasId> createBlas(Use use, const std::vector<Geometry>& geometries) override;
    // Gone from this frame's instances on; its memory and rows return once the GPU is past the frame.
    void destroyBlas(BlasId id) override;
    // A refitted structure: built again this frame (its vertices moved far), or refitted (a little);
    // false when the frame's scratch has no room: not built again, it is not traced until it is.
    bool rebuild(BlasId id) override;
    bool refit(BlasId id) override;
    // A geometry's material: its flags but GI_GEOMETRY_CUT_OUT (its structure has it), textures and threshold.
    void setMaterial(BlasId id, std::uint32_t geometry, const scene_description::Material& material) override;

    // Traced this frame: built, or built in this frame's list (planBuilds).
    bool ready(BlasId id) const override;
    // Rows of the geometry table made since the last call, whose learnt albedo starts over.
    std::vector<std::uint32_t> takeNewGeometries();

    // ---- The frame's instances ----
    // Under `transform` (object to world rows; null for identity), its InstanceID `kind` (GI_INSTANCE_*):
    // its index in the frame's instances, or nullopt past MaxInstances or when not ready.
    std::optional<std::uint32_t> addInstance(BlasId id, const float* transform, std::uint32_t kind) override;
    std::uint32_t instanceCount() const { return static_cast<std::uint32_t>(instances.size()); }

    // ---- The game's textures ----
    // A slot of the scene's texture range (g_SceneTextures) viewing the copy; nullopt when none is free.
    std::optional<std::uint32_t> openTexture(const SceneTexture& texture) override;
    // The slot is free again once the GPU is past this frame.
    void closeTexture(std::uint32_t textureSlot) override;

    // ---- For the kernels ----
    ID3D12Resource* pool() const { return geometryPool.Get(); }
    ID3D12Resource* geometryTable() const { return table.Get(); }
    ID3D12Resource* instanceBuffer() const { return instanceDescs.Get(); }
    ID3D12Resource* topLevel() const { return tlas.Get(); }
    static constexpr std::uint64_t PoolBytes = 384ull << 20;

    // ---- Diagnostics ----
    struct Stats
    {
        std::uint32_t structures{}, compacted{}, waiting{};
        std::uint32_t builtThisFrame{}, compactedThisFrame{};
        std::uint64_t builtTriangles{}; // This frame
        std::uint64_t compactedFrom{}, compactedTo{}; // Bytes, every compaction so far
        std::uint64_t poolUsed{}, poolCapacity{}, blasUsed{}, blasCapacity{};
        std::uint32_t geometriesUsed{};
    };
    Stats stats() const;

private:
    struct Blas
    {
        bool live{};
        Use use{};
        std::uint32_t generation{};
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
        std::uint32_t firstGeometry{}, triangles{};
        std::uint64_t offset{}, size{}; // In the structure pool
        std::uint64_t scratch{}, updateScratch{}; // Bytes a build and a refit need
        bool built{}, compacted{}, queued{};
    };

    // What the GPU may still use: given back once it is past the frame that signalled `fenceValue`.
    enum class Pool : std::uint8_t
    {
        Geometry,
        Structures,
        Rows
    };
    struct Release
    {
        std::uint64_t fenceValue{};
        Pool pool{};
        std::uint64_t offset{}, size{};
    };

    // The frame's recorded work.
    struct Build
    {
        BlasId id{};
        std::uint32_t generation{};
        bool update{};
        std::uint64_t scratch{}; // Offset into the frame's scratch
        std::uint32_t postbuild{}; // Its compacted size's place in the slot's readback, or NoPostbuild
    };
    struct Compaction
    {
        BlasId id{};
        std::uint64_t from{}, to{};
    };
    // A compacted size read back: the structure compacts in the next frame planned.
    struct Due
    {
        BlasId id{};
        std::uint32_t generation{};
        std::uint64_t size{};
    };
    struct Upload
    {
        std::uint64_t poolOffset{}, uploadOffset{}, bytes{};
    };
    // A static structure built from a slot's list: its compacted size comes back with the slot.
    struct Built
    {
        BlasId id{};
        std::uint32_t generation{};
    };
    // A texture slot, and the texture it views, the GPU may still use: free again once it is past the
    // frame that signalled `fenceValue`.
    struct ClosedTexture
    {
        std::uint64_t fenceValue{};
        std::uint32_t slot{};
        ComPtr<ID3D12Resource> resource;
    };

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputsOf(const Blas& blas, bool update) const;
    D3D12_GPU_VIRTUAL_ADDRESS address(const Blas& blas) const;
    void writeRow(std::uint32_t row, const GIGeometry& geometry);
    std::uint64_t reserveUpload(std::uint64_t bytes);
    void writeTexture(std::uint32_t textureSlot, ID3D12Resource* texture, DXGI_FORMAT format, std::uint32_t mips);

    ID3D12Device5* device;
    ComPtr<ID3D12Resource> geometryPool, blasPool, scratch, table, instanceDescs, tlas, postbuildSizes;
    std::array<ComPtr<ID3D12Resource>, FramesInFlight> uploads, postbuildReadback;
    std::array<std::uint8_t*, FramesInFlight> uploadData{};
    std::uint64_t tlasSize{}, tlasScratch{}, uploadUsed{}, geometryUploaded{}, scratchUsed{};
    std::uint32_t slot{};
    std::uint64_t signalValue{};

    RangeAllocator poolAllocator{PoolBytes}, blasAllocator, rowAllocator{MaxGeometries};
    std::vector<Release> releases;
    std::vector<Blas> structures;
    std::vector<BlasId> freeIds, buildQueue;
    std::vector<Due> compactionsDue;
    std::array<std::vector<Built>, FramesInFlight> builtInSlot;

    // The table on the CPU, and its rows written since the last frame; rows made since takeNewGeometries.
    std::vector<GIGeometry> rows = std::vector<GIGeometry>(MaxGeometries);
    std::vector<std::uint32_t> dirtyRows, newRows;

    std::vector<Upload> poolUploads;
    std::vector<Build> builds;
    std::vector<Compaction> compactions;
    std::vector<GIInstanceDesc> instances;
    Stats tally;

    // The game's textures by slot, a descriptor each from `textureDescriptors`; the free slots, and those
    // the GPU may still read.
    D3D12_CPU_DESCRIPTOR_HANDLE textureDescriptors{};
    UINT descriptorSize{};
    std::vector<OpenedTexture> textures = std::vector<OpenedTexture>(GI_MAX_TEXTURES);
    std::vector<std::uint32_t> freeTextures = [] {
        std::vector<std::uint32_t> free;
        for (auto textureSlot = GI_MAX_TEXTURES; textureSlot > 0; --textureSlot) free.push_back(textureSlot - 1);
        return free;
    }();
    std::vector<ClosedTexture> closedTextures;
};
}
