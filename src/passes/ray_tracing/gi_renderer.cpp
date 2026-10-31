// The global illumination's Direct3D 12 side; gi_renderer.hpp describes the route.
#include "passes/ray_tracing/gi_renderer.hpp"

#include "camera.hpp"
#include "clock.hpp"
#include "features/global_illumination/gi_light.hpp"
#include "features/path_tracing/path_tracing_rays.hpp"
#include "features/ray_traced_shadows/gi_shadow.hpp"
#include "frame_log.hpp"
#include "log.hpp"
#include "passes/ray_tracing/gi_d3d12.hpp"
#include "passes/ray_tracing/gi_scene.hpp"
#include "passes/ray_tracing/scene_contents.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"
#include "passes/ray_tracing/trace.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

// The kernels, compiled by DXC at build time (CMakeLists.txt).
#include "ClassifyShadowTiles.h"
#include "ClearCounters.h"
#include "ClearNewGeometries.h"
#include "CompositeLight.h"
#include "DebugScene.h"
#include "DebugShadows.h"
#include "FilterLocalLightShadows.h"
#include "FilterShadows.h"
#include "GenerateDispatch.h"
#include "LearnAlbedo.h"
#include "PopulateCells.h"
#include "PopulateMultibounceCells.h"
#include "PrepareGuides.h"
#include "ProbeLocalLights.h"
#include "ProbeShadows.h"
#include "PurgeTiles.h"
#include "ResolveLight.h"
#include "Skin.h"
#include "TerrainGrid.h"
#include "TraceLampLight.h"
#include "TraceLight.h"
#include "TraceLocalLightShadows.h"
#include "TraceReflections.h"
#include "TraceSunShadows.h"
#include "TraceSurfaceReflections.h"
#include "UpdateFoliageMemory.h"
#include "UpdateMultibounceCells.h"
#include "UpdateTiles.h"

namespace phantom_fx::gi_renderer
{
namespace
{
using namespace phantom_fx::gi;
using namespace phantom_fx::gi_d3d12;
using gi_scene::FramesInFlight;
using gi_scene::MaxGeometries;
using gi_scene::MaxInstances;
using scene_contents::InstancedModel;
using scene_contents::instancedAt;

static_assert(MaxInstances <= (1u << GI_HIT_INSTANCE_BITS));

// The renderer's own upload buffer holds the frame's constants, its new geometries' rows, and its
// characters' skin records and palettes.
constexpr std::uint64_t UploadBytes = 8ull << 20;
// Where no albedo is known yet: MGSV's materials are dark.
constexpr float DefaultAlbedo = 0.2f;
// The sun's ray-traced shadows: how much of the sun cut-out geometry without its texture (not copied
// yet, or a mesh without texture coordinates) blocks beyond the game's shadow map, and how far the rays
// look for what blocks the sun.
constexpr float AlphaTestedBlocking = 0.5f, ShadowRayLength = 5000;
// Diagnostics: the most phases a frame's list times.
constexpr std::uint32_t MaxMarks = 32;

// The kernels' DXIL in gi_d3d12::Kernel's order.
constexpr std::span<const unsigned char> Bytecode[]{GI_ClearNewGeometries, GI_DebugScene, GI_TerrainGrid, GI_Skin,
    GI_ClearCounters, GI_GenerateDispatch, GI_PrepareGuides, GI_TraceLight, GI_LearnAlbedo, GI_PurgeTiles, GI_PopulateMultibounceCells,
    GI_PopulateCells, GI_UpdateTiles, GI_UpdateMultibounceCells, GI_ResolveLight, GI_UpdateFoliageMemory, GI_CompositeLight, GI_TraceSunShadows, GI_ClassifyShadowTiles,
    GI_FilterShadows, GI_DebugShadows, GI_ProbeShadows, GI_TraceLocalLightShadows, GI_FilterLocalLightShadows, GI_ProbeLocalLights, GI_TraceReflections, GI_TraceSurfaceReflections, GI_TraceLampLight};
static_assert(std::size(Bytecode) == static_cast<std::size_t>(Kernel::Count));

using Shared = OpenedTexture;

// Diagnostics: the steps of a frame's CPU time.
enum CpuStep { WaitStep, ProbeStep, ContentsStep, RecordStep, CpuSteps };
constexpr const char* CpuStepNames[CpuSteps]{"waiting for the GPU", "the probe", "the scene's contents", "recording"};

// What a frame's lists bind.
struct FrameBinding
{
    D3D12_GPU_VIRTUAL_ADDRESS constants{}, topLevel{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvs{}, uavs{}, textures{};
};

struct State
{
    ComPtr<ID3D12Device> wrapper;
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    std::array<ComPtr<ID3D12CommandAllocator>, FramesInFlight> allocators;
    std::array<std::uint64_t, FramesInFlight> submitted{};
    ComPtr<ID3D12GraphicsCommandList4> list;
    // Glass and mirrors' list (traceSurfaces), after the frame's, in the same slot.
    std::array<ComPtr<ID3D12CommandAllocator>, FramesInFlight> surfaceAllocators;
    ComPtr<ID3D12GraphicsCommandList4> surfaceList;
    // What that list binds and writes, from the last render that traced reflections, until it runs.
    struct SurfaceTrace
    {
        std::uint32_t slot{}, width{}, height{};
        FrameBinding binding;
        ID3D12Resource* reflection{};
    };
    std::optional<SurfaceTrace> surfaceTrace;
    HANDLE event{};
    std::uint64_t frames{};

    ComPtr<ID3D12RootSignature> rootSignature;
    Pipelines pipelines;
    ComPtr<ID3D12CommandSignature> dispatchSignature;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT descriptorSize{};

    std::unique_ptr<gi_scene::Scene> scene;
    std::unique_ptr<scene_contents::SceneContents> contents;
    ComPtr<ID3D12Resource> geometryAlbedo, counters, dispatchArguments;
    std::array<ComPtr<ID3D12Resource>, FramesInFlight> upload;
    std::array<std::uint8_t*, FramesInFlight> uploadData{};
    // Diagnostics: the frame's instances and their triangles.
    std::uint32_t instances{};
    std::uint64_t triangles{};

    std::unique_ptr<gi_light::Light> light;
    std::unique_ptr<gi_shadow::Shadows> shadows;
    // Diagnostics (the shadow debug view and probe): the probe, read back when the frame's slot comes
    // round again, and each frame's instances of the game's models.
    ComPtr<ID3D12Resource> probe;
    std::array<ComPtr<ID3D12Resource>, FramesInFlight> probeReadback;
    std::array<bool, FramesInFlight> probeWritten{};
    std::array<std::vector<InstancedModel>, FramesInFlight> instancedModels; // Each slot's frame's, by instance

    // GPU timestamps between the named phases of each frame's list (Recorder::mark), read when the
    // frame's slot comes round again: into the settings window's costs, by pass. The scene's, the light's
    // and the shadows' times are averaged for the overlay; each phase's are summed for the log, with the
    // frame's builds and CPU time.
    struct Mark
    {
        const char* phase{};
        std::optional<performance::Pass> pass;
    };
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> timestampReadback;
    std::uint64_t timestampFrequency{};
    std::array<std::vector<Mark>, FramesInFlight> marks;
    double sceneMilliseconds{}, lightMilliseconds{}, shadowMilliseconds{};
    struct Tally
    {
        std::vector<std::pair<const char*, double>> phases; // Milliseconds, in the order they first ran
        std::uint32_t timedFrames{}, frames{}, builds{}, mostBuilds{}, waiting{}, compactions{};
        std::uint64_t builtTriangles{};
        double cpuMilliseconds{}, mostCpuMilliseconds{};
        std::array<double, CpuSteps> cpuSteps{};
        std::uint64_t since{};
    } tally;

    // Diagnostics: a copy of the output every ten seconds, read once the GPU is past it.
    ComPtr<ID3D12Resource> statsReadback, statsCounters;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT statsFootprint{};
    std::uint64_t statsFence{}, lastStats{};
    Shared depth, normal, albedo, output, shadow, shadowDebug, velocity, terrainPages, terrainHeights, lightDebug, materialSpecular, skyCube, guideNormals,
        guideSpecular, reflection, reflectionDistance, reflectionMap, guideMaterial, guideLobe, specularTable, lampMasks, lampDiffuse, lampSpecular, surfaceDepth,
        surfaceNormal, surfaceReflection;

