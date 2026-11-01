// The ray-traced scene on the Direct3D 12 side; gi_scene.hpp describes it.
#include "passes/ray_tracing/gi_scene.hpp"

#include <algorithm>
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
}

Scene::Scene(ID3D12Device5* device, D3D12_CPU_DESCRIPTOR_HANDLE textures, UINT descriptorSize)
    : device(device), blasAllocator(BlasPoolBytes), textureDescriptors(textures), descriptorSize(descriptorSize)
{
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
    for (const auto id : buildQueue)
    {
        auto& blas = structures[id];
        if (!blas.live || !blas.queued) continue;

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

void Scene::recordBottomLevels(ID3D12GraphicsCommandList4& list)
{
    D3D12_RESOURCE_BARRIER readable[]{uavBarrier(), transition(geometryPool.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    list.ResourceBarrier(static_cast<UINT>(std::size(readable)), readable);

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

std::optional<BlasId> Scene::createBlas(Use use, const std::vector<Geometry>& geometries)
{
    if (geometries.empty()) return std::nullopt;

    Blas blas;
    blas.use = use;
    const auto poolAddress = geometryPool->GetGPUVirtualAddress();
    for (const auto& geometry : geometries)
    {
        D3D12_RAYTRACING_GEOMETRY_DESC desc{};
        desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        // Hair dims a ray once for each layer it crosses (PassHair), so a ray must meet each of its triangles once.
        if (!(geometry.flags & GI_GEOMETRY_CUT_OUT)) desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        else if (geometry.flags & GI_GEOMETRY_HAIR) desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;
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
    }

    auto inputs = inputsOf(blas, false);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

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
        GIGeometry row{static_cast<std::uint32_t>(geometry.vertexOffset), geometry.indexCount ? static_cast<std::uint32_t>(geometry.indexOffset) : GI_NO_INDICES,
            geometry.flags, geometry.texture, geometry.alphaThreshold, geometry.specularTexture, geometry.normalTexture, geometry.materialIndex,
            {geometry.emission[0], geometry.emission[1], geometry.emission[2], 0}, static_cast<std::uint32_t>(geometry.normalOffset), {}};
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
    row.flags = (row.flags & GI_GEOMETRY_CUT_OUT) | (material.flags & ~GI_GEOMETRY_CUT_OUT);
    row.texture = material.texture;
    row.alphaThreshold = material.alphaThreshold;
    row.specularTexture = material.specularTexture;
    row.normalTexture = material.normalTexture;
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
    desc.idAndMask = (kind & 0xFFFFFFu) | (GI_MASK_SCENE << 24);
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
        }

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
