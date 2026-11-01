// Glass and mirrors on Direct3D 11; glass.hpp describes them. What their draws share with the water - the surface
// targets, the trace and the composite - is forward_surfaces'.
#include "features/path_tracing/glass.hpp"

#include "d3d11_helpers.hpp"
#include "features/forward_shadows/forward_shadows.hpp"
#include "features/path_tracing/forward_surfaces.hpp"
#include "features/path_tracing/path_tracing.hpp"
#include "features/path_tracing/surface_shaders.hpp"
#include "log.hpp"

#include <exception>
#include <mutex>
#include <string>

namespace phantom_fx::glass
{
namespace
{
using Microsoft::WRL::ComPtr;
using game_shaders::GamePass;

std::mutex stateMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11PixelShader> glassShader, colorGlassShader, mirrorShader;
bool hasPipelineFailed{};

ComPtr<ID3D11PixelShader> surfaceShader(const char* source, const char* name, const D3D_SHADER_MACRO* defines = nullptr)
{
    return pixelShader(device.Get(), (std::string(surface_shaders::SurfaceCommon) + source).c_str(), name, defines);
}

// Compiles the shaders once; call with stateMutex held. Returns false when they are unavailable.
bool ensurePipeline()
{
    if (mirrorShader) return true;
    if (!device || hasPipelineFailed) return false;

    try
    {
        const D3D_SHADER_MACRO color[]{{"COLOR_TEXTURE", "1"}, {}};
        glassShader = surfaceShader(surface_shaders::GlassSurface, "glass-surface");
        colorGlassShader = surfaceShader(surface_shaders::GlassSurface, "glass-surface-color", color);
        mirrorShader = surfaceShader(surface_shaders::MirrorSurface, "mirror-surface");
        return true;
    }
    catch (const std::exception& error)
    {
        hasPipelineFailed = true;
        log(std::string("Glass keeps the game's reflections: ") + error.what());
        return false;
    }
}

// A mirror's texture holds the mirror camera's live picture where the game has one (the intro's: the shader reads it
// across the screen), which is a real reflection and stays; a painted picture is a loaded texture.
bool isLivePicture(ID3D11ShaderResourceView* picture)
{
    return (textureDesc(picture).BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    std::lock_guard lock(stateMutex);
    device = gameDevice;
}

void detach() noexcept
{
    std::lock_guard lock(stateMutex);
    glassShader.Reset();
    colorGlassShader.Reset();
    mirrorShader.Reset();
    device.Reset();
    hasPipelineFailed = false;
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    try
    {
        const auto* row = shader.row();
        if (!row || (row->pass != GamePass::Glass && row->pass != GamePass::Mirror) || !path_tracing::isReflecting()) return false;

        ComPtr<ID3D11PixelShader> surface;
        {
            std::lock_guard lock(stateMutex);
            if (!ensurePipeline()) return false;

            surface = row->pass == GamePass::Mirror ? mirrorShader : row->hasColorTexture ? colorGlassShader : glassShader;
        }

        // A smaller camera's view of the same targets (the intro's mirror) is no main view: its pixels are not the frame's.
        if (!viewportFillsTarget(context)) return false;
        // No targets at this frame's size: leave the game's reflection in place and let its draw run as it is.
        if (!forward_surfaces::accepts(context)) return false;

        ComPtr<ID3D11ShaderResourceView> reflected;
        context->PSGetShaderResources(row->reflectionSlot, 1, reflected.GetAddressOf());
        if (row->pass == GamePass::Mirror && isLivePicture(reflected.Get())) return false;

        // What the game reflects with leaves the draw. A view of nothing reads black: glass keeps its tint, its highlights, what
        // it covers and the fog over it, and the traced reflection takes the cube map's place at its weight.
        ID3D11ShaderResourceView* none{};
        context->PSSetShaderResources(row->reflectionSlot, 1, &none);
        // Its shadow map leaves too (t8): the rays' shadow takes its place at the composite (forward_shadows).
        constexpr UINT GlassShadowSlot = 8, ShadowSamplerSlot = 15;
        ID3D11ShaderResourceView* gameShadow{};
        ID3D11SamplerState* gameSampler{};
        ID3D11ShaderResourceView* litShadow{};
        ID3D11SamplerState* litSampler{};
        const bool shadowReplaced = forward_shadows::tracedShadowResources(context, &litShadow, &litSampler);
        if (shadowReplaced)
        {
            context->PSGetShaderResources(GlassShadowSlot, 1, &gameShadow);
            context->PSGetSamplers(ShadowSamplerSlot, 1, &gameSampler);
            context->PSSetShaderResources(GlassShadowSlot, 1, &litShadow);
            context->PSSetSamplers(ShadowSamplerSlot, 1, &litSampler);
        }
        context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
        context->PSSetShaderResources(row->reflectionSlot, 1, reflected.GetAddressOf());
        if (shadowReplaced)
        {
            context->PSSetShaderResources(GlassShadowSlot, 1, &gameShadow);
            context->PSSetSamplers(ShadowSamplerSlot, 1, &gameSampler);
            if (gameShadow) gameShadow->Release();
            if (gameSampler) gameSampler->Release();
        }

        // The surface's own draw, into the targets the composite reads.
        forward_surfaces::record(context, call, surface.Get());

        return true;
    }
    catch (...)
    {
        return false;
    }
}
}
