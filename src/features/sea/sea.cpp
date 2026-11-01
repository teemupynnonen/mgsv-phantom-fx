// The sea on Direct3D 11; sea.hpp describes it.
#include "features/sea/sea.hpp"

#include "clock.hpp"
#include "command_list_notes.hpp"
#include "d3d11_helpers.hpp"
#include "features/sea/sea_shaders.hpp"
#include "frame_latch.hpp"
#include "game_constants.hpp"
#include "log.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"
#include "performance.hpp"
#include "weather.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace phantom_fx::sea
{
namespace
{
using Microsoft::WRL::ComPtr;
using game_shaders::GamePass;

constexpr float Pi = 3.14159265f;

// The waves' tiles: each N x N texels across its size, the sizes far from multiples of each other so their
// repeats do not line up. Each takes the waves from 6 of the next one's tiles across down to its own size's
// worth, so no wave is in two: 400 m to 9.8 m, 9.8 m to 1.45 m, 1.45 m to 7 cm.
constexpr UINT Cascades = 3, Texels = 256, Levels = 9;
constexpr std::array<float, Cascades> CascadeSizes{400, 59, 8.7f};

// The surface's rings: 128 cells across, the finest 0.25 m apart (32 m across), each next twice as coarse;
// the twelfth is 65 km across, past the horizon from a helicopter.
constexpr UINT GridCells = 128, Rings = 12;
constexpr float GridSpacing = 0.25f;

// The open ocean's fetch, and the wind each weather raises over it, in metres a second at 10 m: a gentle
// breeze in fog, a moderate one in the sun, a gale in rain.
constexpr float Fetch = 300'000, Gravity = 9.81f;
constexpr float WindOf(weather::Type type)
{
    switch (type)
    {
    case weather::Type::Cloudy: return 8;
    case weather::Type::Rainy: return 14;
    case weather::Type::Sandstorm: return 10;
    case weather::Type::Foggy: return 4;
    default: return 6;
    }
}

// The wind turns towards the weather's over about 20 seconds; foam fades over about 2.5.
constexpr float WindSeconds = 20, FoamSeconds = 2.5f;
constexpr float Choppiness = 1;

// The game's sea, seen within this long, is still there; a second without the main view's G-buffer (a
// loading screen, a menu) forgets it, so a sea is never drawn into another map.
constexpr std::uint64_t ShownForMilliseconds = 60'000, ForgottenAfterMilliseconds = 1'000;

// The game's near sea draws' material constants (cVSMaterial, b4), copied on the GPU as they run: c0.z is the
// sea's level, c1.xy the wind's direction. Read back at Mother Base (2026-10-30): c0.z -23.4, as its patches'
// world matrices place them; the far sea's c0.z (its waterHeight, the plane it clips at) 5 m lower, -28.4.
// Read on the CPU through the constant capture they were other draws' constants (levels from -62 m to 2000 m
// within a second): the game's threads do not write them where it maps them.
constexpr UINT GameMaterialSlot = 4, GameMaterialSize = 8 * game_constants::RegisterSize;
// sea_shaders.hpp's cbuffers: Sea at b9 of every stage, the game's sea's material copy at b10.
constexpr UINT ConstantSlot = 9, GameSeaSlot = 10;
struct Constants
{
    float cascadeSizes[4], cascadeLow[4], cascadeHigh[4];
    float alpha, peakFrequency, peakBoost, time;
    float foamLeft, choppiness, gridSpacing;
    std::uint32_t gridCells;
    float microRoughness, waterAlbedo[3];
};

static_assert(sizeof(Constants) == 96);

struct Wanted
{
    Sea sea;
    bool isRunning{};
};

FrameLatch<Wanted> latch;
// From each present on, for the frames recorded after it: the effect plan runs the sea, and the sea is drawn
// in place of the game's (the game shows its sea and has given its level, and nothing failed).
std::atomic<bool> isRunning{}, isDrawing{};
std::atomic<bool> hasPipelineFailed{}, hasDrawFailed{};

// Presents so far: the recording threads tell one frame from the next by them, to take the main view's
// G-buffer and the game's sea once each between two presents.
std::atomic<std::uint64_t> presents{1}, gBufferSeenAt{}, gameSeaCopiedAt{};
// GetTickCount64 when the game last drew its sea, and when the main view's G-buffer was last drawn into.
std::atomic<std::uint64_t> lastGameSea{}, lastGBuffer{};

// What a deferred context, then its command list, recorded that the immediate context needs.
struct Notes
{
    bool isSeaDrawn{}, isGameSeaCopied{};

    void merge(Notes later)
    {
        isSeaDrawn |= later.isSeaDrawn;
        isGameSeaCopied |= later.isGameSeaCopied;
    }
};

CommandListNotes<Notes> notes;

// The waves at present, for the frame recorded after it (present() only).
double seconds{};
float wind{};
LONGLONG lastPresent{};
Constants frameConstants{};

// The render targets a thread's context has bound: the G-buffer's, or a single target with depth (the
// forward passes, which the game's sea draws in).
struct BoundTargets
{
    ID3D11DeviceContext* context{};
    bool isGBuffer{}, isSingleTarget{};
};
thread_local BoundTargets bound;

// ---- Direct3D, guarded by resourceMutex ----
// A tile's maps, a slice a tile, with their levels of detail.
struct Maps
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11UnorderedAccessView> access;
};

// A structured buffer the kernels read and write.
struct WorkBuffer
{
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11UnorderedAccessView> access;
};

std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> immediate;
ComPtr<ID3D11ComputeShader> spectrumShader, rowShader, columnShader, mapShader;
ComPtr<ID3D11VertexShader> surfaceVertex;
ComPtr<ID3D11PixelShader> surfacePixel;
ComPtr<ID3D11Buffer> constantBuffer, indexBuffer, gameSeaMaterial;
// The level's readback (level()): copied from gameSeaMaterial at present, read a second later.
ComPtr<ID3D11Buffer> levelReadback;
bool isLevelReadbackPending{};
std::uint64_t lastLevelRead{};
std::optional<float> readLevel;
// Handed over as the immediate context runs the lists: the frame being run has the sea in its G-buffer
// (until its present); a list that copied the game's sea has run, so gameSeaMaterial holds its level.
bool isSeaInFrameBeingRun{}, hasGameSeaCopy{};
ComPtr<ID3D11RasterizerState> rasterizer;
ComPtr<ID3D11DepthStencilState> depthState;
ComPtr<ID3D11SamplerState> linearSampler, anisotropicSampler;
WorkBuffer spectrum, foam;
Maps displacement, derivatives, moments;
// The sea's depth where it drew, at the render size (surfaceDepth).
ComPtr<ID3D11Texture2D> depthTexture;
ComPtr<ID3D11RenderTargetView> depthTarget;
ComPtr<ID3D11ShaderResourceView> depthView;
UINT depthWidth{}, depthHeight{};

WorkBuffer makeWorkBuffer(UINT count, UINT stride, const char* what)
{
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = count * stride;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = stride;
    WorkBuffer made;
    check(device->CreateBuffer(&desc, nullptr, made.buffer.GetAddressOf()), what);
    check(device->CreateUnorderedAccessView(made.buffer.Get(), nullptr, made.access.GetAddressOf()), what);

    return made;
}

Maps makeMaps(DXGI_FORMAT format, const char* what)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = Texels;
    desc.MipLevels = Levels;
    desc.ArraySize = Cascades;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    Maps made;
    check(device->CreateTexture2D(&desc, nullptr, made.texture.GetAddressOf()), what);
    check(device->CreateShaderResourceView(made.texture.Get(), nullptr, made.view.GetAddressOf()), what);

    D3D11_UNORDERED_ACCESS_VIEW_DESC access{};
    access.Format = format;
    access.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    access.Texture2DArray.ArraySize = Cascades;
    check(device->CreateUnorderedAccessView(made.texture.Get(), &access, made.access.GetAddressOf()), what);

    return made;
}