    ~State()
    {
        if (event) CloseHandle(event);
    }
};
std::unique_ptr<State> state;

// The latest shadow probe as text, for the overlay (another thread) and the log; kept apart from
// the state so stopping does not race the overlay.
std::mutex probeMutex;
std::string probeText, lastProbeKey;
std::uint64_t lastProbeLog{};

ComPtr<ID3D12Resource> openShared(State& s, Shared& shared, ID3D11Texture2D* texture, const char* what)
{
    check(shared.open(s.device.Get(), texture), what);
    return shared.resource;
}

void createPipelines(State& s)
{
    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = SrvCount;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = UavCount;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = GI_MAX_TEXTURES;
    ranges[2].RegisterSpace = 2;

    D3D12_ROOT_PARAMETER parameters[6]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; // b0: the frame's constants
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; // b1: the dispatch's
    parameters[1].Constants.ShaderRegister = 1;
    parameters[1].Constants.Num32BitValues = 4;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &ranges[0];
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[3].DescriptorTable.NumDescriptorRanges = 1;
    parameters[3].DescriptorTable.pDescriptorRanges = &ranges[1];
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; // t0, space1: the scene
    parameters[4].Descriptor.RegisterSpace = 1;
    parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; // t0 on, space2: the game's textures
    parameters[5].DescriptorTable.NumDescriptorRanges = 1;
    parameters[5].DescriptorTable.pDescriptorRanges = &ranges[2];

    // Point and linear clamped, linear wrapped (s2, for the models' texture coordinates).
    D3D12_STATIC_SAMPLER_DESC samplers[3]{};
    for (UINT i = 0; i < 3; ++i)
    {
        samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = i == 2 ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
    }

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(std::size(parameters));
    desc.pParameters = parameters;
    desc.NumStaticSamplers = static_cast<UINT>(std::size(samplers));
    desc.pStaticSamplers = samplers;
    ComPtr<ID3DBlob> blob, errors;
    const auto result = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result))
        throw std::runtime_error("Serializing the root signature failed: "
            + (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : hex(static_cast<std::uint32_t>(result))));

    check(s.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&s.rootSignature)), "Creating the root signature");

    for (std::size_t kernel = 0; kernel < s.pipelines.size(); ++kernel)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = s.rootSignature.Get();
        pipeline.CS = {Bytecode[kernel].data(), Bytecode[kernel].size()};
        check(s.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&s.pipelines[kernel])), "Creating a kernel");
    }

    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature{};
    signature.ByteStride = 16;
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &argument;
    check(s.device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&s.dispatchSignature)), "Creating the dispatch signature");
}

void createResources(State& s)
{
    auto* device = s.device.Get();
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = TableSize * FramesInFlight + GI_MAX_TEXTURES;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&s.heap)), "Creating the descriptor heap");
    s.descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // The game's textures' descriptors follow the frames' tables.
    auto textureRange = s.heap->GetCPUDescriptorHandleForHeapStart();
    textureRange.ptr += std::size_t{TableSize} * FramesInFlight * s.descriptorSize;
    s.scene = std::make_unique<gi_scene::Scene>(device, textureRange, s.descriptorSize);
    s.contents = std::make_unique<scene_contents::SceneContents>(*s.scene);
    s.geometryAlbedo = createStorage(device, std::uint64_t{MaxGeometries} * 16, "Creating the geometry albedo");
    s.counters = createStorage(device, GI_COUNTER_COUNT * 4, "Creating the counters");
    s.probe = createStorage(device, sizeof(GIShadowProbe), "Creating the shadow probe");
    for (auto& readback : s.probeReadback)
        readback = createBuffer(device, sizeof(GIShadowProbe), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
            "Creating the shadow probe's readback");
    s.dispatchArguments = createStorage(device, GI_DISPATCH_SLOTS * 16, "Creating the dispatch arguments");

    for (std::uint32_t slot = 0; slot < FramesInFlight; ++slot)
    {
        s.upload[slot] = createBuffer(device, UploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
            "Creating an upload buffer");
        void* data{};
        check(s.upload[slot]->Map(0, nullptr, &data), "Mapping an upload buffer");
        s.uploadData[slot] = static_cast<std::uint8_t*>(data);
    }

    D3D12_QUERY_HEAP_DESC queries{};
    queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queries.Count = MaxMarks * FramesInFlight;
    check(device->CreateQueryHeap(&queries, IID_PPV_ARGS(&s.timestamps)), "Creating the timestamps");
    s.timestampReadback = createBuffer(device, std::uint64_t{queries.Count} * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_NONE, "Creating the timestamp readback");
    check(s.queue->GetTimestampFrequency(&s.timestampFrequency), "Reading the timestamp frequency");
}

void waitFor(State& s, std::uint64_t value)
{
    if (!value || s.fence->GetCompletedValue() >= value) return;

    check(s.fence->SetEventOnCompletion(value, s.event), "Waiting for Direct3D 12");
    if (WaitForSingleObject(s.event, FenceTimeout) != WAIT_OBJECT_0) throw std::runtime_error("The global illumination's GPU work did not finish within 10 seconds.");
}

struct Uploads
{
    std::uint8_t* data{};
    D3D12_GPU_VIRTUAL_ADDRESS address{};
    std::uint64_t used{};

    // Room for `count` elements of `stride` bytes, aligned for a constant buffer and a structured view.
    std::uint64_t reserve(std::uint64_t stride, std::uint64_t count)
    {
        used = aligned(aligned(used, 256), stride);
        const auto offset = used;
        used += stride * std::max<std::uint64_t>(count, 1);
        if (used > UploadBytes) throw std::runtime_error("The global illumination's upload buffer is full.");

        return offset;
    }
};

// A camera of the frame in double, its projection without the upscaler's jitter (`x`, `y`: Frame::jitter).
camera::View unjitteredCamera(const Matrix& projection, const Matrix& view, float x, float y)
{
    camera::View result{};
    for (int i = 0; i < 16; ++i)
    {
        result.projection[i] = projection[i];
        result.view[i] = view[i];
    }

    result.projection = camera::unjitteredProjection(result.projection, {x, y});

    return result;
}

// In double (camera::inverse); projection-views are invertible.
Matrix inverse(const Matrix& input)
{
    camera::Matrix matrix{};
    for (int i = 0; i < 16; ++i) matrix[i] = input[i];

    const auto result = camera::inverse(matrix);
    Matrix single{};
    for (int i = 0; i < 16; ++i) single[i] = static_cast<float>(result[i]);

    return single;
}

void copyMatrix(float4x4& to, const Matrix& from)
{
    std::memcpy(to.m, from.data(), sizeof(to.m));
}

