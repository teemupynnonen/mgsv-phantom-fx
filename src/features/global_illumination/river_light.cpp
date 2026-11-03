#include "features/global_illumination/river_light.hpp"

#include "d3d11_helpers.hpp"
#include "features/global_illumination/light_combine.hpp"
#include "features/global_illumination/river_light_shaders.hpp"
#include "hresult.hpp"
#include "log.hpp"

#include <d3d11_1.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

namespace phantom_fx::river_light
{
namespace
{
using Microsoft::WRL::ComPtr;

// The river shaders' cPSLight, whose first two rows are the hemisphere ambient's factors (GetHemisphereAmbientLightFactor0 and 1).
constexpr UINT LightSlot = 3, AmbientBytes = 32;
// Slots none of the river shaders binds (forward_scene.hpp), put back after.
constexpr UINT ParametersSlot = 6, FirstViewSlot = 7, ViewCount = 4;
constexpr UINT MaxConstantBytes = D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16;

struct Parameters
{
    float toLightUv[2], lightTexel[2];
    float strength, padding[3];
};

std::atomic<bool> enabled{true};

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11PixelShader> shader;
ComPtr<ID3D11BlendState> replace, removeFromScene;
ComPtr<ID3D11DepthStencilState> depthTest;
ComPtr<ID3D11Buffer> parameters;
// The river's cPSLight with the ambient's rows left zero, at the size of the range the river binds.
ComPtr<ID3D11Buffer> withoutAmbient;
UINT withoutAmbientBytes{};
DrawTarget lit, unlit;
bool hasPipelineFailed{};

// Compiles the shader and makes the states once; call with resourceMutex held.
bool ensurePipeline()
{
    if (shader) return true;
    if (!device || hasPipelineFailed) return false;

    try
    {
        parameters = makeConstantBuffer(device.Get(), sizeof(Parameters), "Creating the river light's constants");

        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        check(device->CreateBlendState(&blend, replace.ReleaseAndGetAddressOf()), "Creating the river light's blends");

        auto& target = blend.RenderTarget[0];
        target.BlendEnable = TRUE;
        // scene - the shader's: what the ambient adds over the traced light, taken back off.
        target.SrcBlend = D3D11_BLEND_ONE;
        target.DestBlend = D3D11_BLEND_ONE;
        target.BlendOp = D3D11_BLEND_OP_REV_SUBTRACT;
        target.SrcBlendAlpha = D3D11_BLEND_ZERO;
        target.DestBlendAlpha = D3D11_BLEND_ONE;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        check(device->CreateBlendState(&blend, removeFromScene.ReleaseAndGetAddressOf()), "Creating the river light's blends");

        // As the river is tested, reversed depth, without writing.
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        check(device->CreateDepthStencilState(&depth, depthTest.ReleaseAndGetAddressOf()), "Creating the river light's depth test");

        // Last, so it marks a complete pipeline.
        shader = pixelShader(device.Get(), shaders::Remove, "river-light");

        return true;
    }
    catch (const std::exception& error)
    {
        hasPipelineFailed = true;
        log(std::string("The rivers keep the game's ambient light: ") + error.what());

        return false;
    }
}

void ensureWithoutAmbient(UINT bytes)
{
    if (withoutAmbient && withoutAmbientBytes == bytes) return;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const std::vector<std::uint8_t> zeros(bytes);
    const D3D11_SUBRESOURCE_DATA data{zeros.data(), 0, 0};
    check(device->CreateBuffer(&desc, &data, withoutAmbient.ReleaseAndGetAddressOf()), "Creating the river's light without its ambient");
    withoutAmbientBytes = bytes;
}

// The range of cPSLight the river binds: its first byte and its size.
struct BoundRange
{
    ComPtr<ID3D11Buffer> buffer;
    UINT first{}, count{};
    UINT start{}, bytes{};
    bool hasOffsets{};
};

BoundRange boundLight(ID3D11DeviceContext* context)
{
    BoundRange range;
    ComPtr<ID3D11DeviceContext1> context1;
    if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context1))))
    {
        context1->PSGetConstantBuffers1(LightSlot, 1, range.buffer.GetAddressOf(), &range.first, &range.count);
        range.hasOffsets = true;
    }
    else context->PSGetConstantBuffers(LightSlot, 1, range.buffer.GetAddressOf());
    if (!range.buffer) return range;

    D3D11_BUFFER_DESC desc{};
    range.buffer->GetDesc(&desc);
    range.start = range.first * 16;
    const UINT end = range.count ? std::min(desc.ByteWidth, range.start + range.count * 16) : desc.ByteWidth;
    range.bytes = end > range.start ? std::min(end - range.start, MaxConstantBytes) : 0;

    return range;
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    std::lock_guard lock(resourceMutex);
    device = gameDevice;
}

void detach() noexcept
{
    std::lock_guard lock(resourceMutex);
    shader.Reset();
    replace.Reset();
    removeFromScene.Reset();
    depthTest.Reset();
    parameters.Reset();
    withoutAmbient.Reset();
    withoutAmbientBytes = 0;
    lit = {};
    unlit = {};
    device.Reset();
    hasPipelineFailed = false;
}

