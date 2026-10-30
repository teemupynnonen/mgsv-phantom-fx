#pragma once

#include "d3d12_helpers.hpp"
#include "dev_tools.hpp"
#include "log.hpp"
#include "performance.hpp"

#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

// Direct3D 12 helpers shared by the global illumination's renderer (gi_renderer.cpp), its light's
// passes (gi_light.cpp), the sun's ray-traced shadows (gi_shadow.cpp) and path tracing's rays
// (path_tracing_rays.cpp): one root signature, one descriptor table pair per frame, and the kernels of
// src/shaders.
namespace phantom_fx::gi_d3d12
{
using Microsoft::WRL::ComPtr;

// Every kernel, in the order of their DXIL in gi_renderer.cpp.
enum class Kernel : std::uint8_t
{
    // gi_scene.hlsl
    ClearNewGeometries, DebugScene, TerrainGrid, Skin,
    // gi_light.hlsl
    ClearCounters, GenerateDispatch, PrepareGuides, TraceLight, LearnAlbedo, PurgeTiles, PopulateMultibounceCells, PopulateCells, UpdateTiles,
    UpdateMultibounceCells, ResolveLight, UpdateFoliageMemory, CompositeLight,
    // gi_shadow.hlsl, gi_shadow_filter.hlsl (the sun's), light_shadows.hlsl (the lights')
    TraceSunShadows, ClassifyShadowTiles, FilterShadows, DebugShadows, ProbeShadows, TraceLocalLightShadows, FilterLocalLightShadows, ProbeLocalLights,
    // reflections.hlsl and direct_light.hlsl (path tracing)
    TraceReflections, TraceLampLight,
#if PHANTOM_FX_DEV_TOOLS
    InspectPixels, // gi_shadow.hlsl: a debug case's record
#endif
    Count
};

using Pipelines = std::array<ComPtr<ID3D12PipelineState>, static_cast<std::size_t>(Kernel::Count)>;

// The descriptor tables (gi_common.hlsli, gi_light.hlsl, gi_shadow.hlsli).
constexpr UINT SrvCount = 32, UavCount = 112, TableSize = SrvCount + UavCount;

// The GI files call these as gi_d3d12::check and so on (d3d12_helpers.hpp, hresult.hpp).
using phantom_fx::check;
using phantom_fx::transition;
using phantom_fx::uavBarrier;

inline ComPtr<ID3D12Resource> createBuffer(ID3D12Device* device, std::uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES initial,
    D3D12_RESOURCE_FLAGS flags, const char* what)
{
    D3D12_HEAP_PROPERTIES properties{heap};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<std::uint64_t>(size, 4);
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> buffer;
    check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr, IID_PPV_ARGS(&buffer)), what);

    return buffer;
}

// A buffer kernels write, zeroed as committed resources are, in the unordered access state.
inline ComPtr<ID3D12Resource> createStorage(ID3D12Device* device, std::uint64_t size, const char* what)
{
    return createBuffer(device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, what);
}

inline ComPtr<ID3D12Resource> createTexture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format, UINT16 mips,
    D3D12_RESOURCE_STATES initial, const char* what)
{
    D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = mips;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> texture;
    check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr, IID_PPV_ARGS(&texture)), what);

    return texture;
}

// One frame's tables in the shader-visible heap; every slot starts as a null view.
class Descriptors
{
public:
    Descriptors(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT size, std::uint32_t frameSlot) : device(device), size(size)
    {
        cpu = heap->GetCPUDescriptorHandleForHeapStart();
        gpu = heap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += std::size_t{frameSlot} * TableSize * size;
        gpu.ptr += std::uint64_t{frameSlot} * TableSize * size;

        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
        nullSrv.Format = DXGI_FORMAT_R32_FLOAT;
        nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullSrv.Texture2D.MipLevels = 1;
        for (UINT i = 0; i < SrvCount; ++i) device->CreateShaderResourceView(nullptr, &nullSrv, at(i));

        D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav{};
        nullUav.Format = DXGI_FORMAT_R32_FLOAT;
        nullUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        for (UINT i = 0; i < UavCount; ++i) device->CreateUnorderedAccessView(nullptr, nullptr, &nullUav, at(SrvCount + i));
    }