GIFrameConstants constantsOf(const Frame& frame, std::uint32_t newGeometries, std::uint32_t instanceCount)
{
    GIFrameConstants constants{};
    copyMatrix(constants.viewProjection, frame.viewProjection);
    copyMatrix(constants.viewProjectionInverse, inverse(frame.viewProjection));
    copyMatrix(constants.previousViewProjection, frame.previousViewProjection);
    constants.eye = {frame.eye[0], frame.eye[1], frame.eye[2], 0};
    constants.sunDirection = {frame.sunDirection[0], frame.sunDirection[1], frame.sunDirection[2], 0};
    constants.sunColor = {frame.sunColor[0], frame.sunColor[1], frame.sunColor[2], 0};
    for (int i = 0; i < 9; ++i) constants.skySH[i] = {frame.skyRadiance[i][0], frame.skyRadiance[i][1], frame.skyRadiance[i][2], 0};
    constants.dimensions = {frame.width, frame.height, frame.frameIndex, instanceCount};
    constants.nearFar = {frame.nearPlane, frame.farPlane, DefaultAlbedo, frame.outputScale};
    constants.counts = {newGeometries, 0, 0, 0};
    constants.shadowRange = {frame.shadowRange[0], frame.shadowRange[1], frame.shadowRange[2], frame.shadowRange[3]};
    constants.shadowParams = {frame.sunTangent, AlphaTestedBlocking, ShadowRayLength, 0};
    constants.rayCone = {2 * std::tan(frame.verticalFov / 2) / static_cast<float>(std::max(frame.height, 1u)), 0, 0, 0};
    constants.jitter = {frame.jitter[0], frame.jitter[1], frame.jitter[2], frame.jitter[3]};

    // The light's motion (CameraMotionOf in gi_light.hlsl), from the projections and views apart.
    const auto current = unjitteredCamera(frame.projection, frame.view, frame.jitter[0], frame.jitter[1]);
    const auto previous = unjitteredCamera(frame.previousProjection, frame.previousView, frame.jitter[2], frame.jitter[3]);
    const auto previousFromCurrent = camera::previousFromCurrentByParts(previous, current);
    for (int i = 0; i < 16; ++i) constants.previousFromCurrent.m[i] = static_cast<float>(previousFromCurrent[i]);

    const auto clipToView = camera::inverse(current.projection);
    constants.clipToViewW = {static_cast<float>(clipToView[12]), static_cast<float>(clipToView[13]), static_cast<float>(clipToView[14]),
        static_cast<float>(clipToView[15])};

    const auto localLights = std::min<std::size_t>(frame.localLights.size(), GI_MAX_LOCAL_LIGHTS);
    for (std::size_t i = 0; i < localLights; ++i)
    {
        const auto& light = frame.localLights[i];
        constants.localLights[i] = {{light.position[0], light.position[1], light.position[2], light.range},
            {light.axis[0], light.axis[1], light.axis[2], light.cosOuter}, {light.clearance, light.penumbraScale, light.coneExponent, light.maxFalloff},
            {light.color[0], light.color[1], light.color[2], light.factor}, {light.sixteenth ? 1.0f : 0.0f, light.addedDistance, light.leastDistance, light.radius},
            {light.shadowStart, 0, 0, 0}, {light.volumeLowest[0], light.volumeLowest[1], light.volumeLowest[2], light.volumeKnown ? 1.0f : 0.0f},
            {light.volumeHighest[0], light.volumeHighest[1], light.volumeHighest[2], 0},
            {light.specularScale, light.diffuseScale, light.size, 0},
            {{light.maskRows[0][0], light.maskRows[0][1], light.maskRows[0][2], light.maskRows[0][3]},
                {light.maskRows[1][0], light.maskRows[1][1], light.maskRows[1][2], light.maskRows[1][3]},
                {light.maskRows[2][0], light.maskRows[2][1], light.maskRows[2][2], light.maskRows[2][3]}},
            {light.maskTile, 0, 0, 0}};
    }
    constants.localLightCount = {static_cast<std::uint32_t>(localLights), frame.localLightRays, 0, 0};

    return constants;
}

// The camera for NRD: column-vector matrices stored by columns, the projections without the upscaler's
// jitter and the jitter apart, the engine's x mirror moved from the projection into the view
// (gi_denoiser::Camera). NRD requires the projections without the jitter: with it in, the denoiser
// followed each frame's jittered samples, and grass and fine detail shimmered (2026-10-25).
gi_denoiser::Camera denoiserCamera(const Frame& frame)
{
    const auto columns = [](const camera::Matrix& rows, bool mirrorColumn0, bool mirrorRow0) {
        std::array<float, 16> stored{};
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
            {
                const bool mirrored = (mirrorColumn0 && column == 0) || (mirrorRow0 && row == 0);
                stored[column * 4 + row] = static_cast<float>(mirrored ? -rows[row * 4 + column] : rows[row * 4 + column]);
            }

        return stored;
    };

    // A light pixel's sample is one of its 2 x 2's render pixels (PickOf in gi_light.hlsl): at its centre on
    // average, moved the jitter back (x right, y down).
    const float width = static_cast<float>(frame.width), height = static_cast<float>(frame.height);
    const auto sampleOffset = [&](float x, float y) { return std::array<float, 2>{-x * width / 4, y * height / 4}; };

    const auto current = unjitteredCamera(frame.projection, frame.view, frame.jitter[0], frame.jitter[1]);
    const auto previous = unjitteredCamera(frame.previousProjection, frame.previousView, frame.jitter[2], frame.jitter[3]);
    gi_denoiser::Camera camera;
    camera.viewToClip = columns(current.projection, true, false);
    camera.previousViewToClip = columns(previous.projection, true, false);
    camera.jitter = sampleOffset(frame.jitter[0], frame.jitter[1]);
    camera.previousJitter = sampleOffset(frame.jitter[2], frame.jitter[3]);
    camera.worldToView = columns(current.view, false, true);
    camera.previousWorldToView = columns(previous.view, false, true);
    camera.restart = frame.cameraCut;
    return camera;
}
}

void start(ID3D11Device* device11, HANDLE sharedFence)
{
    if (state) return;

    auto s = std::make_unique<State>();
    const auto d3d12 = createDevice12(device11);
    s->wrapper = d3d12.wrapper;
    check(d3d12.native.As(&s->device), "Finding Direct3D 12 ray tracing");
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    s->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options));
    if (options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) throw std::runtime_error("The GPU or its driver cannot trace rays from compute shaders (DXR 1.1).");

    const D3D12_COMMAND_QUEUE_DESC queue{D3D12_COMMAND_LIST_TYPE_COMPUTE};
    check(s->device->CreateCommandQueue(&queue, IID_PPV_ARGS(&s->queue)), "Creating the Direct3D 12 queue");

    for (auto& allocator : s->surfaceAllocators)
        check(s->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator)), "Creating a command allocator");

    check(s->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, s->surfaceAllocators[0].Get(), nullptr, IID_PPV_ARGS(&s->surfaceList)),
        "Creating the command list");
    check(s->surfaceList->Close(), "Closing the command list");

    for (auto& allocator : s->allocators)
        check(s->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator)), "Creating a command allocator");

    check(s->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, s->allocators[0].Get(), nullptr, IID_PPV_ARGS(&s->list)),
        "Creating the command list");
    check(s->list->Close(), "Closing the command list");
    check(s->device->OpenSharedHandle(sharedFence, IID_PPV_ARGS(&s->fence)), "Opening the shared fence");
    s->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s->event) throw std::runtime_error("Creating the fence event failed.");

    createPipelines(*s);
    createResources(*s);

    state = std::move(s);
    log("Global illumination started on Direct3D 12.");
}

void stop() noexcept
{
    if (!state) return;

    try
    {
        waitFor(*state, *std::max_element(state->submitted.begin(), state->submitted.end()));
    }
    catch (const std::exception& error)
    {
        log(error.what());
    }

    for (auto* each : trace::inOrder()) each->release();

    state.reset();
    log("Global illumination stopped.");
}

bool started() noexcept
{
    return state != nullptr;
}