ComPtr<ID3D11ComputeShader> kernel(const char* source, const char* name, const D3D_SHADER_MACRO* defines = nullptr)
{
    return computeShader(device.Get(), (std::string(shaders::Common) + source).c_str(), name, defines);
}

// Two triangles a cell, the vertices row by row; the vertex shader places them from their index.
ComPtr<ID3D11Buffer> makeIndices()
{
    constexpr UINT Across = GridCells + 1;
    static_assert(Across * Across <= 65536);
    std::vector<std::uint16_t> indices;
    indices.reserve(GridCells * GridCells * 6);
    for (UINT row = 0; row < GridCells; ++row)
        for (UINT column = 0; column < GridCells; ++column)
        {
            const auto corner = static_cast<std::uint16_t>(row * Across + column);
            const auto below = static_cast<std::uint16_t>(corner + Across);
            indices.insert(indices.end(), {corner, below, static_cast<std::uint16_t>(corner + 1), static_cast<std::uint16_t>(corner + 1), below,
                                              static_cast<std::uint16_t>(below + 1)});
        }

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(indices[0]));
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    const D3D11_SUBRESOURCE_DATA data{indices.data()};
    ComPtr<ID3D11Buffer> buffer;
    check(device->CreateBuffer(&desc, &data, buffer.GetAddressOf()), "Creating the sea's indices");

    return buffer;
}

