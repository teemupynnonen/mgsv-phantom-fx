// The sun's ray-traced shadows; gi_shadow.hpp describes them. The passes and their memory follow AMD's
// FidelityFX Shadow Denoiser (FidelityFX SDK 1.1.4, ffx_denoiser.cpp).
#include "features/ray_traced_shadows/gi_shadow.hpp"

namespace phantom_fx::gi_shadow
{
namespace
{
using gi_d3d12::groups;
using gi_d3d12::Kernel;

// The denoiser's tiles: the ray masks' 8x4 pixels, the tile classification's 8x8.
constexpr UINT MaskTileWidth = 8, MaskTileHeight = 4, Tile = 8;
}

Shadows::Shadows(ID3D12Device* device, UINT width, UINT height) : renderWidth(width), renderHeight(height)
{
    const auto texture = [&](DXGI_FORMAT format, const char* what) {
        return gi_d3d12::createTexture(device, width, height, format, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, what);
    };
    const std::uint64_t maskTiles = std::uint64_t{groups(width, MaskTileWidth)} * groups(height, MaskTileHeight);
    const std::uint64_t tiles = std::uint64_t{groups(width, Tile)} * groups(height, Tile);

    masks = gi_d3d12::createStorage(device, maskTiles * 4, "Creating the shadow rays' masks");
    metadata = gi_d3d12::createStorage(device, tiles * 4, "Creating the shadow tiles");
    reprojection = texture(DXGI_FORMAT_R16G16_FLOAT, "Creating the shadow denoiser's reprojection");
    history = texture(DXGI_FORMAT_R16G16_FLOAT, "Creating the shadow history");
    for (int i = 0; i < 2; ++i)
    {
        moments[i] = texture(DXGI_FORMAT_R11G11B10_FLOAT, "Creating the shadow moments");
        depths[i] = texture(DXGI_FORMAT_R32_FLOAT, "Creating the shadow depth");
    }
}

void Shadows::fill(gi::GIFrameConstants& constants) const
{
    constants.shadowParams.w = fresh ? 1.0f : 0.0f;
}

void Shadows::describe(gi_d3d12::Descriptors& d, std::uint32_t parity, ID3D12Resource* output) const
{
    const auto now = parity, last = 1 - parity;
    const auto maskTiles = std::uint64_t{groups(renderWidth, MaskTileWidth)} * groups(renderHeight, MaskTileHeight);
    d.outputBuffer(binding::Shadow_Masks, masks.Get(), 4, maskTiles);
    d.outputBuffer(binding::Shadow_TileMetadata, metadata.Get(), 4, std::uint64_t{groups(renderWidth, Tile)} * groups(renderHeight, Tile));
    d.outputTexture(binding::Shadow_Reprojection, reprojection.Get(), DXGI_FORMAT_R16G16_FLOAT);
    d.outputTexture(binding::Shadow_History, history.Get(), DXGI_FORMAT_R16G16_FLOAT);
    d.outputTexture(binding::Shadow_Moments.at(0), moments[now].Get(), DXGI_FORMAT_R11G11B10_FLOAT);
    d.outputTexture(binding::Shadow_Moments.at(1), moments[last].Get(), DXGI_FORMAT_R11G11B10_FLOAT);
    d.outputTexture(binding::Shadow_Depths.at(0), depths[now].Get(), DXGI_FORMAT_R32_FLOAT);
    d.outputTexture(binding::Shadow_Depths.at(1), depths[last].Get(), DXGI_FORMAT_R32_FLOAT);
    d.outputTexture(binding::Shadow_Output, output, DXGI_FORMAT_R16_FLOAT);
}

void Shadows::record(gi_d3d12::Recorder& r, bool forReconstruction)
{
    r.mark("shadow rays", performance::Pass::SunShadowRays);
    r.dispatch(Kernel::TraceSunShadows, groups(renderWidth, MaskTileWidth), groups(renderHeight, MaskTileHeight));
    r.barrier();
    // The rays wrote the output; the denoiser's history stops here, and starts over after.
    if (forReconstruction)
    {
        fresh = true;
        return;
    }

    // The temporal pass, then three spatial passes 1, 2 and 4 pixels apart.
    r.mark("shadow denoiser", performance::Pass::SunShadowDenoiser);
    r.dispatch(Kernel::ClassifyShadowTiles, groups(renderWidth, Tile), groups(renderHeight, Tile));
    r.barrier();
    for (std::uint32_t pass = 0; pass < 3; ++pass)
    {
        r.dispatch(Kernel::FilterShadows, groups(renderWidth, Tile), groups(renderHeight, Tile), {pass, 1u << pass});
        r.barrier();
    }

    fresh = false;
}
}
