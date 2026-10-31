#pragma once

#include "features/global_illumination/gi_denoiser.hpp"
#include "passes/ray_tracing/gi_d3d12.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"

#include <d3d12.h>
#include <chrono>
#include <cstdint>
#include <optional>

// The global illumination's indirect light on its Direct3D 12 device (docs/DEVELOPMENT.md, "Global
// illumination", "Light"): a ray a pixel of a grid half the render size each way, its hits lit afresh,
// its bounce from the two-level hash grid cache, NRD's denoiser, then the render size
// (gi_light.hlsl), with their memory at one render size. For DLSS Ray Reconstruction, which denoises
// itself, a sample a render pixel and no denoiser.
namespace phantom_fx::gi_light
{
class Light
{
public:
    // Makes the memory for a render size, with a sample a render pixel for Ray Reconstruction
    // (`isPerRenderPixel`). Throws on Direct3D 12 or NRD errors.
    Light(ID3D12Device* device, UINT width, UINT height, bool isPerRenderPixel);

    UINT width() const { return renderWidth; }
    UINT height() const { return renderHeight; }
    bool isPerRenderPixel() const { return !denoiser; }

    // The light's and the hash grid's constants; `verticalFov` in radians.
    void fill(gi::GIFrameConstants& constants, float verticalFov, std::uint32_t parity) const;
    // Its views in the frame's tables.
    void describe(gi_d3d12::Descriptors& descriptors, std::uint32_t parity) const;
    // The passes, from the rays to the light in the output (u0), on the frame slot's room (`frameSlot`), with
    // NRD's validation layer when `isValidationShown`. The denoiser binds its own root signature and descriptor
    // heap; the recorder's rebind restores the frame's.
    void record(gi_d3d12::Recorder& recorder, std::uint32_t parity, const gi_denoiser::Camera& camera, std::uint32_t frameSlot,
        bool isValidationShown);

private:
    UINT renderWidth, renderHeight, lightWidth, lightHeight;
    std::uint32_t rayCount;
    // None for Ray Reconstruction.
    std::optional<gi_denoiser::Denoiser> denoiser;
    // The real frame rate, smoothed as NRD asks (NRDSettings.h, GetMaxAccumulatedFrameNum): its history is
    // half a second whatever the rate.
    double smoothedFrameMilliseconds{1000.0 / 60.0};
    std::optional<std::chrono::steady_clock::time_point> lastRecord;

    // The view depth this frame's and the last's by parity: a moving mesh's depth change takes the last (PrepareGuides).
    gi_d3d12::ComPtr<ID3D12Resource> viewZ[2], normalRoughness, motion, noisy[2], denoised[2], partial, path;
    // Which render pixel of its 2 x 2 each light pixel's sample is from (PrepareGuides), and NRD's validation layer.
    gi_d3d12::ComPtr<ID3D12Resource> pick, validation;
    gi_d3d12::ComPtr<ID3D12Resource> hash, decay, values, indirectValues, updateTiles, updateValues, updateIndirectValues, visibility,
        visibilityCells, visibilityQueries, multibounce, lights, packedTiles[2];
    // The foliage memory (gi_light.hlsl, "Foliage's light by world position").
    gi_d3d12::ComPtr<ID3D12Resource> foliageKeys, foliageTouched, foliageAdded, foliageLight;
};

// The hash grid's size: 2^12 buckets of 16 tiles, tiles of 8 x 8 cells and three smaller mips.
constexpr std::uint32_t HashBuckets = 1u << 12, HashTilesPerBucket = 1u << 4, HashTileCellRatio = 8;
}
