// The light's passes; gi_light.hpp describes them, gi_light.hlsl holds them.
#include "features/global_illumination/gi_light.hpp"

#include <algorithm>
#include <cmath>

namespace phantom_fx::gi_light
{
namespace
{
using gi_d3d12::groups;
using gi_d3d12::Kernel;

constexpr std::uint32_t HashTiles = HashBuckets * HashTilesPerBucket;
constexpr std::uint32_t CellsPerTile = HashTileCellRatio * HashTileCellRatio + 16 + 4 + 1; // Mips 0 to 3
constexpr std::uint32_t HashCells = HashTiles * CellsPerTile;
// Capsaicin's defaults: cells of 32 pixels on screen, no smaller than 10 cm, 16 samples a cell, and
// 70% of the multibounce rays left out.
constexpr float HashCellPixels = 32, HashMinCellSize = 0.1f, MaxSamples = 16, DiscardMultibounce = 0.7f;
// Hits a cell takes its light from each frame (gi_light.hlsl, HashGridCache_QueueLight).
constexpr std::uint32_t CellSamplesPerFrame = 4;
}

Light::Light(ID3D12Device* device, UINT width, UINT height, bool isPerRenderPixel)
    : renderWidth(width), renderHeight(height), lightWidth(isPerRenderPixel ? width : (width + 1) / 2),
      lightHeight(isPerRenderPixel ? height : (height + 1) / 2), rayCount(lightWidth * lightHeight)
{
    if (!isPerRenderPixel) denoiser.emplace(device, lightWidth, lightHeight);

    const auto texture = [&](DXGI_FORMAT format, const char* what) {
        return gi_d3d12::createTexture(device, lightWidth, lightHeight, format, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, what);
    };
    // Only the denoiser and the light grid's render pixel picks read these: a texel each for Ray Reconstruction.
    const auto denoiserTexture = [&](DXGI_FORMAT format, const char* what) {
        return isPerRenderPixel ? gi_d3d12::createTexture(device, 1, 1, format, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, what) : texture(format, what);
    };
    const auto buffer = [&](std::uint64_t bytes, const char* what) { return gi_d3d12::createStorage(device, bytes, what); };

    normalRoughness = denoiserTexture(DXGI_FORMAT_R10G10B10A2_UNORM, "Creating the light's normals");
    motion = denoiserTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the light's motion");
    for (int i = 0; i < 2; ++i)
    {
        viewZ[i] = denoiserTexture(DXGI_FORMAT_R32_FLOAT, "Creating the light's view depth");
        noisy[i] = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the light's rays");
        denoised[i] = denoiserTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the denoised light");
        packedTiles[i] = buffer(std::uint64_t{HashTiles} * 4, "Creating the packed tiles");
    }
    partial = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the rays' own light");
    path = texture(DXGI_FORMAT_R32G32B32A32_UINT, "Creating the rays' bounces");
    pick = denoiserTexture(DXGI_FORMAT_R8_UINT, "Creating the light's samples");
    validation = denoiserTexture(DXGI_FORMAT_R8G8B8A8_UNORM, "Creating the denoiser's validation layer");

    hash = buffer(std::uint64_t{HashTiles} * 4, "Creating the hash grid");
    decay = buffer(std::uint64_t{HashTiles} * 4, "Creating the hash grid's ages");
    values = buffer(std::uint64_t{HashCells} * 8, "Creating the hash grid's light");
    indirectValues = buffer(std::uint64_t{HashCells} * 8, "Creating the hash grid's indirect light");
    updateTiles = buffer(std::uint64_t{HashTiles} * 4, "Creating the updated tiles");
    updateValues = buffer(std::uint64_t{HashCells} * 16, "Creating the hash grid's new light");
    updateIndirectValues = buffer(std::uint64_t{HashCells} * 16, "Creating the hash grid's new indirect light");
    // Second hits (a ray's bounce) first, then third hits (their cells' multibounce rays).
    visibility = buffer(std::uint64_t{rayCount} * 2 * 16, "Creating the hits");
    visibilityCells = buffer(std::uint64_t{rayCount} * 2 * 4, "Creating the hits' cells");
    visibilityQueries = buffer(std::uint64_t{rayCount} * 2 * 4, "Creating the hits' rays");
    multibounce = buffer(std::uint64_t{rayCount} * 8, "Creating the multibounce weights");
    lights = buffer(std::uint64_t{rayCount} * 2 * 4, "Creating the hits to light");
    foliageKeys = buffer(std::uint64_t{GI_FOLIAGE_MEMORY_SLOTS} * 4, "Creating the foliage memory's keys");
    foliageTouched = buffer(std::uint64_t{GI_FOLIAGE_MEMORY_SLOTS} * 4, "Creating the foliage memory's ages");
    foliageAdded = buffer(std::uint64_t{GI_FOLIAGE_MEMORY_SLOTS} * 16, "Creating the foliage memory's new light");
    foliageLight = buffer(std::uint64_t{GI_FOLIAGE_MEMORY_SLOTS} * 8, "Creating the foliage memory's light");
}

void Light::fill(gi::GIFrameConstants& constants, float verticalFov, std::uint32_t parity) const
{
    const float width = static_cast<float>(renderWidth), height = static_cast<float>(renderHeight);
    const float scale = std::max(1.0f / height, height / (width * width));
    const float hashCell = std::tan(verticalFov * HashCellPixels * scale);
    constants.lightLayout = {lightWidth, lightHeight, rayCount, parity};
    constants.cellSizes = {0, hashCell, HashMinCellSize, hashCell * HashTileCellRatio};
    constants.hashCounts = {HashBuckets, HashTiles, HashCells, HashTilesPerBucket};
    constants.hashTileSizes = {HashTileCellRatio, HashTileCellRatio / 2, HashTileCellRatio / 4, HashTileCellRatio / 8};
    constants.hashTileCells = {64, 16, 4, 1};
    constants.hashTileFirstCells = {0, 64, 80, 84};
    constants.hashMisc = {CellsPerTile, parity, CellSamplesPerFrame, 0};
    constants.hashSamples = {MaxSamples, DiscardMultibounce, MaxSamples, static_cast<float>(HashTileCellRatio)};
}

void Light::describe(gi_d3d12::Descriptors& d, std::uint32_t parity) const
{
    const auto now = parity, last = 1 - parity;
    d.outputTexture(binding::Light_ViewZ, viewZ[now].Get(), DXGI_FORMAT_R32_FLOAT);
    d.outputTexture(binding::Light_NormalRoughness, normalRoughness.Get(), DXGI_FORMAT_R10G10B10A2_UNORM);
    d.outputTexture(binding::Light_Motion, motion.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    d.outputTexture(binding::Light_Noisy.at(0), noisy[0].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    d.outputTexture(binding::Light_Noisy.at(1), noisy[1].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    d.outputTexture(binding::Light_Denoised.at(0), denoised[0].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    d.outputTexture(binding::Light_Denoised.at(1), denoised[1].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    d.outputTexture(binding::Light_Partial, partial.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    d.outputTexture(binding::Light_Path, path.Get(), DXGI_FORMAT_R32G32B32A32_UINT);
    d.outputBuffer(binding::HashGridCache_HashBuffer, hash.Get(), 4, HashTiles);
    d.outputBuffer(binding::HashGridCache_DecayTileBuffer, decay.Get(), 4, HashTiles);
    d.outputBuffer(binding::HashGridCache_ValueBuffer, values.Get(), 8, HashCells);
    d.outputBuffer(binding::HashGridCache_ValueIndirectBuffer, indirectValues.Get(), 8, HashCells);
    d.outputBuffer(binding::HashGridCache_UpdateTileBuffer, updateTiles.Get(), 4, HashTiles);
    d.outputBuffer(binding::HashGridCache_UpdateCellValueBuffer, updateValues.Get(), 4, std::uint64_t{HashCells} * 4);
    d.outputBuffer(binding::HashGridCache_UpdateCellValueIndirectBuffer, updateIndirectValues.Get(), 4, std::uint64_t{HashCells} * 4);
    d.outputBuffer(binding::HashGridCache_VisibilityBuffer, visibility.Get(), 16, std::uint64_t{rayCount} * 2);
    d.outputBuffer(binding::HashGridCache_VisibilityCellBuffer, visibilityCells.Get(), 4, std::uint64_t{rayCount} * 2);
    d.outputBuffer(binding::HashGridCache_VisibilityQueryBuffer, visibilityQueries.Get(), 4, std::uint64_t{rayCount} * 2);
    d.outputBuffer(binding::HashGridCache_MultibounceInfoBuffer, multibounce.Get(), 8, rayCount);
    d.outputBuffer(binding::HashGridCache_PackedTileIndexBuffer, packedTiles[now].Get(), 4, HashTiles);
    d.outputBuffer(binding::HashGridCache_PreviousPackedTileIndexBuffer, packedTiles[last].Get(), 4, HashTiles);
    d.outputBuffer(binding::HashGridCache_LightBuffer, lights.Get(), 4, std::uint64_t{rayCount} * 2);
    d.outputTexture(binding::Light_LastViewZ, viewZ[last].Get(), DXGI_FORMAT_R32_FLOAT);
    d.outputBuffer(binding::FoliageMemory_Keys, foliageKeys.Get(), 4, GI_FOLIAGE_MEMORY_SLOTS);
    d.outputBuffer(binding::FoliageMemory_Touched, foliageTouched.Get(), 4, GI_FOLIAGE_MEMORY_SLOTS);
    d.outputBuffer(binding::FoliageMemory_Added, foliageAdded.Get(), 4, std::uint64_t{GI_FOLIAGE_MEMORY_SLOTS} * 4);
    d.outputBuffer(binding::FoliageMemory_Light, foliageLight.Get(), 8, GI_FOLIAGE_MEMORY_SLOTS);
    d.outputTexture(binding::Light_Pick, pick.Get(), DXGI_FORMAT_R8_UINT);
    d.outputTexture(binding::Light_Validation, validation.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
}

void Light::record(gi_d3d12::Recorder& r, std::uint32_t parity, const gi_denoiser::Camera& camera, std::uint32_t frameSlot,
    bool isValidationShown)
{
    const auto now = std::chrono::steady_clock::now();
    if (lastRecord)
    {
        // NRD's low-pass filter, a fifth of a second; pauses (menus, loading) are left out.
        const double frame = std::chrono::duration<double, std::milli>(now - *lastRecord).count();
        const double samples = 0.2 * 1000.0 / smoothedFrameMilliseconds;
        if (frame < 250.0) smoothedFrameMilliseconds += (frame - smoothedFrameMilliseconds) / (1.0 + samples);
    }
    lastRecord = now;

    const auto lightPixels = [&](Kernel kernel) {
        r.dispatch(kernel, groups(lightWidth, 8), groups(lightHeight, 8));
        r.barrier();
    };

    r.mark("guides", performance::Pass::GiRays);
    r.dispatch(Kernel::ClearCounters, 1);
    r.barrier();
    // Tiles nothing touched for 50 frames leave the cache.
    r.indirect(Kernel::PurgeTiles, GI_COUNTER_PACKED_TILES + 1 - parity, 64);
    // The denoiser's guides, and which render pixel of its 2 x 2 a light pixel samples: none for Ray Reconstruction.
    if (denoiser) lightPixels(Kernel::PrepareGuides);

    r.mark("rays and their hits' light", performance::Pass::GiRays);
    lightPixels(Kernel::TraceLight);
    r.dispatch(Kernel::LearnAlbedo, groups(std::uint64_t{groups(lightWidth, 16)} * groups(lightHeight, 16), 64));
    r.barrier();

    r.mark("cache: multibounce rays", performance::Pass::GiRays);
    r.indirect(Kernel::PopulateMultibounceCells, GI_COUNTER_VISIBILITY0, 32);
    r.mark("cache: hit lighting (shadow rays)", performance::Pass::GiRays);
    r.indirect(Kernel::PopulateCells, GI_COUNTER_LIGHTS, 32);
    r.mark("cache update", performance::Pass::GiRays);
    r.indirect(Kernel::UpdateTiles, GI_COUNTER_UPDATE_TILES, 1);
    r.indirect(Kernel::UpdateMultibounceCells, GI_COUNTER_VISIBILITY1, 32);
    lightPixels(Kernel::ResolveLight);
    // The foliage memory takes the frame's light; its empty slots return at once.
    r.dispatch(Kernel::UpdateFoliageMemory, groups(GI_FOLIAGE_MEMORY_SLOTS, 64));
    r.barrier();

    if (denoiser)
    {
        r.mark("denoiser", performance::Pass::GiDenoiser);
        denoiser->record(r.commands(), camera, {motion.Get(), normalRoughness.Get(), viewZ[parity].Get(), noisy[0].Get(), noisy[1].Get(),
            denoised[0].Get(), denoised[1].Get(), isValidationShown ? validation.Get() : nullptr}, frameSlot,
            static_cast<float>(1000.0 / smoothedFrameMilliseconds));
        r.rebind();
    }

    r.mark("render size", performance::Pass::GiDenoiser);
    r.dispatch(Kernel::CompositeLight, groups(renderWidth, 8), groups(renderHeight, 8));
    r.barrier();
}
}