// Compiles and makes everything once; call with resourceMutex held. Returns false when it is unavailable.
bool ensurePipeline()
{
    if (surfacePixel) return true;
    if (!device || hasPipelineFailed) return false;

    try
    {
        constantBuffer = makeConstantBuffer(device.Get(), sizeof(Constants), "Creating the sea's constants");
        // Zero until the game's first sea draws: sea level, and the shaders' own wind.
        D3D11_BUFFER_DESC copy{};
        copy.ByteWidth = GameMaterialSize;
        copy.Usage = D3D11_USAGE_DEFAULT;
        copy.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const std::array<float, GameMaterialSize / 4> zeros{};
        const D3D11_SUBRESOURCE_DATA initial{zeros.data()};
        check(device->CreateBuffer(&copy, &initial, gameSeaMaterial.GetAddressOf()), "Creating the game's sea constants");
        D3D11_BUFFER_DESC readback{};
        readback.ByteWidth = GameMaterialSize;
        readback.Usage = D3D11_USAGE_STAGING;
        readback.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateBuffer(&readback, nullptr, levelReadback.GetAddressOf()), "Creating the sea level's readback");
        spectrumShader = kernel(shaders::Spectrum, "sea-spectrum");
        const D3D_SHADER_MACRO rows[]{{"COLUMNS", "0"}, {}}, columns[]{{"COLUMNS", "1"}, {}};
        rowShader = kernel(shaders::Fft, "sea-fft-rows", rows);
        columnShader = kernel(shaders::Fft, "sea-fft-columns", columns);
        mapShader = kernel(shaders::Maps, "sea-maps");

        const auto surface = std::string(shaders::Common) + shaders::Surface;
        const D3D_SHADER_MACRO vertex[]{{"VERTEX", "1"}, {}};
        const auto vertexCode = compile(surface.c_str(), "sea-surface-vertex", "vs_5_0", vertex);
        check(device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr, surfaceVertex.GetAddressOf()),
            "Creating the sea's vertex shader");

        spectrum = makeWorkBuffer(Cascades * 2 * Texels * Texels, 16, "Creating the sea's spectrum");
        foam = makeWorkBuffer(Cascades * Texels * Texels, 4, "Creating the sea's foam");
        displacement = makeMaps(DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the sea's displacement");
        derivatives = makeMaps(DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the sea's slopes");
        moments = makeMaps(DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the sea's slope spread");
        indexBuffer = makeIndices();

        // Both sides: a folding crest shows its back.
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        check(device->CreateRasterizerState(&raster, rasterizer.GetAddressOf()), "Creating the sea's rasterizer state");

        // As the G-buffer's objects: reversed Z, nearer is greater.
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        check(device->CreateDepthStencilState(&depth, depthState.GetAddressOf()), "Creating the sea's depth state");

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampler, linearSampler.GetAddressOf()), "Creating the sea's sampler");
        sampler.Filter = D3D11_FILTER_ANISOTROPIC;
        sampler.MaxAnisotropy = 16;
        check(device->CreateSamplerState(&sampler, anisotropicSampler.GetAddressOf()), "Creating the sea's anisotropic sampler");

        // Last, so it marks a complete pipeline.
        const auto pixelCode = compile(surface.c_str(), "sea-surface-pixel", "ps_5_0");
        check(device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr, surfacePixel.GetAddressOf()),
            "Creating the sea's pixel shader");
        return true;
    }
    catch (const std::exception& error)
    {
        hasPipelineFailed = true;
        log(std::string("The sea is unavailable: ") + error.what());
        return false;
    }
}

