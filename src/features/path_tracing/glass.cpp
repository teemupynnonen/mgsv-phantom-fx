// Glass and mirrors on Direct3D 11; glass.hpp describes them. What their draws share with the water - the surface
// targets, the trace and the composite - is forward_surfaces'.
#include "features/path_tracing/glass.hpp"

#include "d3d11_helpers.hpp"
#include "features/path_tracing/forward_surfaces.hpp"
#include "features/path_tracing/path_tracing.hpp"

namespace phantom_fx::glass
{
namespace
{
using Microsoft::WRL::ComPtr;
using game_shaders::GamePass;

// A mirror's texture holds the mirror camera's live picture where the game has one (the intro's: the shader reads it
// across the screen), which is a real reflection and stays; a painted picture is a loaded texture.
bool isLivePicture(ID3D11ShaderResourceView* picture)
{
    return (textureDesc(picture).BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
}
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    try
    {
        const auto* row = shader.row();
        if (!row || (row->pass != GamePass::Glass && row->pass != GamePass::Mirror) || !path_tracing::isReflecting()) return false;

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
        context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
        context->PSSetShaderResources(row->reflectionSlot, 1, reflected.GetAddressOf());

        // The surface's own draw, into the targets the composite reads.
        forward_surfaces::record(context, call, *row);

        return true;
    }
    catch (...)
    {
        return false;
    }
}
}
