// The ray-traced scene on the Direct3D 12 side; gi_scene.hpp describes it.
#include "passes/ray_tracing/gi_scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace phantom_fx::gi_scene
{
namespace
{
using namespace gi_d3d12;

// The structures' memory, and the scratch the frame's static builds share after the top level's.
constexpr std::uint64_t BlasPoolBytes = 384ull << 20;
constexpr std::uint64_t BuildScratchBytes = 64ull << 20;
// Each frame slot's upload buffer: the pool's writes, the table's, the instances.
constexpr std::uint64_t UploadBytes = GeometryUploadBytes + std::uint64_t{MaxInstances} * sizeof(GIInstanceDesc) + (8ull << 20);
// The static structures a frame builds at most: the rest wait for the next frames, so a new area
// costs a few frames a little rather than one frame a lot.
constexpr std::uint32_t BuildsPerFrame = 128;
constexpr std::uint64_t BuildTrianglesPerFrame = 300'000;

// What a mesh's material says of its row of the table. Its structure keeps GI_GEOMETRY_CUT_OUT.
void applyMaterial(GIGeometry& row, const scene_description::Material& material)
{
    row.flags = (row.flags & GI_GEOMETRY_CUT_OUT) | (material.flags & ~GI_GEOMETRY_CUT_OUT);
    row.texture = material.texture;
    row.alphaThreshold = material.alphaThreshold;
    row.texelsPerMetre = material.texelsPerMetre;
    row.specularTexture = material.specularTexture;
    row.normalTexture = material.normalTexture;
    row.materialIndex = material.materialIndex;
    row.emission = {material.emission[0], material.emission[1], material.emission[2]};
    row.layerTexture = material.layerTexture;
    row.layerMaskTexture = material.layerMaskTexture;
}

// Opacity micromaps: the arrays' memory and their builds' scratch; the frames a static structure waits for its cut-out meshes'
// textures and for room in the frames' bakes.
constexpr std::uint64_t MicromapPoolBytes = 128ull << 20, MicromapScratchBytes = 32ull << 20;
constexpr std::uint32_t MicromapWaitFrames = 90;
// Each triangle's micro-triangles, 4 to this. Few leave rays testing alpha where they need not, and the coarser ones change the
// picture under rays that take unknown ones as settled (TraceClosest): 2, 4 and 5 moved 5.1, 4.7 and 1.8% of a jungle frame, 6 no
// more than doing nothing, and 6 is the fastest of them too (2026-11-02). A jungle takes 16 MB of it.
constexpr std::uint32_t MicromapLevel = 6;
// The coarsest a big mesh goes: BakeMicromaps writes whole words of 16 micro-triangles.
constexpr std::uint32_t CoarsestMicromapLevel = 2;
// The micro-triangles a frame bakes at most, each sampled 15 times (BakeMicromaps): the rest wait.
constexpr std::uint64_t MicroTrianglesPerFrame = 16'000'000;
constexpr std::uint64_t MicromapArrayAlignment = 128; // D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_BYTE_ALIGNMENT
// Texture slots go above a mesh's GPU address in its micromap's key, which fits under it (linkMicromaps).
constexpr int MicromapKeyTextureShift = 48;

constexpr auto BakeRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE;

// A cut-out geometry the rays test alpha on, whose micromap the scene bakes. Not hair, which dims rays layer by layer (PassHair) and
// so must see each hit, nor glass, whose alpha is no hole: every ray but a reflection's passes it (CutOutBlocks). What rays towards
// lights pass (a tree fern's fronds) only in a structure they skip whole (GI_MASK_VIEW_ONLY): elsewhere an opaque micro-triangle
// would end their search on it, where they skip it as a candidate.
bool isMicromapped(std::uint32_t flags, bool castsNothing)
{
    if (!(flags & GI_GEOMETRY_CUT_OUT) || !(flags & GI_GEOMETRY_UV) || (flags & (GI_GEOMETRY_HAIR | GI_GEOMETRY_GLASS))) return false;

    return castsNothing || !(flags & GI_GEOMETRY_NO_SHADOW);
}

// A mesh is baked whole in one frame, so a big one goes coarser to stay within a frame's bake: a dispatch that runs for seconds loses
// the device.
std::uint32_t micromapLevelOf(std::uint32_t triangles)
{
    auto level = MicromapLevel;
    while (level > CoarsestMicromapLevel && (std::uint64_t{triangles} << (2 * level)) > MicroTrianglesPerFrame) --level;

    return level;
}
}

Scene::Scene(ID3D12Device5* device, D3D12_CPU_DESCRIPTOR_HANDLE textures, UINT descriptorSize, bool isMicromapCapable)
    : device(device), blasAllocator(BlasPoolBytes), textureDescriptors(textures), descriptorSize(descriptorSize), isMicromapCapable(isMicromapCapable),
      micromapAllocator(isMicromapCapable ? MicromapPoolBytes : 0)
{
    if (isMicromapCapable)
    {
        bake = createStorage(device, BakeBytes, "Creating the micromaps' bake buffer");
        micromapPool = createBuffer(device, MicromapPoolBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the micromap pool");
        micromapScratch = createStorage(device, MicromapScratchBytes, "Creating the micromaps' build scratch");
    }

    geometryPool = createBuffer(device, PoolBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the geometry pool");
    blasPool = createBuffer(device, BlasPoolBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the acceleration structure pool");
    table = createBuffer(device, std::uint64_t{MaxGeometries} * sizeof(GIGeometry), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_FLAG_NONE, "Creating the geometry table");
    instanceDescs = createBuffer(device, std::uint64_t{MaxInstances} * sizeof(GIInstanceDesc), D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the instance list");
    postbuildSizes = createStorage(device, std::uint64_t{BuildsPerFrame} * 8, "Creating the compacted sizes");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    inputs.NumDescs = MaxInstances;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
    tlasSize = prebuild.ResultDataMaxSizeInBytes;
    tlasScratch = aligned(prebuild.ScratchDataSizeInBytes, 256);
    tlas = createBuffer(device, tlasSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the scene's acceleration structure");
    scratch = createStorage(device, tlasScratch + BuildScratchBytes, "Creating the build scratch");

    for (std::uint32_t i = 0; i < FramesInFlight; ++i)
    {
        uploads[i] = createBuffer(device, UploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
            "Creating the scene's upload buffer");
        void* data{};
        check(uploads[i]->Map(0, nullptr, &data), "Mapping the scene's upload buffer");
        uploadData[i] = static_cast<std::uint8_t*>(data);
        postbuildReadback[i] = createBuffer(device, std::uint64_t{BuildsPerFrame} * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_FLAG_NONE, "Creating the compacted sizes' readback");
    }

    for (std::uint32_t textureSlot = 0; textureSlot < GI_MAX_TEXTURES; ++textureSlot) writeTexture(textureSlot, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, 1);
}

void Scene::beginFrame(std::uint32_t frameSlot, std::uint64_t completedFence, std::uint64_t frameSignal)
{
    slot = frameSlot;
    signalValue = frameSignal;
    std::erase_if(releases, [&](const Release& release) {
        if (release.fenceValue > completedFence) return false;

        if (release.pool == Pool::Geometry) poolAllocator.release(release.offset, release.size);
        else if (release.pool == Pool::Structures) blasAllocator.release(release.offset, release.size);
        else if (release.pool == Pool::Rows) rowAllocator.release(release.offset, release.size);
        else if (release.pool == Pool::Micromaps) micromapAllocator.release(release.offset, release.size);
        return true;
    });
    std::erase_if(closedTextures, [&](const ClosedTexture& closed) {
        if (closed.fenceValue > completedFence) return false;

        freeTextures.push_back(closed.slot);
        return true;
    });

    // The slot's last static builds wrote their compacted sizes; the GPU is past them.
    auto& built = builtInSlot[slot];
    if (!built.empty())
    {
        void* data{};
        const D3D12_RANGE range{0, built.size() * 8};
        if (SUCCEEDED(postbuildReadback[slot]->Map(0, &range, &data)))
        {
            const auto* sizes = static_cast<const std::uint64_t*>(data);
            for (std::size_t i = 0; i < built.size(); ++i)
                if (sizes[i]) compactionsDue.push_back({built[i].id, built[i].generation, sizes[i]});

            const D3D12_RANGE none{};
            postbuildReadback[slot]->Unmap(0, &none);
        }

        built.clear();
    }

    poolUploads.clear();
    builds.clear();
    compactions.clear();
    instances.clear();
    jobs.clear();
    micromapBuilds.clear();
    jobGroups = 0;
    bakeUsed = micromapScratchUsed = frameMicroTriangles = 0;
    uploadUsed = geometryUploaded = 0;
    scratchUsed = tlasScratch;
    tally.builtThisFrame = tally.compactedThisFrame = 0;
    tally.builtTriangles = 0;
}

void Scene::planBuilds()
{
    std::uint32_t staticBuilds{};
    std::uint64_t triangles{};
    std::vector<BlasId> waiting;
    tally.waitingForMicromaps = 0;
    for (const auto id : buildQueue)
    {
        auto& blas = structures[id];
        if (!blas.live || !blas.queued) continue;

        // Its micromaps need all its cut-out geometries' textures and room in a bake: it waits for them a while.
        if (blas.mayHaveMicromaps) linkMicromaps(blas);
        if (blas.mayHaveMicromaps)
        {
            if (blas.framesWaited++ < MicromapWaitFrames)
            {
                ++tally.waitingForMicromaps;
                waiting.push_back(id);
                continue;
            }

            ++tally.builtWithoutMicromaps;
            releaseMicromaps(blas);
            blas.mayHaveMicromaps = false;
        }

        if (staticBuilds >= BuildsPerFrame || triangles >= BuildTrianglesPerFrame || scratchUsed + blas.scratch > tlasScratch + BuildScratchBytes)
        {
            waiting.push_back(id);
            continue;
        }

        // The readback's k-th compacted size is the slot's k-th static build's.
        builds.push_back({id, blas.generation, false, scratchUsed, staticBuilds++});
        builtInSlot[slot].push_back({id, blas.generation});
        scratchUsed += blas.scratch;
        triangles += blas.triangles;
        blas.queued = false;
        blas.built = true;
    }

    buildQueue = std::move(waiting);
    tally.waiting = static_cast<std::uint32_t>(buildQueue.size());

    // Built structures copied into the room their compacted size needs; the room they leave returns
    // once the GPU is past this frame, whose instances use the copies.
    for (const auto& due : compactionsDue)
    {
        auto& blas = structures[due.id];
        if (!blas.live || blas.generation != due.generation || blas.compacted || due.size >= blas.size) continue;

        const auto to = blasAllocator.allocate(due.size, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        if (!to) continue;

        compactions.push_back({due.id, blas.offset, *to});
        releases.push_back({signalValue, Pool::Structures, blas.offset, blas.size});
        tally.compactedFrom += blas.size;
        tally.compactedTo += due.size;
        ++tally.compactedThisFrame;
        blas.offset = *to;
        blas.size = due.size;
        blas.compacted = true;
    }

    compactionsDue.clear();
}

void Scene::recordWrites(ID3D12GraphicsCommandList4& list)
{
    // The pool: the frame's uploads, then writable for the kernels that make geometry.
    auto barrier = transition(geometryPool.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        poolUploads.empty() ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST);
    list.ResourceBarrier(1, &barrier);
    if (!poolUploads.empty())
    {
        for (const auto& upload : poolUploads)
            list.CopyBufferRegion(geometryPool.Get(), upload.poolOffset, uploads[slot].Get(), upload.uploadOffset, upload.bytes);

        barrier = transition(geometryPool.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list.ResourceBarrier(1, &barrier);
    }

    // The table's rows written since the last frame, in runs.
    if (dirtyRows.empty()) return;

    std::sort(dirtyRows.begin(), dirtyRows.end());
    dirtyRows.erase(std::unique(dirtyRows.begin(), dirtyRows.end()), dirtyRows.end());
    barrier = transition(table.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list.ResourceBarrier(1, &barrier);
    std::size_t written{};
    while (written < dirtyRows.size())
    {
        std::size_t end = written + 1;
        while (end < dirtyRows.size() && dirtyRows[end] == dirtyRows[end - 1] + 1) ++end;

        const auto first = dirtyRows[written];
        const auto bytes = (end - written) * sizeof(GIGeometry);
        const auto at = reserveUpload(bytes);
        if (at == UploadBytes) break;

        std::memcpy(uploadData[slot] + at, &rows[first], bytes);
        list.CopyBufferRegion(table.Get(), std::uint64_t{first} * sizeof(GIGeometry), uploads[slot].Get(), at, bytes);
        written = end;
    }

    // What did not fit goes next frame.
    dirtyRows.erase(dirtyRows.begin(), dirtyRows.begin() + static_cast<std::ptrdiff_t>(written));
    barrier = transition(table.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list.ResourceBarrier(1, &barrier);
}

void Scene::recordGeometryReady(ID3D12GraphicsCommandList4& list)
{
    D3D12_RESOURCE_BARRIER readable[]{uavBarrier(), transition(geometryPool.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    list.ResourceBarrier(static_cast<UINT>(std::size(readable)), readable);
}

void Scene::recordMicromaps(ID3D12GraphicsCommandList4& list)
{
    if (micromapBuilds.empty()) return;

    // The jobs' descriptions, states and indices, read by the arrays' builds and the bottom levels'.
    D3D12_RESOURCE_BARRIER ready[]{uavBarrier(), transition(bake.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, BakeRead)};
    list.ResourceBarrier(static_cast<UINT>(std::size(ready)), ready);
    for (const auto& build : micromapBuilds)
    {
        D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC array{};
        array.NumOmmHistogramEntries = static_cast<UINT>(build.histogram.size());
        array.pOmmHistogram = build.histogram.data();
        array.InputBuffer = bake->GetGPUVirtualAddress();
        array.PerOmmDescs = {bake->GetGPUVirtualAddress() + build.descOffset, sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC)};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
        desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;
        desc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        desc.Inputs.NumDescs = 1;
        desc.Inputs.pOpacityMicromapArrayDesc = &array;
        desc.DestAccelerationStructureData = micromapPool->GetGPUVirtualAddress() + build.destination;
        desc.ScratchAccelerationStructureData = micromapScratch->GetGPUVirtualAddress() + build.scratch;
        list.BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
    }

    auto pool = transition(geometryPool.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list.ResourceBarrier(1, &pool);
    for (const auto& build : micromapBuilds) list.CopyBufferRegion(geometryPool.Get(), build.indexTo, bake.Get(), build.indexFrom, build.indexBytes);

    pool = transition(geometryPool.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_RESOURCE_BARRIER done[]{uavBarrier(), pool};
    list.ResourceBarrier(static_cast<UINT>(std::size(done)), done);
}

void Scene::recordBottomLevels(ID3D12GraphicsCommandList4& list)
{
    bool postbuild{};
    for (const auto& build : builds)
    {
        // Gone since it was planned: nothing reads what the build would write.
        const auto& blas = structures[build.id];
        if (!blas.live || blas.generation != build.generation) continue;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
        desc.Inputs = inputsOf(blas, build.update);
        desc.DestAccelerationStructureData = address(blas);
        desc.SourceAccelerationStructureData = build.update ? address(blas) : 0;
        desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress() + build.scratch;
        if (blas.use == Use::Static)
        {
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC info{postbuildSizes->GetGPUVirtualAddress() + std::uint64_t{build.postbuild} * 8,
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE};
            list.BuildRaytracingAccelerationStructure(&desc, 1, &info);
            postbuild = true;
        }
        else
            list.BuildRaytracingAccelerationStructure(&desc, 0, nullptr);

        ++tally.builtThisFrame;
        tally.builtTriangles += blas.triangles;
    }

    const auto all = uavBarrier();
    list.ResourceBarrier(1, &all);
    if (postbuild)
    {
        auto barrier = transition(postbuildSizes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list.ResourceBarrier(1, &barrier);
        list.CopyBufferRegion(postbuildReadback[slot].Get(), 0, postbuildSizes.Get(), 0, builtInSlot[slot].size() * 8);
        barrier = transition(postbuildSizes.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list.ResourceBarrier(1, &barrier);
    }

    for (const auto& compaction : compactions)
        list.CopyRaytracingAccelerationStructure(blasPool->GetGPUVirtualAddress() + compaction.to, blasPool->GetGPUVirtualAddress() + compaction.from,
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);

    if (!compactions.empty()) list.ResourceBarrier(1, &all);
    if (!micromapBuilds.empty())
    {
        const auto writable = transition(bake.Get(), BakeRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list.ResourceBarrier(1, &writable);
    }
}

void Scene::recordInstances(ID3D12GraphicsCommandList4& list)
{
    auto barrier = transition(instanceDescs.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list.ResourceBarrier(1, &barrier);
    if (!instances.empty())
    {
        const auto bytes = instances.size() * sizeof(GIInstanceDesc);
        const auto at = reserveUpload(bytes);
        if (at == UploadBytes) throw std::runtime_error("The scene's upload buffer has no room for the frame's instances.");

        std::memcpy(uploadData[slot] + at, instances.data(), bytes);
        list.CopyBufferRegion(instanceDescs.Get(), 0, uploads[slot].Get(), at, bytes);
    }

    barrier = transition(instanceDescs.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list.ResourceBarrier(1, &barrier);
}

void Scene::recordTopLevel(ID3D12GraphicsCommandList4& list)
{
    D3D12_RESOURCE_BARRIER ready[]{uavBarrier(), transition(instanceDescs.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    list.ResourceBarrier(static_cast<UINT>(std::size(ready)), ready);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC top{};
    top.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    top.Inputs.NumDescs = instanceCount();
    top.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    top.Inputs.InstanceDescs = instanceDescs->GetGPUVirtualAddress();
    top.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
    top.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    list.BuildRaytracingAccelerationStructure(&top, 0, nullptr);

    const auto all = uavBarrier();
    list.ResourceBarrier(1, &all);
}

std::optional<std::uint64_t> Scene::allocate(std::uint64_t bytes)
{
    return poolAllocator.allocate(aligned(bytes, 16), 16);
}

void Scene::release(std::uint64_t offset, std::uint64_t bytes)
{
    releases.push_back({signalValue, Pool::Geometry, offset, aligned(bytes, 16)});
}

bool Scene::upload(std::uint64_t offset, std::span<const std::uint8_t> data)
{
    if (geometryUploaded + data.size() > GeometryUploadBytes) return false;

    const auto at = reserveUpload(data.size());
    if (at == UploadBytes) return false;

    std::memcpy(uploadData[slot] + at, data.data(), data.size());
    poolUploads.push_back({offset, at, data.size()});
    geometryUploaded += data.size();
    return true;
}

std::uint64_t Scene::uploadRoom() const
{
    return GeometryUploadBytes - geometryUploaded;
}

// A structure of this model's geometries; one whose every geometry lets rays towards lights through is marked
// castsNothing, so its instances take GI_MASK_VIEW_ONLY (addInstance) and those rays skip them whole.
std::optional<BlasId> Scene::createBlas(Use use, const std::vector<Geometry>& geometries)
{
    if (geometries.empty()) return std::nullopt;

    Blas blas;
    blas.use = use;
    blas.castsNothing = std::ranges::all_of(geometries, [](const Geometry& geometry) { return (geometry.material.flags & GI_GEOMETRY_NO_SHADOW) != 0; });
    const auto poolAddress = geometryPool->GetGPUVirtualAddress();
    for (const auto& geometry : geometries)
    {
        D3D12_RAYTRACING_GEOMETRY_DESC desc{};
        desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        // Hair and glass dim a ray once for each layer it crosses (PassHair, PaneTransmittance), so a ray must meet each of
        // their triangles once.
        if (!(geometry.material.flags & GI_GEOMETRY_CUT_OUT)) desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        else if (geometry.material.flags & (GI_GEOMETRY_HAIR | GI_GEOMETRY_GLASS)) desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;
        desc.Triangles.VertexBuffer = {poolAddress + geometry.vertexOffset, 16};
        desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        desc.Triangles.VertexCount = geometry.vertexCount;
        if (geometry.indexCount)
        {
            desc.Triangles.IndexBuffer = poolAddress + geometry.indexOffset;
            desc.Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
            desc.Triangles.IndexCount = geometry.indexCount;
        }

        blas.geometries.push_back(desc);
        blas.triangles += (geometry.indexCount ? geometry.indexCount : geometry.vertexCount) / 3;
        if (isMicromapCapable && geometry.indexCount && isMicromapped(geometry.material.flags, blas.castsNothing))
            blas.micromapped.push_back(static_cast<std::uint32_t>(blas.geometries.size() - 1));
    }

    auto inputs = inputsOf(blas, false);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

    // Room for it with all its micromaps too, as it has them all or none (linkMicromaps): with some of each it can need more than
    // with either, by up to 19 kB of a 800 kB structure, and its build then writes into the next structure's room. That hung the GPU
    // in about half of the launches where a texture came late (2026-11-02).
    blas.mayHaveMicromaps = !blas.micromapped.empty();
    if (blas.mayHaveMicromaps)
    {
        auto geometriesWith = blas.geometries;
        std::vector<D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC> triangles;
        std::vector<D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC> links;
        triangles.reserve(geometries.size());
        links.reserve(geometries.size());
        for (const auto i : blas.micromapped)
        {
            triangles.push_back(geometriesWith[i].Triangles);
            links.push_back({{bake->GetGPUVirtualAddress(), 4}, DXGI_FORMAT_R32_UINT, 0, micromapPool->GetGPUVirtualAddress()});
            geometriesWith[i].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES;
            geometriesWith[i].OmmTriangles = {&triangles.back(), &links.back()};
        }

        auto with = inputs;
        with.pGeometryDescs = geometriesWith.data();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildWith{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&with, &prebuildWith);
        prebuild.ResultDataMaxSizeInBytes = std::max(prebuild.ResultDataMaxSizeInBytes, prebuildWith.ResultDataMaxSizeInBytes);
        prebuild.ScratchDataSizeInBytes = std::max(prebuild.ScratchDataSizeInBytes, prebuildWith.ScratchDataSizeInBytes);
        prebuild.UpdateScratchDataSizeInBytes = std::max(prebuild.UpdateScratchDataSizeInBytes, prebuildWith.UpdateScratchDataSizeInBytes);
        blas.micromapTriangles.resize(geometries.size());
        blas.links.resize(geometries.size());
    }

    const auto first = rowAllocator.allocate(geometries.size());
    if (!first) return std::nullopt;

    const auto offset = blasAllocator.allocate(prebuild.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    if (!offset)
    {
        rowAllocator.release(*first, geometries.size());
        return std::nullopt;
    }

    blas.live = true;
    blas.firstGeometry = static_cast<std::uint32_t>(*first);
    blas.offset = *offset;
    blas.size = prebuild.ResultDataMaxSizeInBytes;
    blas.scratch = aligned(prebuild.ScratchDataSizeInBytes, 256);
    blas.updateScratch = aligned(prebuild.UpdateScratchDataSizeInBytes, 256);

    for (std::uint32_t i = 0; i < geometries.size(); ++i)
    {
        const auto& geometry = geometries[i];
        GIGeometry row{.vertexOffset = static_cast<std::uint32_t>(geometry.vertexOffset),
            .indexOffset = geometry.indexCount ? static_cast<std::uint32_t>(geometry.indexOffset) : GI_NO_INDICES,
            .flags = geometry.material.flags & GI_GEOMETRY_CUT_OUT,
            .layerMaskUvOffset = static_cast<std::uint32_t>(geometry.layerMaskUvOffset),
            .normalOffset = static_cast<std::uint32_t>(geometry.normalOffset),
            .layerUvOffset = static_cast<std::uint32_t>(geometry.layerUvOffset),
            .layerMapping = {geometry.layerMapping[0], geometry.layerMapping[1], geometry.layerMapping[2], geometry.layerMapping[3]}};
        applyMaterial(row, geometry.material);
        writeRow(blas.firstGeometry + i, row);
        newRows.push_back(blas.firstGeometry + i);
    }

    BlasId id;
    if (!freeIds.empty())
    {
        id = freeIds.back();
        freeIds.pop_back();
        blas.generation = structures[id].generation + 1;
    }
    else
    {
        id = static_cast<BlasId>(structures.size());
        structures.emplace_back();
    }

    if (use == Use::Static)
    {
        blas.queued = true;
        buildQueue.push_back(id);
    }

    structures[id] = std::move(blas);
    return id;
}

void Scene::destroyBlas(BlasId id)
{
    auto& blas = structures[id];
    if (!blas.live) return;

    releases.push_back({signalValue, Pool::Structures, blas.offset, blas.size});
    releases.push_back({signalValue, Pool::Rows, blas.firstGeometry, blas.geometries.size()});
    releaseMicromaps(blas);

    const auto generation = blas.generation;
    blas = {};
    blas.generation = generation;
    freeIds.push_back(id);
}

bool Scene::rebuild(BlasId id)
{
    auto& blas = structures[id];
    if (scratchUsed + blas.scratch > tlasScratch + BuildScratchBytes)
    {
        blas.built = false;
        return false;
    }

    if (blas.mayHaveMicromaps) linkMicromaps(blas);
    builds.push_back({id, blas.generation, false, scratchUsed, 0});
    scratchUsed += blas.scratch;
    blas.built = true;
    return true;
}

bool Scene::refit(BlasId id)
{
    auto& blas = structures[id];
    if (!blas.built) return rebuild(id);
    if (scratchUsed + blas.updateScratch > tlasScratch + BuildScratchBytes) return false;

    builds.push_back({id, blas.generation, true, scratchUsed, 0});
    scratchUsed += blas.updateScratch;
    return true;
}

void Scene::setMaterial(BlasId id, std::uint32_t geometry, const scene_description::Material& material)
{
    const auto& blas = structures[id];
    if (!blas.live || geometry >= blas.geometries.size()) return;

    auto row = rows[blas.firstGeometry + geometry];
    applyMaterial(row, material);
    writeRow(blas.firstGeometry + geometry, row);
}

bool Scene::ready(BlasId id) const
{
    const auto& blas = structures[id];
    if (!blas.live) return false;

    return blas.built;
}

std::vector<std::uint32_t> Scene::takeNewGeometries()
{
    return std::exchange(newRows, {});
}

std::optional<std::uint32_t> Scene::addInstance(BlasId id, const float* transform, std::uint32_t kind)
{
    if (instances.size() >= MaxInstances || !ready(id)) return std::nullopt;

    const auto& blas = structures[id];
    GIInstanceDesc desc{};
    for (int row = 0; row < 3; ++row)
    {
        float* to = &desc.transform[row].x;
        for (int column = 0; column < 4; ++column) to[column] = transform ? transform[row * 4 + column] : (row == column ? 1.0f : 0.0f);
    }

    // Only the terrain and the sea are one-sided (GI_RAY_FLAGS); the game's meshes are often open shells. The
    // sea's rays from the waves' troughs below its plane pass its back.
    const std::uint32_t flags = (kind & (GI_INSTANCE_TERRAIN | GI_INSTANCE_SEA)) ? 0 : D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
    // The rays towards lights skip what casts no shadow (blas.castsNothing) whole; a search of the terrain alone takes the
    // terrain's rings and patches by their own bit.
    const std::uint32_t mask = (blas.castsNothing ? GI_MASK_VIEW_ONLY : GI_MASK_CASTS)
        | ((kind & GI_INSTANCE_TERRAIN) ? GI_MASK_TERRAIN : 0);
    desc.idAndMask = (kind & 0xFFFFFFu) | (mask << 24);
    desc.hitGroupAndFlags = (blas.firstGeometry & 0xFFFFFFu) | (flags << 24);
    const auto blasAddress = address(blas);
    desc.blas = {static_cast<std::uint32_t>(blasAddress), static_cast<std::uint32_t>(blasAddress >> 32)};
    instances.push_back(desc);
    return static_cast<std::uint32_t>(instances.size() - 1);
}

std::optional<std::uint32_t> Scene::openTexture(const SceneTexture& texture)
{
    if (freeTextures.empty()) return std::nullopt;

    const auto textureSlot = freeTextures.back();
    auto& opened = textures[textureSlot];
    check(opened.open(device, texture.texture), "Opening one of the game's textures");
    freeTextures.pop_back();
    const auto desc = opened.resource->GetDesc();
    writeTexture(textureSlot, opened.resource.Get(), texture.viewFormat, texture.mips);

    return textureSlot;
}

void Scene::closeTexture(std::uint32_t textureSlot)
{
    closedTextures.push_back({signalValue, textureSlot, std::move(textures[textureSlot].resource)});
    textures[textureSlot] = {};
}

Scene::Stats Scene::stats() const
{
    auto result = tally;
    result.structures = result.compacted = 0;
    for (const auto& blas : structures)
        if (blas.live)
        {
            ++result.structures;
            if (blas.compacted) ++result.compacted;
            if (!blas.micromapKeys.empty()) ++result.micromapStructures;
        }

    result.micromapMeshes = static_cast<std::uint32_t>(micromaps.size());
    for (const auto& [key, micromap] : micromaps) result.microTriangles += micromap.microTriangles;
    result.micromapUsed = micromapAllocator.usedSize();
    result.micromapCapacity = micromapAllocator.capacity();

    result.poolUsed = poolAllocator.usedSize();
    result.poolCapacity = poolAllocator.capacity();
    result.blasUsed = blasAllocator.usedSize();
    result.blasCapacity = blasAllocator.capacity();
    result.geometriesUsed = static_cast<std::uint32_t>(rowAllocator.usedSize());
    return result;
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Scene::inputsOf(const Blas& blas, bool update) const
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = blas.use == Use::Static ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION
                                           : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    if (update) inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    inputs.NumDescs = static_cast<UINT>(blas.geometries.size());
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = blas.geometries.data();
    return inputs;
}

void Scene::releaseMicromaps(Blas& blas)
{
    for (const auto key : blas.micromapKeys)
    {
        const auto micromap = micromaps.find(key);
        if (--micromap->second.users) continue;

        releases.push_back({signalValue, Pool::Micromaps, micromap->second.arrayOffset, micromap->second.arrayBytes});
        releases.push_back({signalValue, Pool::Geometry, micromap->second.indexOffset, micromap->second.indexBytes});
        micromaps.erase(micromap);
    }

    blas.micromapKeys.clear();
}

void Scene::linkMicromaps(Blas& blas)
{
    for (const auto i : blas.micromapped)
    {
        const auto& row = rows[blas.firstGeometry + i];
        if (!(row.flags & GI_GEOMETRY_ALPHA_TEXTURE) || !row.texture) return;
    }

    // By its texture too: a model's variants share their triangles, not always what covers them.
    const auto keyOf = [&](std::uint32_t i) {
        return blas.geometries[i].Triangles.IndexBuffer | (std::uint64_t{rows[blas.firstGeometry + i].texture} << MicromapKeyTextureShift);
    };

    // Held as they are baked, for a structure that waits for room in the frames' bakes.
    for (const auto i : blas.micromapped)
    {
        const auto key = keyOf(i);
        if (std::ranges::find(blas.micromapKeys, key) != blas.micromapKeys.end()) continue;

        auto micromap = micromaps.find(key);
        if (micromap == micromaps.end())
        {
            const auto planned = planMicromap(blas, i);
            if (!planned) return;

            micromap = micromaps.emplace(key, *planned).first;
        }

        ++micromap->second.users;
        blas.micromapKeys.push_back(key);
    }

    for (const auto i : blas.micromapped)
    {
        const auto& micromap = micromaps.at(keyOf(i));
        auto& geometry = blas.geometries[i];
        blas.micromapTriangles[i] = geometry.Triangles;
        blas.links[i] = {{geometryPool->GetGPUVirtualAddress() + micromap.indexOffset, 4}, DXGI_FORMAT_R32_UINT, 0,
            micromapPool->GetGPUVirtualAddress() + micromap.arrayOffset};
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES;
        geometry.OmmTriangles = {&blas.micromapTriangles[i], &blas.links[i]};
    }

    blas.mayHaveMicromaps = false;
}

std::optional<Scene::Micromap> Scene::planMicromap(const Blas& blas, std::uint32_t geometry)
{
    const auto count = blas.geometries[geometry].Triangles.IndexCount / 3;
    const auto level = micromapLevelOf(count);
    const auto microTriangles = std::uint64_t{count} << (2 * level);
    const auto dataBytes = aligned(microTriangles / 4, 128);
    const auto descBytes = aligned(count * sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC), 128);
    const auto indexBytes = aligned(std::uint64_t{count} * 4, 128);
    const auto needed = descBytes + indexBytes + dataBytes;
    if (bakeUsed + needed > BakeBytes || (bakeUsed && frameMicroTriangles + microTriangles > MicroTrianglesPerFrame)) return std::nullopt;

    MicromapBuild build;
    build.histogram.push_back({count, level, D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE});
    D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC array{};
    array.NumOmmHistogramEntries = 1;
    array.pOmmHistogram = build.histogram.data();
    array.InputBuffer = bake->GetGPUVirtualAddress();
    array.PerOmmDescs = {bake->GetGPUVirtualAddress(), sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC)};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = 1;
    inputs.pOpacityMicromapArrayDesc = &array;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
    const auto scratchBytes = aligned(prebuild.ScratchDataSizeInBytes, 256);
    if (micromapScratchUsed + scratchBytes > MicromapScratchBytes) return std::nullopt;

    const auto destination = micromapAllocator.allocate(prebuild.ResultDataMaxSizeInBytes, MicromapArrayAlignment);
    const auto indices = poolAllocator.allocate(indexBytes, 16);
    if (!destination || !indices)
    {
        if (destination) micromapAllocator.release(*destination, prebuild.ResultDataMaxSizeInBytes);
        if (indices) poolAllocator.release(*indices, indexBytes);
        ++tally.micromapsRefused;
        return std::nullopt;
    }

    const auto descOffset = bakeUsed, indexOffset = descOffset + descBytes, dataOffset = indexOffset + indexBytes;
    jobs.push_back({rows[blas.firstGeometry + geometry], jobGroups, level, static_cast<std::uint32_t>(dataOffset), static_cast<std::uint32_t>(descOffset),
        static_cast<std::uint32_t>(indexOffset), {}});
    build.descOffset = descOffset;
    build.destination = *destination;
    build.scratch = micromapScratchUsed;
    build.indexFrom = indexOffset;
    build.indexTo = *indices;
    build.indexBytes = std::uint64_t{count} * 4;
    micromapBuilds.push_back(std::move(build));
    jobGroups += count;
    bakeUsed += needed;
    frameMicroTriangles += microTriangles;
    micromapScratchUsed += scratchBytes;
    return Micromap{*destination, prebuild.ResultDataMaxSizeInBytes, *indices, indexBytes, microTriangles};
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::address(const Blas& blas) const
{
    return blasPool->GetGPUVirtualAddress() + blas.offset;
}

void Scene::writeRow(std::uint32_t row, const GIGeometry& geometry)
{
    rows[row] = geometry;
    dirtyRows.push_back(row);
}

// A descriptor of the scene's texture range: the texture's levels of detail, or a null view.
void Scene::writeTexture(std::uint32_t textureSlot, ID3D12Resource* texture, DXGI_FORMAT format, std::uint32_t mips)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = format;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = mips;
    auto at = textureDescriptors;
    at.ptr += std::size_t{textureSlot} * descriptorSize;
    device->CreateShaderResourceView(texture, &desc, at);
}

// Room in the slot's upload buffer, aligned for any copy; UploadBytes when there is none.
std::uint64_t Scene::reserveUpload(std::uint64_t bytes)
{
    const auto at = aligned(uploadUsed, 16);
    if (at + bytes > UploadBytes) return UploadBytes;

    uploadUsed = at + bytes;
    return at;
}
}