void releasePipeline()
{
    spectrumShader.Reset();
    rowShader.Reset();
    columnShader.Reset();
    mapShader.Reset();
    surfaceVertex.Reset();
    surfacePixel.Reset();
    constantBuffer.Reset();
    indexBuffer.Reset();
    gameSeaMaterial.Reset();
    levelReadback.Reset();
    isLevelReadbackPending = false;
    readLevel.reset();
    // The game's sea's copy starts again from zeros.
    isSeaInFrameBeingRun = hasGameSeaCopy = false;
    rasterizer.Reset();
    depthState.Reset();
    linearSampler.Reset();
    anisotropicSampler.Reset();
    spectrum = foam = {};
    displacement = derivatives = moments = {};
    depthTexture.Reset();
    depthTarget.Reset();
    depthView.Reset();
    depthWidth = depthHeight = 0;
}

// The sea's depth target at the G-buffer's size; call with resourceMutex held.
void ensureDepthTarget(UINT width, UINT height)
{
    if (depthWidth == width && depthHeight == height) return;

    depthTexture.Reset();
    depthTarget.Reset();
    depthView.Reset();
    depthWidth = depthHeight = 0;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    check(device->CreateTexture2D(&desc, nullptr, depthTexture.GetAddressOf()), "Creating the sea's depth");
    check(device->CreateRenderTargetView(depthTexture.Get(), nullptr, depthTarget.GetAddressOf()), "Creating the sea's depth");
    check(device->CreateShaderResourceView(depthTexture.Get(), nullptr, depthView.GetAddressOf()), "Creating the sea's depth");
    depthWidth = width;
    depthHeight = height;
}

// The waves at the frame's time: the spectrum, its inverse FFT along the rows and the columns, and the maps
// with their levels of detail. Call with resourceMutex held.
void simulate(ID3D11DeviceContext* context)
{
    {
        const SavedComputeState saved(context, {.buffers = GameSeaSlot + 1, .accesses = 5});
        ID3D11Buffer* const constants[]{constantBuffer.Get(), gameSeaMaterial.Get()};
        context->CSSetConstantBuffers(ConstantSlot, static_cast<UINT>(std::size(constants)), constants);
        context->CSSetUnorderedAccessViews(0, 1, spectrum.access.GetAddressOf(), nullptr);
        context->CSSetShader(spectrumShader.Get(), nullptr, 0);
        context->Dispatch(groups(Texels), groups(Texels), Cascades);
        // A thread group a line of a slice; the column pass waits for the row pass's writes.
        context->CSSetShader(rowShader.Get(), nullptr, 0);
        context->Dispatch(Texels, Cascades * 2, 1);
        context->CSSetShader(columnShader.Get(), nullptr, 0);
        context->Dispatch(Texels, Cascades * 2, 1);

        ID3D11UnorderedAccessView* const outputs[]{spectrum.access.Get(), displacement.access.Get(), derivatives.access.Get(), moments.access.Get(),
            foam.access.Get()};
        context->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(outputs)), outputs, nullptr);
        context->CSSetShader(mapShader.Get(), nullptr, 0);
        context->Dispatch(groups(Texels), groups(Texels), Cascades);
    }

    context->GenerateMips(displacement.view.Get());
    context->GenerateMips(derivatives.view.Get());
    context->GenerateMips(moments.view.Get());
}