void setEnabled(bool isEnabled) noexcept
{
    enabled.store(isEnabled, std::memory_order_relaxed);
}

void draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept
{
    try
    {
        if (!enabled.load(std::memory_order_relaxed)) return;

        const auto ambient = light_combine::ambient();
        if (!ambient) return;

        ID3D11RenderTargetView* gameTargets[2]{};
        ComPtr<ID3D11DepthStencilView> gameDepthView;
        context->OMGetRenderTargets(2, gameTargets, gameDepthView.GetAddressOf());
        ComPtr<ID3D11RenderTargetView> heldTargets[2];
        for (int i = 0; i < 2; ++i) heldTargets[i].Attach(gameTargets[i]);
        const auto [width, height] = sizeOf(gameTargets[0]);
        if (!width) return;

        const auto light = boundLight(context);
        if (!light.buffer || light.bytes <= AmbientBytes || light.bytes % 16) return;

        // Held through the draws: the two aside share their targets between the recording threads.
        std::lock_guard lock(resourceMutex);
        if (!ensurePipeline()) return;
        ensureDrawTarget(device.Get(), lit, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the river with its ambient");
        ensureDrawTarget(device.Get(), unlit, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the river without its ambient");
        ensureWithoutAmbient(light.bytes);

        const auto [lightWidth, lightHeight] = sizeOf(ambient->traced.Get());
        if (!lightWidth) return;
        const Parameters values{{1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height)},
            {1.0f / static_cast<float>(lightWidth), 1.0f / static_cast<float>(lightHeight)}, ambient->strength, {}};
        check(write(context, parameters.Get(), values), "Writing the river light's constants");

        const auto gameBlend = boundBlend(context);
        ComPtr<ID3D11PixelShader> gameShader;
        ComPtr<ID3D11DepthStencilState> gameDepth;
        ComPtr<ID3D11Buffer> gameParameters;
        ID3D11ShaderResourceView* gameViews[ViewCount]{};
        UINT stencilReference{};
        context->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
        context->OMGetDepthStencilState(gameDepth.GetAddressOf(), &stencilReference);
        context->PSGetConstantBuffers(ParametersSlot, 1, gameParameters.GetAddressOf());
        context->PSGetShaderResources(FirstViewSlot, ViewCount, gameViews);
        ComPtr<ID3D11ShaderResourceView> heldViews[ViewCount];
        for (UINT i = 0; i < ViewCount; ++i) heldViews[i].Attach(gameViews[i]);

        const auto drawAgain = [&] { context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance); };
        const float clear[4]{};

        // The river as the game drew it, then without its ambient: the rows after the ambient's copied, its own left zero.
        context->OMSetBlendState(replace.Get(), nullptr, 0xFFFFFFFF);
        context->ClearRenderTargetView(lit.target.Get(), clear);
        context->OMSetRenderTargets(1, lit.target.GetAddressOf(), gameDepthView.Get());
        drawAgain();

        const D3D11_BOX rows{light.start + AmbientBytes, 0, 0, light.start + light.bytes, 1, 1};
        context->CopySubresourceRegion(withoutAmbient.Get(), 0, AmbientBytes, 0, 0, light.buffer.Get(), 0, &rows);
        context->PSSetConstantBuffers(LightSlot, 1, withoutAmbient.GetAddressOf());
        context->ClearRenderTargetView(unlit.target.Get(), clear);
        context->OMSetRenderTargets(1, unlit.target.GetAddressOf(), gameDepthView.Get());
        drawAgain();

        ComPtr<ID3D11DeviceContext1> context1;
        if (light.hasOffsets && SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context1))))
            context1->PSSetConstantBuffers1(LightSlot, 1, light.buffer.GetAddressOf(), &light.first, &light.count);
        else context->PSSetConstantBuffers(LightSlot, 1, light.buffer.GetAddressOf());

        // What the ambient adds over the traced light, off the scene.
        context->OMSetRenderTargets(2, gameTargets, gameDepthView.Get());
        ID3D11ShaderResourceView* const inputs[ViewCount]{lit.view.Get(), unlit.view.Get(), ambient->traced.Get(), ambient->probe.Get()};
        context->PSSetShader(shader.Get(), nullptr, 0);
        context->PSSetConstantBuffers(ParametersSlot, 1, parameters.GetAddressOf());
        context->PSSetShaderResources(FirstViewSlot, ViewCount, inputs);
        context->OMSetBlendState(removeFromScene.Get(), nullptr, 0xFFFFFFFF);
        context->OMSetDepthStencilState(depthTest.Get(), 0);
        drawAgain();

        context->OMSetDepthStencilState(gameDepth.Get(), stencilReference);
        context->OMSetBlendState(gameBlend.state.Get(), gameBlend.factor, gameBlend.sampleMask);
        context->PSSetShaderResources(FirstViewSlot, ViewCount, gameViews);
        context->PSSetConstantBuffers(ParametersSlot, 1, gameParameters.GetAddressOf());
        context->PSSetShader(gameShader.Get(), nullptr, 0);
    }
    catch (const std::exception& error)
    {
        logOnce(std::string("The river's light: nothing this frame: ") + error.what());
    }
}
}