    void texture(UINT slot, ID3D12Resource* resource, DXGI_FORMAT format, UINT mips = 1)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = mips;
        device->CreateShaderResourceView(resource, &desc, at(slot));
    }

    void buffer(UINT slot, ID3D12Resource* resource, std::uint64_t offset, std::uint64_t stride, std::uint64_t count)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Buffer.FirstElement = offset / stride;
        desc.Buffer.NumElements = static_cast<UINT>(std::max<std::uint64_t>(count, 1));
        desc.Buffer.StructureByteStride = static_cast<UINT>(stride);
        device->CreateShaderResourceView(resource, &desc, at(slot));
    }

    // A buffer as a ByteAddressBuffer.
    void rawBuffer(UINT slot, ID3D12Resource* resource, std::uint64_t bytes)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Buffer.NumElements = static_cast<UINT>(bytes / 4);
        desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(resource, &desc, at(slot));
    }

    void outputTexture(UINT slot, ID3D12Resource* resource, DXGI_FORMAT format, UINT mip = 0)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = mip;
        device->CreateUnorderedAccessView(resource, nullptr, &desc, at(SrvCount + slot));
    }

    void outputBuffer(UINT slot, ID3D12Resource* resource, std::uint64_t stride, std::uint64_t count)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = static_cast<UINT>(std::max<std::uint64_t>(count, 1));
        desc.Buffer.StructureByteStride = static_cast<UINT>(stride);
        device->CreateUnorderedAccessView(resource, nullptr, &desc, at(SrvCount + slot));
    }

    // A buffer as a RWByteAddressBuffer.
    void outputRawBuffer(UINT slot, ID3D12Resource* resource, std::uint64_t bytes)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = static_cast<UINT>(bytes / 4);
        desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(resource, nullptr, &desc, at(SrvCount + slot));
    }

    D3D12_GPU_DESCRIPTOR_HANDLE srvTable() const { return gpu; }
    D3D12_GPU_DESCRIPTOR_HANDLE uavTable() const { return {gpu.ptr + std::uint64_t{SrvCount} * size}; }

private:
    D3D12_CPU_DESCRIPTOR_HANDLE at(UINT index) const { return {cpu.ptr + std::size_t{index} * size}; }

    ID3D12Device* device;
    UINT size;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
};

// Records kernels on a list whose root signature and tables are bound.
class Recorder
{
public:
    Recorder(ID3D12GraphicsCommandList4& list, const Pipelines& pipelines, ID3D12CommandSignature* signature, ID3D12Resource* arguments)
        : list(list), pipelines(pipelines), signature(signature), arguments(arguments)
    {
    }

    void dispatch(Kernel kernel, std::uint32_t x, std::uint32_t y = 1, std::array<std::uint32_t, 4> parameters = {})
    {
        if (!x || !y) return;

        bind(kernel, parameters);
        list.Dispatch(x, y, 1);
    }

    // A dispatch whose group count the GPU works out from its counter: groups of `perGroup`
    // threads (or tiles) over counter `counter`, plus `second` when given.
    void indirect(Kernel kernel, std::uint32_t counter, std::uint32_t perGroup, std::array<std::uint32_t, 4> parameters = {},
        std::uint32_t second = 0xFFFFFFFFu)
    {
        const auto slot = nextSlot++ % 16;
        dispatch(Kernel::GenerateDispatch, 1, 1, {counter, perGroup, slot, second});
        D3D12_RESOURCE_BARRIER ready[]{uavBarrier(), gi_d3d12::transition(arguments, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)};
        list.ResourceBarrier(static_cast<UINT>(std::size(ready)), ready);
        bind(kernel, parameters);
        list.ExecuteIndirect(signature, 1, arguments, std::uint64_t{slot} * 16, nullptr, 0);
        D3D12_RESOURCE_BARRIER done[]{gi_d3d12::transition(arguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS), uavBarrier()};
        list.ResourceBarrier(static_cast<UINT>(std::size(done)), done);
    }

    // Every unordered access before is done before any after.
    void barrier()
    {
        const auto all = uavBarrier();
        list.ResourceBarrier(1, &all);
    }

    void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        const auto barrier = gi_d3d12::transition(resource, before, after);
        list.ResourceBarrier(1, &barrier);
    }

    ID3D12GraphicsCommandList4& commands() { return list; }

    // Ends the phase timed before and starts timing `phase`, for the log and, under `pass`, the
    // settings window's costs; null ends the last.
    void mark(const char* phase, std::optional<performance::Pass> pass = std::nullopt)
    {
        if (marker) marker(phase, pass);
    }

    std::function<void(const char*, std::optional<performance::Pass>)> marker;

    // Binds the frame's descriptor heap, root signature and tables again, after a pass with its own
    // (the light's denoiser, gi_denoiser.hpp).
    void rebind()
    {
        if (rebinder) rebinder();
    }

    std::function<void()> rebinder;

private:
    void bind(Kernel kernel, const std::array<std::uint32_t, 4>& parameters)
    {
        list.SetPipelineState(pipelines[static_cast<std::size_t>(kernel)].Get());
        list.SetComputeRoot32BitConstants(1, 4, parameters.data(), 0);
    }

    ID3D12GraphicsCommandList4& list;
    const Pipelines& pipelines;
    ID3D12CommandSignature* signature;
    ID3D12Resource* arguments;
    std::uint32_t nextSlot{};
};

// ---- Rounding ----

// Thread groups of `size` that cover `threads`.
inline std::uint32_t groups(std::uint64_t threads, std::uint32_t size)
{
    return static_cast<std::uint32_t>((threads + size - 1) / size);
}

inline std::uint64_t aligned(std::uint64_t value, std::uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}
}