// What the surface's draw changes on a recording context, the game's put back when this goes out of scope.
class SavedSurfaceState
{
public:
    explicit SavedSurfaceState(ID3D11DeviceContext* context) : context(context)
    {
        context->IAGetInputLayout(layout.GetAddressOf());
        context->IAGetPrimitiveTopology(&topology);
        context->IAGetIndexBuffer(indices.GetAddressOf(), &indexFormat, &indexOffset);
        context->VSGetShader(vertex.GetAddressOf(), nullptr, nullptr);
        context->VSGetConstantBuffers(ConstantSlot, ConstantCount, vertexConstants);
        context->VSGetShaderResources(0, ViewCount, vertexViews);
        context->VSGetSamplers(0, 1, vertexSampler.GetAddressOf());
        context->HSGetShader(hull.GetAddressOf(), nullptr, nullptr);
        context->DSGetShader(domain.GetAddressOf(), nullptr, nullptr);
        context->GSGetShader(geometry.GetAddressOf(), nullptr, nullptr);
        context->PSGetShader(pixel.GetAddressOf(), nullptr, nullptr);
        context->PSGetConstantBuffers(ConstantSlot, 1, pixelConstants.GetAddressOf());
        context->PSGetShaderResources(0, ViewCount, pixelViews);
        context->PSGetSamplers(0, SamplerCount, pixelSamplers);
        context->RSGetState(rasterizer.GetAddressOf());
        context->OMGetBlendState(blend.GetAddressOf(), blendFactor, &sampleMask);
        context->OMGetDepthStencilState(depthState.GetAddressOf(), &stencilReference);
        context->OMGetRenderTargets(TargetCount, targets, depth.GetAddressOf());
    }

    ~SavedSurfaceState()
    {
        context->IASetInputLayout(layout.Get());
        context->IASetPrimitiveTopology(topology);
        context->IASetIndexBuffer(indices.Get(), indexFormat, indexOffset);
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->VSSetConstantBuffers(ConstantSlot, ConstantCount, vertexConstants);
        context->VSSetShaderResources(0, ViewCount, vertexViews);
        context->VSSetSamplers(0, 1, vertexSampler.GetAddressOf());
        context->HSSetShader(hull.Get(), nullptr, 0);
        context->DSSetShader(domain.Get(), nullptr, 0);
        context->GSSetShader(geometry.Get(), nullptr, 0);
        context->PSSetShader(pixel.Get(), nullptr, 0);
        context->PSSetConstantBuffers(ConstantSlot, 1, pixelConstants.GetAddressOf());
        context->PSSetShaderResources(0, ViewCount, pixelViews);
        context->PSSetSamplers(0, SamplerCount, pixelSamplers);
        context->RSSetState(rasterizer.Get());
        context->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
        context->OMSetDepthStencilState(depthState.Get(), stencilReference);
        context->OMSetRenderTargets(TargetCount, targets, depth.Get());

        release(vertexConstants);
        release(vertexViews);
        release(pixelViews);
        release(pixelSamplers);
        release(targets);
    }

    SavedSurfaceState(const SavedSurfaceState&) = delete;
    SavedSurfaceState& operator=(const SavedSurfaceState&) = delete;

private:
    static constexpr UINT ViewCount = 3, SamplerCount = 2, TargetCount = 4, ConstantCount = GameSeaSlot + 1 - ConstantSlot;

    template<class Object, std::size_t Count> static void release(Object* (&objects)[Count])
    {
        for (auto* object : objects)
            if (object) object->Release();
    }

    ID3D11DeviceContext* context;

    ComPtr<ID3D11InputLayout> layout;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    ComPtr<ID3D11Buffer> indices;
    DXGI_FORMAT indexFormat{};
    UINT indexOffset{};
    ComPtr<ID3D11VertexShader> vertex;
    ID3D11Buffer* vertexConstants[ConstantCount]{};
    ID3D11ShaderResourceView* vertexViews[ViewCount]{};
    ComPtr<ID3D11SamplerState> vertexSampler;
    ComPtr<ID3D11HullShader> hull;
    ComPtr<ID3D11DomainShader> domain;
    ComPtr<ID3D11GeometryShader> geometry;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11Buffer> pixelConstants;
    ID3D11ShaderResourceView* pixelViews[ViewCount]{};
    ID3D11SamplerState* pixelSamplers[SamplerCount]{};
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
    float blendFactor[4]{};
    UINT sampleMask{};
    ComPtr<ID3D11DepthStencilState> depthState;
    UINT stencilReference{};
    ID3D11RenderTargetView* targets[TargetCount]{};
    ComPtr<ID3D11DepthStencilView> depth;
};