// Diagnostics: every ten seconds, a copy of the output and the counters for logStats.
void copyStats(State& s, Recorder& recorder, ID3D12Resource* output, const Frame& frame)
{
    const auto now = GetTickCount64();
    if (s.statsFence || now - s.lastStats <= (frame.logOften ? 0u : 10'000u)) return;

    s.lastStats = now;
    const auto desc = output->GetDesc();
    UINT64 bytes{};
    s.device->GetCopyableFootprints(&desc, 0, 1, 0, &s.statsFootprint, nullptr, nullptr, &bytes);
    if (!s.statsReadback || s.statsReadback->GetDesc().Width < bytes)
        s.statsReadback = createBuffer(s.device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_FLAG_NONE, "Creating the light's readback");
    if (!s.statsCounters)
        s.statsCounters = createBuffer(s.device.Get(), GI_COUNTER_COUNT * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_FLAG_NONE, "Creating the counters' readback");

    recorder.transition(s.counters.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    s.list->CopyBufferRegion(s.statsCounters.Get(), 0, s.counters.Get(), 0, GI_COUNTER_COUNT * 4);
    recorder.transition(s.counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    recorder.transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{output, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    from.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION to{s.statsReadback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    to.PlacedFootprint = s.statsFootprint;
    s.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    recorder.transition(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    s.statsFence = frame.signalValue;
}

// Diagnostics: the output light's level, logged once its copy is back.
void logStats(State& s)
{
    if (!s.statsFence || s.fence->GetCompletedValue() < s.statsFence) return;

    s.statsFence = 0;
    void* data{};
    if (FAILED(s.statsReadback->Map(0, nullptr, &data))) return;

    const auto& footprint = s.statsFootprint.Footprint;
    std::vector<float> luminance;
    double sum[3]{};
    for (UINT y = 0; y < footprint.Height; y += 8)
    {
        const auto* row = reinterpret_cast<const std::uint16_t*>(static_cast<const std::uint8_t*>(data) + std::uint64_t{y} * footprint.RowPitch);
        for (UINT x = 0; x < footprint.Width; x += 8)
        {
            float rgb[3]{};
            for (int c = 0; c < 3; ++c)
            {
                // Half to float.
                const std::uint32_t h = row[x * 4 + c];
                const std::uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
                float value = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                    : exponent == 31 ? 65504.0f : std::ldexp(static_cast<float>(mantissa | 1024), static_cast<int>(exponent) - 25);
                rgb[c] = sign ? -value : value;
                sum[c] += rgb[c];
            }

            luminance.push_back(0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2]);
        }
    }

    s.statsReadback->Unmap(0, nullptr);
    if (luminance.empty()) return;

    // On the game's render thread: one partial sort a share, not a whole sort.
    const auto at = [&](double share) {
        const auto nth = luminance.begin() + static_cast<std::ptrdiff_t>(share * static_cast<double>(luminance.size() - 1));
        std::nth_element(luminance.begin(), nth, luminance.end());
        return std::to_string(*nth);
    };
    const auto count = static_cast<double>(luminance.size());
    log("Global illumination's light (luminance): 1% " + at(0.01) + ", 10% " + at(0.1) + ", median " + at(0.5) + ", 90% " + at(0.9) + ", 99% "
        + at(0.99) + ", max " + at(1.0) + "; mean rgb " + std::to_string(sum[0] / count) + " " + std::to_string(sum[1] / count) + " "
        + std::to_string(sum[2] / count) + ".");

    void* mapped{};
    if (FAILED(s.statsCounters->Map(0, nullptr, &mapped))) return;

    std::uint32_t counters[GI_COUNTER_COUNT]{};
    std::memcpy(counters, mapped, sizeof(counters));
    s.statsCounters->Unmap(0, nullptr);
    const auto value = [&](std::uint32_t counter) { return std::to_string(counters[counter]); };
    log("Global illumination's rays: " + value(GI_COUNTER_HITS) + " hits (" + value(GI_COUNTER_NEAR_HITS) + " within 25 cm), "
        + value(GI_COUNTER_BOUNCE_HITS) + " bounces that hit (" + value(GI_COUNTER_VISIBILITY0) + " into the cache, " + value(GI_COUNTER_FULL_BUCKETS)
        + " without room in it), " + value(GI_COUNTER_VISIBILITY1) + " multibounce rays kept (" + value(GI_COUNTER_MULTIBOUNCE_AWAY)
        + " of those traced reached the sky), " + value(GI_COUNTER_LIGHTS) + " cache hits lit, " + value(GI_COUNTER_COPIES)
        + " copies of a surface skipped, " + value(GI_COUNTER_UPDATE_TILES) + " cache tiles updated, " + value(GI_COUNTER_FOLIAGE)
        + " light pixels on foliage (" + value(GI_COUNTER_FOLIAGE_MEMORY_FULL) + " without room in the foliage memory).");

    constexpr const char* MaximumNames[GI_COUNTER_MAXIMA]{"a hit's albedo", "a hit's direct light", "a cell's direct light", "a cell's indirect light",
        "a multibounce", "a ray's light", "a denoised light pixel", "a render pixel"};
    std::string maxima;
    for (std::uint32_t i = 0; i < GI_COUNTER_MAXIMA; ++i)
        maxima += std::string(i ? ", " : "") + MaximumNames[i] + " " + std::to_string(std::bit_cast<float>(counters[GI_COUNTER_MAXIMA_FIRST + i]));
    log("Global illumination's largest values in that frame (before the exposure; the lights line gives the sun's and sky's): " + maxima + ".");
}

// The phases a frame slot timed, once the GPU is past them: into the averages and the tally.
void readTimes(State& s, std::uint32_t slot)
{
    auto& marks = s.marks[slot];
    if (marks.size() < 2 || !s.timestampFrequency) return;

    void* data{};
    const D3D12_RANGE range{std::size_t{slot} * MaxMarks * 8, std::size_t{slot + 1} * MaxMarks * 8};
    if (FAILED(s.timestampReadback->Map(0, &range, &data))) return;

    const auto* ticks = reinterpret_cast<const std::uint64_t*>(static_cast<const std::uint8_t*>(data) + range.Begin);
    const double toMilliseconds = 1000.0 / static_cast<double>(s.timestampFrequency);
    performance::Milliseconds passes{};
    for (std::size_t i = 0; i + 1 < marks.size(); ++i)
    {
        // From the frame's list's end to the next list's start: the game's own work.
        if (!*marks[i].phase) continue;

        const double milliseconds = ticks[i + 1] >= ticks[i] ? static_cast<double>(ticks[i + 1] - ticks[i]) * toMilliseconds : 0.0;
        if (const auto pass = marks[i].pass) passes[static_cast<std::size_t>(*pass)] += static_cast<float>(milliseconds);
        auto& phases = s.tally.phases;
        const auto found = std::find_if(phases.begin(), phases.end(), [&](const auto& phase) { return std::strcmp(phase.first, marks[i].phase) == 0; });
        if (found != phases.end()) found->second += milliseconds;
        else phases.emplace_back(marks[i].phase, milliseconds);
    }

    const D3D12_RANGE none{};
    s.timestampReadback->Unmap(0, &none);
    marks.clear();
    performance::timedOnDirect3D12(passes);

    using enum performance::Pass;
    const auto sum = [&](std::initializer_list<performance::Pass> of) {
        double total{};
        for (const auto pass : of) total += passes[static_cast<std::size_t>(pass)];
        return total;
    };
    const double scene = sum({RayTracedScene});
    const double light = sum({GiRays, GiDenoiser});
    const double shadow = sum({SunShadowRays, SunShadowDenoiser, LightShadowRays, LightShadowFilter});
    s.sceneMilliseconds += (scene - s.sceneMilliseconds) * 0.05;
    s.lightMilliseconds += (light - s.lightMilliseconds) * 0.05;
    s.shadowMilliseconds += (shadow - s.shadowMilliseconds) * 0.05;
    ++s.tally.timedFrames;
}

std::string percentOf(std::uint64_t used, std::uint64_t capacity)
{
    return std::to_string(capacity ? used * 100 / capacity : 0) + "%";
}

// The probe as text, and a key that changes when what blocks its ray changes.
std::pair<std::string, std::string> describeProbe(const State& s, std::span<const InstancedModel> instancedModels, const GIShadowProbe& probe)
{
    const auto stats = s.scene->stats();
    const auto scene = std::format("scene: {} instances; geometry pool {}, structures {}, {} structures waiting to be built", s.instances,
        percentOf(stats.poolUsed, stats.poolCapacity), percentOf(stats.blasUsed, stats.blasCapacity), stats.waiting);
    if (probe.receiver.w <= 0) return {"centre pixel: the sky\n" + scene, "sky"};

    std::string text = std::format("centre pixel: surface {:.1f} m away at ({:.1f} {:.1f} {:.1f}), normal ({:.2f} {:.2f} {:.2f}), {:.2f} beyond the "
        "shadow map (0 inside, 1 past it); shadow there {:.2f}, this frame's ray {}\n",
        probe.normal.w, probe.receiver.x, probe.receiver.y, probe.receiver.z, probe.normal.x, probe.normal.y, probe.normal.z, probe.ray.w,
        probe.shadow.x, probe.shadow.y > 0.5f ? "lit" : "blocked");

    const auto& h = probe.hits;
    text += std::format("sun ray towards ({:.2f} {:.2f} {:.2f}): near segment to {:.2f} m (copy tolerance {:.2f} m), {} copies of the surface skipped; "
        "the shadow rays {}\n", probe.ray.x, probe.ray.y, probe.ray.z, probe.segments.x, probe.segments.y, h.x,
        h.y ? std::format("are blocked near, at {:.2f} m (instance {})", probe.segments.z, h.y - 1)
            : h.z ? std::format("are blocked at {:.1f} m (instance {})", probe.segments.w, h.z - 1)
                  : std::string("reach the sun"));

    // Where the rays start: the surface they leave, how it faces the sun, which side of it they leave
    // and how far off it, and whatever that lift passes through.
    const auto dot = [](const float4& a, const float4& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const auto& start = probe.start;
    const auto& lift = probe.lift;
    const auto& sh = probe.startHit;
    const bool exact = start.w > 0.5f, farSide = lift.w > 0.5f;
    text += std::format("the rays start on {}, its normal ({:.2f} {:.2f} {:.2f}) towards the eye, the sun at cosine {:.2f} to it (to the G-buffer's "
        "normal {:.2f}); they leave its {} side, lifted {:.1f} mm ({:.1f} mm along its normal); the lift passes through {}\n",
        exact ? std::format("the scene's own surface (instance {}, geometry {}, triangle {})", sh.x - 1, sh.y, sh.z)
              : std::string("the depth's point (the scene's surface not found there)"),
        start.x, start.y, start.z, dot(start, probe.ray), dot(probe.normal, probe.ray), farSide ? "far" : "sunward",
        std::sqrt(dot(lift, lift)) * 1000.0f, dot(lift, start) * 1000.0f,
        probe.crossed.x ? std::format("{} ({:.1f} mm from the start)", s.contents->describeHit(instancedModels, probe.crossed), std::bit_cast<float>(probe.crossed.w) * 1000.0f)
                        : std::string("nothing"));

    std::string key = std::format("{}|{}|{}|{}", h.y || h.z ? "blocked" : "lit", h.w ? std::to_string(probe.hitGeometry.x) : "-", farSide, probe.crossed.x);
    if (h.w)
    {
        const auto& g = probe.hitGeometry;
        const auto kind = g.y & 0xFFFFu, flags = g.y >> 16;
        const bool startsMesh = exact && sh.x == h.w && sh.y == (g.z & 0x7FFFFFFFu);
        text += std::format("with every instance: hits {}, geometry {} ({}{}{}), triangle {}{}", s.contents->describeHit(instancedModels, uint4{h.w, g.z, kind, g.w}), g.x,
            (flags & GI_GEOMETRY_CUT_OUT) ? "cut-out" : "opaque", (flags & GI_GEOMETRY_ALPHA_TEXTURE) ? " with the game's alpha" : "",
            (flags & GI_GEOMETRY_LEAF) ? ", leaves" : (flags & GI_GEOMETRY_HAIR) ? ", hair" : "", sh.w,
            !startsMesh ? "" : sh.w == sh.z ? ", the start's own triangle" : ", of the start's own mesh");
        if (kind & GI_INSTANCE_TERRAIN)
            text += std::format("; its triangle's highest corner at {:.1f} m as its ring was made, the heightmap {:.1f} m there now (level of detail {:.0f})",
                probe.terrain.x, probe.terrain.y, probe.terrain.z);
        text += "\n";
    }
    else
        text += "with every instance: nothing in the scene blocks it\n";

    const auto& g = probe.lightRays;
    text += std::format("its global illumination rays ({}): {} hit within 1 m, {} within 10 m, {} get away, {} hit a back face; the nearest hit: {}\n",
        GI_PROBE_LIGHT_RAYS, g.x, g.y, g.z, g.w, s.contents->describeHit(instancedModels, probe.lightNearest));
    text += std::format("the camera's ray (the scene view's): {}; the game's surface at {:.2f} m\n", s.contents->describeHit(instancedModels, probe.view), probe.normal.w);
    key += std::format("|{}", probe.view.x);

    // The characters where the sun ray's blocker is one, else where the camera's ray meets one: a
    // character's parts are models of their own in one place.
    const auto blocker = h.w && (probe.hitGeometry.y & GI_INSTANCE_CHARACTER) ? instancedAt(instancedModels, h.w - 1) : nullptr;
    const auto viewed = probe.view.x && (probe.view.z & GI_INSTANCE_CHARACTER) ? instancedAt(instancedModels, probe.view.x - 1) : nullptr;
    if (const auto character = blocker ? blocker : viewed)
        text += std::format("the characters at {}'s place, every mesh of each:{}\n", blocker ? "the blocker" : "the camera ray's hit",
            s.contents->charactersAt(character->position));

    // When the terrain rings were last made, and the game's models whose world bounds the ray crosses
    // past the receiver.
    const auto [alongRay, nearest] = s.contents->describeRay({probe.receiver.x, probe.receiver.y, probe.receiver.z}, {probe.ray.x, probe.ray.y, probe.ray.z},
        ShadowRayLength);
    text += std::format("frame {}; ", static_cast<std::uint32_t>(probe.terrain.w)) + alongRay;
    key += std::format("|{}", nearest);
    return {text + "\n" + scene, key};
}

// The probe the slot's last frame wrote, once the GPU is past it: into the overlay's text, and the
// log when what blocks its ray changed (at most five times a second).
void readProbe(State& s, std::uint32_t slot)
{
    if (!s.probeWritten[slot]) return;

    s.probeWritten[slot] = false;
    void* data{};
    if (FAILED(s.probeReadback[slot]->Map(0, nullptr, &data))) return;

    GIShadowProbe probe{};
    std::memcpy(&probe, data, sizeof(probe));
    const D3D12_RANGE none{};
    s.probeReadback[slot]->Unmap(0, &none);

    auto [text, key] = describeProbe(s, s.instancedModels[slot], probe);
    const auto now = GetTickCount64();
    std::lock_guard lock(probeMutex);
    probeText = text;
    if constexpr (DiagnosticLog)
    {
        if (key != lastProbeKey && now - lastProbeLog >= 200)
        {
            lastProbeKey = key;
            lastProbeLog = now;
            log("Shadow probe:\n" + text);
        }
    }
}

// Diagnostics: every ten seconds, what a frame cost on average.
void logTally(State& s)
{
    auto& t = s.tally;
    const auto now = GetTickCount64();
    if (!t.since) t.since = now;
    if (now - t.since < 10'000 || !t.frames) return;

    const auto milliseconds = [](double value) {
        auto text = std::to_string(value);
        return text.substr(0, text.find('.') + 4);
    };

    std::string line = "Global illumination's GPU time a frame (Direct3D 12):";
    double total{};
    for (const auto& [phase, sum] : t.phases)
    {
        const double perFrame = t.timedFrames ? sum / t.timedFrames : 0.0;
        line += std::string(" ") + phase + " " + milliseconds(perFrame) + ",";
        total += perFrame;
    }

    std::string steps;
    for (std::size_t step = 0; step < CpuSteps; ++step)
        steps += std::string(step ? ", " : "") + CpuStepNames[step] + " " + milliseconds(t.cpuSteps[step] / t.frames);

    log(line + " total " + milliseconds(total) + " ms. Bottom-level structures a frame: " + std::to_string(t.builds / t.frames) + " built ("
        + std::to_string(t.builtTriangles / t.frames / 1000) + "k triangles), at most " + std::to_string(t.mostBuilds) + " in a frame, "
        + std::to_string(t.waiting / t.frames) + " waiting, " + std::to_string(t.compactions) + " compacted in all. CPU a frame: " + milliseconds(t.cpuMilliseconds / t.frames)
        + " ms (" + steps + "), at most " + milliseconds(t.mostCpuMilliseconds) + " ms.");
    t = {};
    t.since = now;
}

namespace
{
void bindFrame(State& s, ID3D12GraphicsCommandList4& list, const FrameBinding& frame)
{
    ID3D12DescriptorHeap* heaps[]{s.heap.Get()};
    list.SetDescriptorHeaps(1, heaps);
    list.SetComputeRootSignature(s.rootSignature.Get());
    list.SetComputeRootConstantBufferView(0, frame.constants);
    list.SetComputeRootDescriptorTable(2, frame.srvs);
    list.SetComputeRootDescriptorTable(3, frame.uavs);
    list.SetComputeRootShaderResourceView(4, frame.topLevel);
    list.SetComputeRootDescriptorTable(5, frame.textures);
}

// A timestamp in `list` at each of a Recorder's marks, into the slot's.
std::function<void(const char*, std::optional<performance::Pass>)> timestampMarker(State& s, ID3D12GraphicsCommandList4& list, std::uint32_t slot)
{
    return [&s, &list, slot](const char* phase, std::optional<performance::Pass> pass) {
        auto& marks = s.marks[slot];
        if (marks.size() >= MaxMarks) return;

        list.EndQuery(s.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * MaxMarks + static_cast<UINT>(marks.size()));
        marks.push_back({phase ? phase : "", pass});
    };
}
}

Result render(const Frame& frame)
{
    auto& s = *state;
    const auto started = clock::ticksNow();
    auto lapStarted = started;
    const auto lap = [&](CpuStep step) {
        const auto now = clock::ticksNow();
        s.tally.cpuSteps[step] += clock::millisecondsOf(now - lapStarted);
        lapStarted = now;
    };

    Result result;
    const auto& traces = frame.traces;
    s.surfaceTrace.reset();
    const auto slot = static_cast<std::uint32_t>(s.frames++ % FramesInFlight);
    waitFor(s, s.submitted[slot]);
    lap(WaitStep);
    auto& scene = *s.scene;
    scene.beginFrame(slot, s.fence->GetCompletedValue(), frame.signalValue);
    if constexpr (DiagnosticLog) logStats(s);
    readTimes(s, slot);
    readProbe(s, slot);
    const std::function<void()> waitForAll = [&] { waitFor(s, *std::max_element(s.submitted.begin(), s.submitted.end())); };
    const trace::TraceFrame traceFrame{frame, s.device.Get(), waitForAll, *s.contents, s.instancedModels[slot], slot};
    for (auto* each : trace::inOrder()) each->prepare(traceFrame);
    lap(ProbeStep);
    if constexpr (DiagnosticLog)
    {
        static std::filesystem::path dumped;
        if (const auto folder = frame_log::recordingDirectory(); !folder.empty() && folder != dumped)
        {
            dumped = folder;
            s.contents->dumpModels(folder);
        }
    }
    if constexpr (DiagnosticLog) logTally(s);

    // A new render size starts the light and the shadows over. Each has memory only once traced;
    // the shadows give theirs back when turned off.
    // Ray Reconstruction coming or going changes the light's grid, a sample a render pixel or a 2 x 2.
    if (traces.tracesLight
        && (!s.light || s.light->width() != frame.width || s.light->height() != frame.height || s.light->isPerRenderPixel() != traces.isForReconstruction))
    {
        waitForAll();
        s.light.reset();
        s.light = std::make_unique<gi_light::Light>(s.device.Get(), frame.width, frame.height, traces.isForReconstruction);
        log("Global illumination's light at " + std::to_string(frame.width) + "x" + std::to_string(frame.height)
            + (traces.isForReconstruction ? ", a sample a render pixel for Ray Reconstruction." : "."));
    }

    if (traces.tracesSun && (!s.shadows || s.shadows->width() != frame.width || s.shadows->height() != frame.height))
    {
        waitForAll();
        s.shadows.reset();
        s.shadows = std::make_unique<gi_shadow::Shadows>(s.device.Get(), frame.width, frame.height);
        log("Ray-traced shadows at " + std::to_string(frame.width) + "x" + std::to_string(frame.height) + ".");
    }
    else if (!traces.tracesSun && s.shadows)
    {
        waitForAll();
        s.shadows.reset();
    }

    const bool probeLights = traces.tracesLampShadows && frame.probeLights;
    {
        std::lock_guard lock(probeMutex);
        if (!frame.debugShadows) probeText.clear();
    }

    // ---- The scene's contents: the game's models, textures, characters and terrain, then the frame's
    // instances ----
    const bool terrain = frame.terrain.has_value();
    const scene_contents::GameScene game{.frameIndex = frame.frameIndex,
        .eye = frame.eye,
        .newModels = frame.newModels,
        .hasNewLists = frame.modelInstancesUpdated,
        .placements = frame.modelInstances,
        .characters = frame.characters,
        .newTextures = frame.newTextures,
        .terrainEye = terrain ? std::optional(frame.eye) : std::nullopt,
        .seaCentre = frame.seaLevel ? std::optional(std::array{frame.eye[0], *frame.seaLevel, frame.eye[2]}) : std::nullopt,
        .shouldListInstances = frame.debugShadows || probeLights};
    std::optional<State::SurfaceTrace> surfaceTrace;
    auto update = s.contents->update(game);
    s.instancedModels[slot] = std::move(update.instanced);
    result.releasedModels = std::move(update.releasedModels);
    result.releasedTextures = std::move(update.releasedTextures);
    const auto instanceCount = scene.instanceCount();
    s.instances = instanceCount;
    s.triangles = update.triangles;
    lap(ContentsStep);

    // New geometry starts what was learnt about it over (ClearNewGeometries).
    const auto newGeometries = scene.takeNewGeometries();

    check(s.allocators[slot]->Reset(), "Resetting a command allocator");
    check(s.list->Reset(s.allocators[slot].Get(), nullptr), "Resetting the command list");
    auto& list = *s.list.Get();
    Descriptors descriptors(s.device.Get(), s.heap.Get(), s.descriptorSize, slot);
    shared_textures::SharedBindings tracesTextures(s.device.Get(), descriptors);
    try
    {
        auto* depth = openShared(s, s.depth, frame.textures.depth, "Opening the depth").Get();
        auto* normal = openShared(s, s.normal, frame.textures.normal, "Opening the normals").Get();
        auto* albedo = openShared(s, s.albedo, frame.textures.albedo, "Opening the albedo").Get();
        auto* output = openShared(s, s.output, frame.textures.output, "Opening the output").Get();
        auto* shadow = traces.tracesSun ? openShared(s, s.shadow, frame.textures.shadow, "Opening the shadow").Get() : nullptr;
        auto* shadowDebug = frame.debugShadows ? openShared(s, s.shadowDebug, frame.textures.shadowDebug, "Opening the shadow debug view").Get() : nullptr;
        // The light's debug views are its denoiser's.
        auto* lightDebug = traces.tracesLight && !traces.isForReconstruction && frame.textures.lightDebug
            ? openShared(s, s.lightDebug, frame.textures.lightDebug, "Opening the light's debug view").Get()
            : nullptr;
        auto* velocity = (traces.tracesSun || traces.tracesLight) && frame.textures.velocity
            ? openShared(s, s.velocity, frame.textures.velocity, "Opening the velocity").Get()
            : nullptr;
        // Path tracing's: Ray Reconstruction's guides, the reflections' and the lamps' (path_tracing_rays.hpp).
        path_tracing_rays::Textures traced;
        if (traces.tracesReflections || traces.tracesLampLight) traced.guideNormals = openShared(s, s.guideNormals, frame.textures.guideNormals, "Opening the normals guide").Get();
        if (traces.tracesReflections)
        {
            traced.guideSpecular = openShared(s, s.guideSpecular, frame.textures.guideSpecular, "Opening the specular albedo guide").Get();
            traced.reflection = openShared(s, s.reflection, frame.textures.reflection, "Opening the reflections").Get();
            traced.reflectionDistance = openShared(s, s.reflectionDistance, frame.textures.reflectionDistance, "Opening the reflections' hit distance").Get();
            traced.reflectionMap = openShared(s, s.reflectionMap, frame.textures.reflectionMap, "Opening the reflection map").Get();
            if (traces.tracesGlass)
            {
                traced.surfaceDepth = openShared(s, s.surfaceDepth, frame.textures.surfaceDepth, "Opening the glass's depth").Get();
                traced.surfaceNormal = openShared(s, s.surfaceNormal, frame.textures.surfaceNormal, "Opening the glass's normals").Get();
                traced.surfaceReflection = openShared(s, s.surfaceReflection, frame.textures.surfaceReflection, "Opening the glass's reflections").Get();
            }
        }
        if (traces.tracesLampLight)
        {
            traced.guideMaterial = openShared(s, s.guideMaterial, frame.textures.guideMaterial, "Opening the material guide").Get();
            traced.guideLobe = openShared(s, s.guideLobe, frame.textures.guideLobe, "Opening the lobe guide").Get();
            traced.specularTable = openShared(s, s.specularTable, frame.textures.specularTable, "Opening the game's specular table").Get();
            if (frame.textures.lampMasks) traced.lampMasks = openShared(s, s.lampMasks, frame.textures.lampMasks, "Opening the masks").Get();
            traced.lampDiffuse = openShared(s, s.lampDiffuse, frame.textures.lampDiffuse, "Opening the lamps' diffuse light").Get();
            traced.lampSpecular = openShared(s, s.lampSpecular, frame.textures.lampSpecular, "Opening the lamps' specular light").Get();
        }
        auto* materialSpecular = traces.isPathTraced
            ? openShared(s, s.materialSpecular, frame.textures.materialSpecular, "Opening the materials' specular colours").Get()
            : nullptr;
        auto* skyCube = frame.textures.skyCube ? openShared(s, s.skyCube, frame.textures.skyCube, "Opening the sky's cube").Get() : nullptr;
        auto* terrainPages = terrain ? openShared(s, s.terrainPages, frame.terrain->pages, "Opening the terrain's page table").Get() : nullptr;
        auto* terrainHeights = terrain ? openShared(s, s.terrainHeights, frame.terrain->heights, "Opening the terrain's heights").Get() : nullptr;

        Uploads uploads{s.uploadData[slot], s.upload[slot]->GetGPUVirtualAddress()};
        const auto newGeometryCount = static_cast<std::uint32_t>(newGeometries.size());
        const auto newGeometriesOffset = uploads.reserve(4, newGeometryCount);
        std::memcpy(uploads.data + newGeometriesOffset, newGeometries.data(), newGeometries.size() * 4);
        const auto& skins = update.skins;
        const auto skinCount = static_cast<std::uint32_t>(skins.size());
        const auto skinVertices = skins.empty() ? 0u : skins.back().firstVertex + skins.back().vertexCount;
        const auto skinOffset = uploads.reserve(sizeof(GISkinRecord), skinCount);
        std::memcpy(uploads.data + skinOffset, skins.data(), skins.size() * sizeof(GISkinRecord));
        const auto& palettes = update.palettes;
        const auto paletteRows = palettes.size() / 4;
        const auto paletteOffset = uploads.reserve(16, paletteRows);
        std::memcpy(uploads.data + paletteOffset, palettes.data(), palettes.size() * sizeof(float));

        const auto parity = frame.frameIndex & 1;
        const auto constantsOffset = uploads.reserve(256, (sizeof(GIFrameConstants) + 255) / 256);
        auto constants = constantsOf(frame, newGeometryCount, instanceCount);
        if (terrain)
        {
            const auto& t = *frame.terrain;
            constants.terrainScale = {t.scale[0], t.scale[1], t.scale[2], 1.0f};
            constants.terrainOffset = {t.offset[0], t.offset[1], t.offset[2], 0.0f};
            constants.terrainUvConverter = {t.uvConverter[0], t.uvConverter[1], t.uvConverter[2], t.uvConverter[3]};
            constants.terrainTexelCorrect = {t.texelCorrect[0], t.texelCorrect[1], t.texelCorrect[2], t.texelCorrect[3]};
            for (std::uint32_t r = 0; r < GI_TERRAIN_RINGS; ++r)
            {
                const auto& ring = update.terrain[r];
                constants.terrainRings[r] = {ring.corner[0], ring.corner[1], ring.cellSize, static_cast<float>(GI_TERRAIN_CELLS)};
                (&constants.terrainRingOffsets.x)[r] = static_cast<std::uint32_t>(ring.poolOffset);
            }
        }
        if (traces.tracesLight) s.light->fill(constants, frame.verticalFov, parity);
        if (traces.tracesSun) s.shadows->fill(constants);
        constants.objectMotion = {velocity ? 1u : 0u, 0, 0, 0};
        constants.shadowDiagnostics = {frame.twoSidedShadows ? 1u : 0u, frame.shadowStarts ? 1u : 0u, 0, 0};
        constants.lightDiagnostics = {0, 0, lightDebug ? static_cast<std::uint32_t>(frame.lightDebugView) : 0u, 0};
        constants.reconstruction = {traces.isForReconstruction ? 1u : 0u, materialSpecular ? 1u : 0u, skyCube ? 1u : 0u, 0};
        constants.emission = {frame.emissionScale, frame.emissionCap, 0, 0};
        if (traces.isPathTraced)
        {
            const auto emitters = std::min<std::size_t>(update.emitters.size(), GI_MAX_EMITTERS);
            std::copy_n(update.emitters.begin(), emitters, constants.emitters);
            constants.emitterCount = {static_cast<std::uint32_t>(emitters), 0, 0, 0};
        }

        // A ray a render pixel for Ray Reconstruction, whatever the ray quality: as many rays as High's four a
        // 2 x 2, the most a sample can take apart.
        constants.lightRays = {traces.isForReconstruction ? 1u : frame.lightRays, 0, 0, 0};

        // ---- Descriptors ----
        descriptors.texture(0, depth, DXGI_FORMAT_R32_FLOAT);
        descriptors.texture(1, normal, DXGI_FORMAT_R10G10B10A2_UNORM);
        descriptors.texture(2, albedo, DXGI_FORMAT_R8G8B8A8_UNORM);
        descriptors.buffer(4, s.upload[slot].Get(), newGeometriesOffset, 4, newGeometryCount);
        descriptors.buffer(12, s.upload[slot].Get(), skinOffset, sizeof(GISkinRecord), skinCount);
        descriptors.buffer(13, s.upload[slot].Get(), paletteOffset, 16, paletteRows);
        descriptors.buffer(5, scene.instanceBuffer(), 0, sizeof(GIInstanceDesc), instanceCount);
        descriptors.rawBuffer(6, scene.pool(), gi_scene::Scene::PoolBytes);
        descriptors.buffer(16, scene.geometryTable(), 0, sizeof(GIGeometry), MaxGeometries);
        descriptors.outputTexture(0, output, DXGI_FORMAT_R16G16B16A16_FLOAT);
        descriptors.outputRawBuffer(1, scene.pool(), gi_scene::Scene::PoolBytes);
        descriptors.outputBuffer(4, s.counters.Get(), 4, GI_COUNTER_COUNT);
        descriptors.outputBuffer(5, s.dispatchArguments.Get(), 16, GI_DISPATCH_SLOTS);
        descriptors.outputBuffer(6, s.geometryAlbedo.Get(), 16, MaxGeometries);
        if (terrain)
        {
            descriptors.texture(14, terrainPages, frame.terrain->pagesFormat);
            descriptors.texture(15, terrainHeights, frame.terrain->heightsFormat);
        }
        if (velocity) descriptors.texture(17, velocity, frame.textures.velocityFormat);
        if (materialSpecular) descriptors.texture(23, materialSpecular, DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (skyCube) descriptors.cube(24, skyCube, DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (frame.debugShadows)
        {
            descriptors.outputBuffer(77, s.probe.Get(), sizeof(GIShadowProbe), 1);
            descriptors.outputTexture(78, shadowDebug, DXGI_FORMAT_R8G8B8A8_UNORM);
        }
        if (lightDebug) descriptors.outputTexture(79, lightDebug, DXGI_FORMAT_R8G8B8A8_UNORM);
        if (traces.tracesReflections || traces.tracesLampLight) path_tracing_rays::describe(descriptors, traced);
        if (traces.tracesLight) s.light->describe(descriptors, parity);
        if (traces.tracesSun) s.shadows->describe(descriptors, parity, shadow);
        trace::TraceBindings bindings{constants, descriptors, tracesTextures};
        for (auto* each : trace::inOrder())
            if (each->runsIn(traces)) each->bind(traceFrame, bindings);

        std::memcpy(uploads.data + constantsOffset, &constants, sizeof(constants));

        // The game's textures stay in the common state between frames: the shaders' reads promote them.
        auto textureRange = s.heap->GetGPUDescriptorHandleForHeapStart();
        textureRange.ptr += std::uint64_t{TableSize} * FramesInFlight * s.descriptorSize;
        const FrameBinding binding{uploads.address + constantsOffset, scene.topLevel()->GetGPUVirtualAddress(), descriptors.srvTable(), descriptors.uavTable(), textureRange};
        const auto bind = [&] { bindFrame(s, list, binding); };
        bind();
        Recorder recorder(list, s.pipelines, s.dispatchSignature.Get(), s.dispatchArguments.Get());
        auto& marks = s.marks[slot];
        marks.clear();
        recorder.rebinder = bind;
        recorder.marker = timestampMarker(s, list, slot);

        // The shared textures, in the state the passes use each in: they leave the common state first and go
        // back to it after. Those the frame does not use are null.
        constexpr auto Read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, Write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        const std::pair<ID3D12Resource*, D3D12_RESOURCE_STATES> shared[]{{depth, Read}, {normal, Read}, {albedo, Read}, {output, Write},
            {shadow, Write}, {shadowDebug, Write}, {lightDebug, Write}, {velocity, Read}, {terrainPages, Read},
            {terrainHeights, Read}, {traced.guideNormals, Read}, {traced.guideSpecular, Read}, {traced.reflection, Write}, {traced.reflectionDistance, Write}, {traced.reflectionMap, Write},
            {traced.guideMaterial, Read}, {traced.guideLobe, Read}, {traced.specularTable, Read}, {traced.lampMasks, Read}, {traced.lampDiffuse, Write}, {traced.lampSpecular, Write}};
        const auto transitionShared = [&](bool toCommon) {
            std::vector<D3D12_RESOURCE_BARRIER> barriers;
            if (toCommon) barriers.push_back(uavBarrier());
            for (const auto& [resource, inUse] : shared)
                if (resource)
                    barriers.push_back(toCommon ? transition(resource, inUse, D3D12_RESOURCE_STATE_COMMON)
                                                : transition(resource, D3D12_RESOURCE_STATE_COMMON, inUse));
            tracesTextures.addTransitions(barriers, toCommon);
            list.ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        };

        recorder.mark("scene writes", performance::Pass::RayTracedScene);
        transitionShared(false);

        // The pool's uploads and the table's rows, then the geometry the GPU makes: the terrain, the
        // characters' poses.
        scene.recordWrites(list);
        if (newGeometryCount) recorder.dispatch(Kernel::ClearNewGeometries, groups(newGeometryCount, 64));

        for (std::uint32_t r = 0; r < GI_TERRAIN_RINGS; ++r)
            if (update.terrain[r].isRemade) recorder.dispatch(Kernel::TerrainGrid, groups(GI_TERRAIN_CELLS, 8), groups(GI_TERRAIN_CELLS, 8), {r});

        if (skinCount) recorder.dispatch(Kernel::Skin, groups(skinVertices, 64), 1, {skinVertices, skinCount});

        recorder.mark("bottom-level structures", performance::Pass::RayTracedScene);
        scene.recordBottomLevels(list);

        // The instances, then the top level over the bottom levels.
        recorder.mark("instances and top level", performance::Pass::RayTracedScene);
        scene.recordInstances(list);
        scene.recordTopLevel(list);

        // ---- The shadows, then the light or the debug view ----
        for (auto* each : trace::inOrder())
            if (each->runsIn(traces)) each->record(traceFrame, recorder);

        if (traces.tracesLampLight) path_tracing_rays::recordLamps(recorder, frame.width, frame.height);
        if (traces.tracesSun) s.shadows->record(recorder, traces.isForReconstruction);
        if (frame.debugShadows)
        {
            recorder.mark("debug view and probe");
            recorder.dispatch(Kernel::DebugShadows, groups(frame.width, 8), groups(frame.height, 8));
            recorder.dispatch(Kernel::ProbeShadows, 1, 1);
            recorder.barrier();
            recorder.transition(s.probe.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list.CopyBufferRegion(s.probeReadback[slot].Get(), 0, s.probe.Get(), 0, sizeof(GIShadowProbe));
            recorder.transition(s.probe.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            s.probeWritten[slot] = true;
        }
        if (frame.showScene)
        {
            recorder.mark("scene view");
            recorder.dispatch(Kernel::DebugScene, groups(frame.width, 8), groups(frame.height, 8));
        }
        else if (traces.tracesLight) s.light->record(recorder, parity, denoiserCamera(frame), slot, lightDebug && frame.lightDebugView == LightDebugView::Validation);

        if (traces.tracesReflections) path_tracing_rays::recordReflections(recorder, frame.width, frame.height);

        if (traces.tracesGlass) surfaceTrace = State::SurfaceTrace{slot, frame.width, frame.height, binding, traced.surfaceReflection};

        if constexpr (DiagnosticLog)
        {
            if (traces.tracesLight && !frame.showScene) copyStats(s, recorder, output, frame);
        }

        transitionShared(true);
        recorder.mark(nullptr);
        list.ResolveQueryData(s.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * MaxMarks, static_cast<UINT>(marks.size()), s.timestampReadback.Get(),
            std::uint64_t{slot} * MaxMarks * 8);
        check(list.Close(), "Recording the global illumination");
    }
    catch (...)
    {
        // The next frame resets the list, which it can only once closed.
        list.Close();
        throw;
    }

    check(s.queue->Wait(s.fence.Get(), frame.waitValue), "Waiting for the global illumination's inputs");
    ID3D12CommandList* lists[]{s.list.Get()};
    s.queue->ExecuteCommandLists(1, lists);
    check(s.queue->Signal(s.fence.Get(), frame.signalValue), "Signalling the global illumination");
    s.submitted[slot] = frame.signalValue;
    tracesTextures.markWritten();
    lap(RecordStep);
    s.surfaceTrace = surfaceTrace;

    const double cpu = clock::millisecondsOf(clock::ticksNow() - started);
    const auto stats = scene.stats();
    auto& t = s.tally;
    ++t.frames;
    t.cpuMilliseconds += cpu;
    t.mostCpuMilliseconds = std::max(t.mostCpuMilliseconds, cpu);
    t.builds += stats.builtThisFrame;
    t.mostBuilds = std::max(t.mostBuilds, stats.builtThisFrame);
    t.builtTriangles += stats.builtTriangles;
    t.waiting += stats.waiting;
    t.compactions += stats.compactedThisFrame;

    return result;
}

bool traceSurfaces(std::uint64_t waitValue, std::uint64_t signalValue)
{
    if (!state || !state->surfaceTrace) return false;

    auto& s = *state;
    const auto trace = *std::exchange(s.surfaceTrace, std::nullopt);
    check(s.surfaceAllocators[trace.slot]->Reset(), "Resetting a command allocator");
    check(s.surfaceList->Reset(s.surfaceAllocators[trace.slot].Get(), nullptr), "Resetting the command list");
    auto& list = *s.surfaceList.Get();
    try
    {
        bindFrame(s, list, trace.binding);
        Recorder recorder(list, s.pipelines, s.dispatchSignature.Get(), s.dispatchArguments.Get());
        auto& marks = s.marks[trace.slot];
        const auto firstMark = static_cast<UINT>(marks.size());
        recorder.marker = timestampMarker(s, list, trace.slot);
        recorder.transition(trace.reflection, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        path_tracing_rays::recordSurfaceReflections(recorder, trace.width, trace.height);
        recorder.transition(trace.reflection, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        recorder.mark(nullptr);
        const auto marked = static_cast<UINT>(marks.size()) - firstMark;
        list.ResolveQueryData(s.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, trace.slot * MaxMarks + firstMark, marked, s.timestampReadback.Get(),
            (std::uint64_t{trace.slot} * MaxMarks + firstMark) * 8);
        check(list.Close(), "Recording the glass's reflections");
    }
    catch (...)
    {
        // The next trace resets the list, which it can only once closed.
        list.Close();
        throw;
    }

    check(s.queue->Wait(s.fence.Get(), waitValue), "Waiting for the glass");
    ID3D12CommandList* lists[]{s.surfaceList.Get()};
    s.queue->ExecuteCommandLists(1, lists);
    check(s.queue->Signal(s.fence.Get(), signalValue), "Signalling the glass's reflections");
    s.submitted[trace.slot] = signalValue;

    return true;
}

std::string shadowProbe()
{
    std::lock_guard lock(probeMutex);
    return probeText;
}

std::string status()
{
    if (!state) return {};

    const auto& s = *state;
    const auto stats = s.scene->stats();
    const auto milliseconds = [](double value) {
        auto text = std::to_string(value);
        return text.substr(0, text.find('.') + 3) + " ms";
    };

    const auto compactedShare = stats.compactedFrom ? std::to_string(stats.compactedTo * 100 / stats.compactedFrom) + "%" : std::string("-");
    return std::to_string(s.instances) + " instances, " + std::to_string(s.triangles / 1000) + "k triangles; geometry pool "
        + percentOf(stats.poolUsed, stats.poolCapacity) + ", structures " + percentOf(stats.blasUsed, stats.blasCapacity) + " (" + std::to_string(stats.structures)
        + ", " + std::to_string(stats.compacted) + " compacted to " + compactedShare + " of their size, " + std::to_string(stats.waiting) + " waiting), "
        + std::to_string(stats.geometriesUsed) + " geometries; GPU: scene " + milliseconds(s.sceneMilliseconds) + ", light " + milliseconds(s.lightMilliseconds)
        + ", shadows " + milliseconds(s.shadowMilliseconds) + "; " + s.contents->status();
}
}
