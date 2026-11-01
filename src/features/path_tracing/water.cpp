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
ComPtr<ID3D11PixelShader> waterShader;
ComPtr<ID3D11Buffer> waterParameters; // The water's roughness, for its surface shader (the menu's)
bool hasPipelineFailed{};

// Compiles the shader once; call with stateMutex held. Returns false when it is unavailable.
bool ensurePipeline()
{
    if (waterShader) return true;
    if (!device || hasPipelineFailed) return false;

    try
    {
        waterShader = pixelShader(device.Get(), (std::string(surface_shaders::SurfaceCommon) + surface_shaders::WaterSurface).c_str(), "water-surface");
        waterParameters = makeConstantBuffer(device.Get(), 16, "Creating the water's constants");
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
    waterShader.Reset();
    waterParameters.Reset();
    device.Reset();
    hasPipelineFailed = false;
}

void recordWater(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept
{
    try
    {
        ComPtr<ID3D11PixelShader> surface;
        ComPtr<ID3D11Buffer> parameters;
        {
            std::lock_guard lock(stateMutex);
            if (!ensurePipeline()) return;

            surface = waterShader;
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
