// Rivers and other liquids on Direct3D 11; water.hpp describes them. What their draws share with glass and mirrors - the
// surface targets, the trace and the composite - is forward_surfaces'.
#include "features/path_tracing/water.hpp"

#include "d3d11_helpers.hpp"
#include "features/path_tracing/forward_surfaces.hpp"
#include "features/path_tracing/path_tracing.hpp"
#include "features/path_tracing/surface_shaders.hpp"
#include "log.hpp"

#include <exception>
#include <mutex>
#include <string>

namespace phantom_fx::water
{
namespace
{
using Microsoft::WRL::ComPtr;

std::mutex stateMutex;
ComPtr<ID3D11Device> device;
// WaterSurface, by how the river builds its normal: [isLayeredRiver][hasTwoChannelNormal] (game_shaders.hpp).
ComPtr<ID3D11PixelShader> waterShaders[2][2];
ComPtr<ID3D11Buffer> waterParameters; // The water's roughness, for its surface shader (the menu's)
bool hasPipelineFailed{};

// Compiles the shaders once; call with stateMutex held. Returns false when they are unavailable.
bool ensurePipeline()
{
    if (waterShaders[1][1]) return true;
    if (!device || hasPipelineFailed) return false;

    try
    {
        waterParameters = makeConstantBuffer(device.Get(), 16, "Creating the water's constants");
        const auto source = std::string(surface_shaders::SurfaceCommon) + surface_shaders::WaterSurface;
        for (int layered = 0; layered < 2; ++layered)
            for (int twoChannel = 0; twoChannel < 2; ++twoChannel)
            {
                const D3D_SHADER_MACRO defines[]{{"LAYERED", layered ? "1" : "0"}, {"TWO_CHANNEL", twoChannel ? "1" : "0"}, {}};
                // Last, [1][1], so it marks a complete pipeline.
                waterShaders[layered][twoChannel] = pixelShader(device.Get(), source.c_str(), "water-surface", defines);
            }
        return true;
    }
    catch (const std::exception& error)
    {
        hasPipelineFailed = true;
        log(std::string("Water keeps the game's reflections: ") + error.what());
        return false;
    }
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
    for (auto& shaders : waterShaders)
        for (auto& shader : shaders) shader.Reset();
    waterParameters.Reset();
    device.Reset();
    hasPipelineFailed = false;
}

void recordWater(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, const game_shaders::GameShader& row) noexcept
{
    try
    {
        ComPtr<ID3D11PixelShader> surface;
        ComPtr<ID3D11Buffer> parameters;
        {
            std::lock_guard lock(stateMutex);
            if (!ensurePipeline()) return;

            surface = waterShaders[row.isLayeredRiver ? 1 : 0][row.hasTwoChannelNormal ? 1 : 0];
            parameters = waterParameters;
        }

        if (!viewportFillsTarget(context)) return;

        // The menu's roughness, for the surface shader (WaterSurface): the water's traced reflection spreads by it.
        const float roughness[4]{path_tracing::waterReflectionRoughness(), 0.0f, 0.0f, 0.0f};
        check(write(context, parameters.Get(), roughness), "Writing the water's constants");
        context->PSSetConstantBuffers(0, 1, parameters.GetAddressOf());

        forward_surfaces::record(context, call, surface.Get());
    }
    catch (...)
    {
    }
}
}
