#pragma once

#include "passes/ray_tracing/gi_d3d12.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"

#include <d3d12.h>
#include <cstdint>

// The sun's ray-traced shadows on the global illumination's Direct3D 12 device (docs/DEVELOPMENT.md,
// "Ray-traced shadows"): a shadow ray a pixel through the scene, then AMD's FidelityFX Shadow
// Denoiser, with their memory at one render size (gi_shadow.hlsl, gi_shadow_filter.hlsl). For DLSS Ray
// Reconstruction, which denoises itself, the rays alone.
namespace phantom_fx::gi_shadow
{
class Shadows
{
public:
    // Makes the memory for a render size. Throws on Direct3D 12 errors.
    Shadows(ID3D12Device* device, UINT width, UINT height);

    UINT width() const { return renderWidth; }
    UINT height() const { return renderHeight; }

    // Marks a fresh start, which has no history to blend.
    void fill(gi::GIFrameConstants& constants) const;
    // Its views in the frame's tables; `output` is the shared shadow, in the unordered access
    // state while the passes run.
    void describe(gi_d3d12::Descriptors& descriptors, std::uint32_t parity, ID3D12Resource* output) const;
    // The rays, then the denoiser's passes, into the output; the rays alone `forReconstruction`. Needs the
    // frame's scene built.
    void record(gi_d3d12::Recorder& recorder, bool forReconstruction);

private:
    UINT renderWidth, renderHeight;
    bool fresh{true};

    gi_d3d12::ComPtr<ID3D12Resource> masks, metadata, reprojection, history, moments[2], depths[2];
};
}