// The sea into the G-buffer bound on `context`, before the game's first draw into it: the waves, then the
// surface into the G-buffer's three targets, its depth and the sea's own depth. Returns whether it drew. Call
// with resourceMutex held.
bool drawSea(ID3D11DeviceContext* context, const Constants& constants)
{
    ComPtr<ID3D11RenderTargetView> gameTargets[3];
    ComPtr<ID3D11DepthStencilView> gameDepth;
    ID3D11RenderTargetView* raw[3]{};
    context->OMGetRenderTargets(3, raw, gameDepth.GetAddressOf());
    for (int i = 0; i < 3; ++i) gameTargets[i].Attach(raw[i]);
    if (!gameTargets[0] || !gameTargets[1] || !gameTargets[2] || !gameDepth) return false;

    const auto [width, height] = sizeOf(gameTargets[0].Get());
    ensureDepthTarget(width, height);
    check(write(context, constantBuffer.Get(), constants), "Writing the sea's constants");
    performance::Timed timed(context, performance::Pass::SeaWaves);
    simulate(context);
    timed.next(performance::Pass::SeaSurface);

    const SavedSurfaceState saved(context);
    const float noSea[4]{};
    context->ClearRenderTargetView(depthTarget.Get(), noSea);
    ID3D11RenderTargetView* const targets[]{gameTargets[0].Get(), gameTargets[1].Get(), gameTargets[2].Get(), depthTarget.Get()};
    context->OMSetRenderTargets(static_cast<UINT>(std::size(targets)), targets, gameDepth.Get());
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(depthState.Get(), 0);
    context->RSSetState(rasterizer.Get());
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetIndexBuffer(indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);

    ID3D11ShaderResourceView* const maps[]{displacement.view.Get(), derivatives.view.Get(), moments.view.Get()};
    context->VSSetShader(surfaceVertex.Get(), nullptr, 0);
    ID3D11Buffer* const buffers[]{constantBuffer.Get(), gameSeaMaterial.Get()};
    context->VSSetConstantBuffers(ConstantSlot, static_cast<UINT>(std::size(buffers)), buffers);
    context->VSSetShaderResources(0, 1, maps);
    context->VSSetSamplers(0, 1, linearSampler.GetAddressOf());
    ID3D11SamplerState* const samplers[]{linearSampler.Get(), anisotropicSampler.Get()};
    context->PSSetShader(surfacePixel.Get(), nullptr, 0);
    context->PSSetConstantBuffers(ConstantSlot, 1, constantBuffer.GetAddressOf());
    context->PSSetShaderResources(0, static_cast<UINT>(std::size(maps)), maps);
    context->PSSetSamplers(0, static_cast<UINT>(std::size(samplers)), samplers);
    context->DrawIndexedInstanced(GridCells * GridCells * 6, Rings, 0, 0, 0);
    logOnce("Sea: drawn into the G-buffer, in place of the game's.");

    return true;
}

std::uint64_t keyOf(ID3D11DeviceContext* context)
{
    return reinterpret_cast<std::uint64_t>(context);
}

// The game's near sea draw's material constants into their copy, once between two presents: the frames
// recorded after it read them. The far sea's lie 5 m lower.
void copyGameSea(ID3D11DeviceContext* context, std::uint64_t presented)
{
    if (gameSeaCopiedAt.exchange(presented) == presented) return;

    ComPtr<ID3D11Buffer> material;
    context->VSGetConstantBuffers(GameMaterialSlot, 1, material.GetAddressOf());
    D3D11_BUFFER_DESC desc{};
    if (material) material->GetDesc(&desc);
    if (desc.ByteWidth != GameMaterialSize) return;

    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return;

    context->CopyResource(gameSeaMaterial.Get(), material.Get());
    notes.note(keyOf(context), [](Notes& contextNotes) { contextNotes.isGameSeaCopied = true; });
    logOnce("Sea: its level and wind come from the game's sea.");
}

// The sea's level for the CPU: the game's sea's copy, read back once a second, a present after its copy so the
// CPU never waits for the GPU. Only while the sea draws, so after a list with the game's sea's copy ran and the
// readback follows it on the GPU (read before, it found zeros: y 0, 23 m above the sea). Call with
// resourceMutex held, on the immediate context at present.
void readBackLevel()
{
    if (!immediate || !levelReadback) return;

    if (isLevelReadbackPending)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(immediate->Map(levelReadback.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) return;

        float material[2][4]{};
        std::memcpy(material, mapped.pData, sizeof(material));
        immediate->Unmap(levelReadback.Get(), 0);
        isLevelReadbackPending = false;
        if (!readLevel || *readLevel != material[0][2]) log(std::format("Sea: its level is y {:.2f}.", material[0][2]));
        readLevel = material[0][2];
        return;
    }

    const auto now = GetTickCount64();
    if (now - lastLevelRead < 1000) return;

    immediate->CopyResource(levelReadback.Get(), gameSeaMaterial.Get());
    isLevelReadbackPending = true;
    lastLevelRead = now;
}

