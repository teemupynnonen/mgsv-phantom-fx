#include "passes/forward_water_pass.hpp"

#include "d3d11_helpers.hpp"
#include "features/path_tracing/forward_surfaces.hpp"
#include "features/path_tracing/water.hpp"
#include "features/water_caustics/water_caustics.hpp"

namespace phantom_fx::forward_water_pass
{
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    try
    {
        const auto* row = shader.row();
        if (!row || row->pass != game_shaders::GamePass::ForwardWater) return false;

        // A smaller camera's view of the same targets (the intro's mirror) is no main view: its pixels are not the frame's.
        if (!viewportFillsTarget(context)) return false;

        const bool isReflecting = water::drawRiver(context, call);
        // An overlay lies on the river under it, whose caustics show through.
        if (!row->isRiverOverlay) water_caustics::draw(context, call);
        if (isReflecting) forward_surfaces::record(context, call, *row);

        return true;
    }
    catch (...)
    {
        return false;
    }
}
}