// The next frame's waves: the time, the wind turning towards the weather's, the tiles' wavenumbers.
Constants constantsAfter(double elapsed, const Sea& sea)
{
    seconds += elapsed;
    const auto weather = weather::state();
    const float target = WindOf(weather.requested.value_or(weather::Type::Sunny)) * sea.waves;
    if (wind <= 0) wind = target;
    wind += (target - wind) * (1 - std::exp(-static_cast<float>(elapsed) / WindSeconds));

    Constants constants{};
    for (UINT cascade = 0; cascade < Cascades; ++cascade)
    {
        constants.cascadeSizes[cascade] = CascadeSizes[cascade];
        // Each tile from where the next one's waves stop; the smallest to just below its texels' limit.
        constants.cascadeLow[cascade] = cascade == 0 ? 1e-4f : 6 * 2 * Pi / CascadeSizes[cascade];
        constants.cascadeHigh[cascade] = cascade + 1 < Cascades ? 6 * 2 * Pi / CascadeSizes[cascade + 1] : 0.95f * Pi * Texels / CascadeSizes[cascade];
    }

    // JONSWAP for the wind and fetch. Past the fetch a wind fully develops the sea over, the sea grows no
    // more: there the peak is Pierson and Moskowitz's (0.877 g / U), and its boost Mitsuyasu's (1980). Offline,
    // the tiles' heights came within 15% of Pierson and Moskowitz's (0.89 m at 6 m/s, 4.7 m at 14 m/s).
    const float speed = std::max(wind, 0.5f);
    const float fetch = std::min(Fetch, 1.58e4f * speed * speed / Gravity);
    const float dimensionless = Gravity * fetch / (speed * speed);
    constants.alpha = 0.076f * std::pow(dimensionless, -0.22f);
    constants.peakFrequency = 22 * Gravity / speed * std::pow(dimensionless, -1.0f / 3);
    constants.peakBoost = std::max(7 * std::pow(dimensionless, -0.143f), 1.0f);
    // Wrapped well before float loses the fast ripples' phase: a wave's phase is its frequency times this.
    constants.time = static_cast<float>(std::fmod(seconds, 3600.0));
    constants.foamLeft = std::exp(-static_cast<float>(elapsed) / FoamSeconds);
    constants.choppiness = Choppiness;
    constants.gridSpacing = GridSpacing;
    constants.gridCells = GridCells;
    // Cox and Munk's slope variance for the wind (both axes: 0.003 + 0.00512 U) less the tiles' own, which
    // offline came to about 0.0155 + 0.00135 U: the ripples shorter than the smallest tile's waves.
    constants.microRoughness = std::max(0.00377f * speed - 0.0125f, 0.001f);
    const float waterAlbedo[]{GI_SEA_ALBEDO};
    std::copy(std::begin(waterAlbedo), std::end(waterAlbedo), constants.waterAlbedo);

    return constants;
}
}

void attach(ID3D11Device* gameDevice, ID3D11DeviceContext* gameImmediate) noexcept
{
    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    immediate = gameImmediate;
    if (latch.latest().isRunning) ensurePipeline();
}

void detach() noexcept
{
    isDrawing = false;
    notes.clear();
    std::lock_guard lock(resourceMutex);
    releasePipeline();
    device.Reset();
    immediate.Reset();
    hasPipelineFailed = hasDrawFailed = false;
}

void configure(const Configuration& configuration) noexcept
{
    latch.set({configuration.settings.sea, configuration.effects.runs(Effect::Sea)});
    if (!latch.latest().isRunning) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

Health health()
{
    if (!latch.latest().isRunning) return {};
    if (hasPipelineFailed || hasDrawFailed) return Health::unavailable("The sea is unavailable (see PhantomFX.log).");
    if (!isDrawing.load(std::memory_order_relaxed)) return Health::waiting("The sea shows where the game has one, around Mother Base.");

    return {};
}

void present() noexcept
{
    const auto settings = latch.present();
    isRunning = settings.isRunning;
    notes.present();

    const auto ticks = clock::ticksNow();
    const double elapsed = lastPresent ? std::clamp(clock::millisecondsOf(ticks - lastPresent) / 1000, 0.0, 0.1) : 0;
    lastPresent = ticks;

    {
        std::lock_guard lock(resourceMutex);
        isSeaInFrameBeingRun = false;

        if (settings.isRunning)
        {
            const auto now = GetTickCount64();
            if (now - lastGBuffer.load() > ForgottenAfterMilliseconds) lastGameSea = 0;
            const auto gameSea = lastGameSea.load();
            const bool isGameSeaShown = gameSea && now - gameSea < ShownForMilliseconds;
            frameConstants = constantsAfter(elapsed, settings.sea);
            isDrawing = isGameSeaShown && hasGameSeaCopy && surfacePixel && !hasDrawFailed;
            if (isDrawing) readBackLevel();
        }
        else
        {
            // The game's sea is seen again before the sea draws again: the targets bound while it was off were
            // not followed (targetsBound).
            isDrawing = false;
            lastGameSea = 0;
        }
    }

    ++presents;
}

void targetsBound(ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* depth) noexcept
{
    if (!isRunning.load(std::memory_order_relaxed)) return;

    try
    {
        bound = {context, isGBuffer(count, targets, depth), depth && targetCount(count, targets) == 1};
    }
    catch (...)
    {
        bound = {};
    }
}

void note(ID3D11DeviceContext* context, const ui_redirect::DrawCall&, game_shaders::DrawnShader& shader) noexcept
{
    if (!isRunning.load(std::memory_order_relaxed) || bound.context != context) return;

    try
    {
        const auto presented = presents.load(std::memory_order_relaxed);
        if (bound.isSingleTarget)
        {
            const auto* row = shader.row();
            if (!row || row->pass != GamePass::Sea) return;

            lastGameSea = GetTickCount64();
            if (!row->isFarSea) copyGameSea(context, presented);
            return;
        }

        // The main view's G-buffer: a smaller camera's view (the intro's mirror) draws into a corner of it.
        if (!bound.isGBuffer || gBufferSeenAt.load() == presented || !viewportFillsTarget(context)) return;
        if (gBufferSeenAt.exchange(presented) == presented) return;

        lastGBuffer = GetTickCount64();
        if (!isDrawing.load(std::memory_order_relaxed)) return;

        std::lock_guard lock(resourceMutex);
        if (!ensurePipeline() || !drawSea(context, frameConstants)) return;

        notes.note(keyOf(context), [](Notes& contextNotes) { contextNotes.isSeaDrawn = true; });
    }
    catch (const std::exception& error)
    {
        hasDrawFailed = true;
        logOnce(std::string("Sea: not drawn, the game's sea is back: ") + error.what());
    }
    catch (...)
    {
        hasDrawFailed = true;
        logOnce("Sea: not drawn, the game's sea is back.");
    }
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall&, game_shaders::DrawnShader& shader) noexcept
{
    if (!isDrawing.load(std::memory_order_relaxed) || bound.context != context || !bound.isSingleTarget) return false;

    const auto* row = shader.row();
    return row && row->pass == GamePass::Sea;
}

void recorded(std::uint64_t into, std::uint64_t from) noexcept
{
    notes.recorded(into, from);
}

void forget(std::uint64_t contextOrList) noexcept
{
    notes.forget(contextOrList);
}

void executing(ID3D11DeviceContext* context, std::uint64_t list) noexcept
{
    if (!isRunning.load(std::memory_order_relaxed)) return;

    const auto noted = notes.take(list);
    if (!noted) return;

    std::lock_guard lock(resourceMutex);
    if (context != immediate.Get()) return;

    isSeaInFrameBeingRun |= noted->isSeaDrawn;
    hasGameSeaCopy |= noted->isGameSeaCopied;
}

std::optional<float> level() noexcept
{
    std::lock_guard lock(resourceMutex);
    if (!isSeaInFrameBeingRun) return std::nullopt;

    return readLevel;
}

ComPtr<ID3D11ShaderResourceView> surfaceDepth(UINT width, UINT height) noexcept
{
    std::lock_guard lock(resourceMutex);
    if (!isSeaInFrameBeingRun || depthWidth != width || depthHeight != height) return nullptr;

    return depthView;
}
}
