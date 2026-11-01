#include "features/forward_shadows/forward_shadows.hpp"

#include "d3d11_helpers.hpp"
#include "features/forward_shadows/caustics_map.hpp"
#include "features/forward_shadows/forward_shadows_shaders.hpp"
#include "features/path_tracing/forward_surfaces.hpp"
#include "features/path_tracing/water.hpp"
#include "hresult.hpp"
#include "log.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace phantom_fx::forward_shadows
{
namespace
{
using Microsoft::WRL::ComPtr;

// The caustics' constants: the render size, the frame's index and the strength. The camera is the draw's own
// (shaders::Caustics, b1).
struct CausticParameters
{
    float dimensions[4];
};

// The river's vertex shader reads its camera, cVSScene, at cb2; the caustics read it at b1.
constexpr UINT RiverSceneSlot = 2, CausticSceneSlot = 1;

// The game's comparison sampler, which the forward shaders sample their shadow map with (s15 in both the
// river's and the glass's shaders).
constexpr UINT ShadowSamplerSlot = 15;

std::mutex stateMutex; // The state below, made on the game's thread.
ComPtr<ID3D11ShaderResourceView> lit;      // 1x1, so the map's comparison always passes: no map shadow
ComPtr<ID3D11SamplerState> litSampler;     // comparison ALWAYS
ComPtr<ID3D11ShaderResourceView> blackCube; // The game's cube map, out of the way: the surface trace reflects it
// The caustics under the water (shaders::Caustics), drawn before it blends: the scene times the light its waves focus.
ComPtr<ID3D11PixelShader> causticShader;
ComPtr<ID3D11BlendState> causticBlend;     // scene = scene x the shader's
ComPtr<ID3D11Buffer> causticConstants;
ComPtr<ID3D11Texture2D> causticsTexture;   // The art's caustic web, tiling (caustics_map.hpp)
ComPtr<ID3D11ShaderResourceView> causticsView;
ComPtr<ID3D11SamplerState> causticsSampler; // Linear, wrap
std::atomic<bool> causticsEnabled{true};
std::atomic<float> causticsIntensity{1.0f}; // The menu's Intensity: the pattern's strength under the water
// The camera the water is drawn with, and the frame's index, as the kick leaves them.
Cameras cameras;
// The redraws' depth state: the game's own at its draw, without the depth write (made per draw in draw()).
ComPtr<ID3D11DepthStencilState> noWriteDepth;
D3D11_DEPTH_STENCIL_DESC noWriteDepthDesc{};


bool ensurePipeline(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (lit) return true;

    try
    {
        // The game's cube map, black: the rays' reflection takes its place.
        D3D11_TEXTURE2D_DESC cube{};
        cube.Width = cube.Height = 1;
        cube.MipLevels = 1;
        cube.ArraySize = 6;
        cube.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        cube.SampleDesc.Count = 1;
        cube.Usage = D3D11_USAGE_IMMUTABLE;
        cube.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        cube.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
        const std::uint32_t black = 0xFF000000u;
        const D3D11_SUBRESOURCE_DATA faces[6]{{&black, 4, 4}, {&black, 4, 4}, {&black, 4, 4}, {&black, 4, 4}, {&black, 4, 4}, {&black, 4, 4}};
        ComPtr<ID3D11Texture2D> cubeObject;
        check(device->CreateTexture2D(&cube, faces, cubeObject.GetAddressOf()), "Creating the forward shadows' black cube");
        check(device->CreateShaderResourceView(cubeObject.Get(), nullptr, blackCube.GetAddressOf()), "Creating the forward shadows' black cube view");

        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = texture.Height = 1;
        texture.MipLevels = texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_R8_UNORM;
        texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_IMMUTABLE;
        texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const std::uint8_t white = 0xFF;
        const D3D11_SUBRESOURCE_DATA data{&white, 1, 1};
        ComPtr<ID3D11Texture2D> textureObject;
        check(device->CreateTexture2D(&texture, &data, textureObject.GetAddressOf()), "Creating the forward shadows' white");
        check(device->CreateShaderResourceView(textureObject.Get(), nullptr, lit.GetAddressOf()), "Creating the forward shadows' white view");

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampler, litSampler.GetAddressOf()), "Creating the forward shadows' sampler");

        // The caustics are an addition: a shader that will not compile leaves the rest of the pass as it was.
        try
        {
            causticShader = pixelShader(device, shaders::Caustics, "forward-caustics");
            causticConstants = makeConstantBuffer(device, sizeof(CausticParameters), "Creating the caustics' constants");
            D3D11_BLEND_DESC caustics{};
            caustics.RenderTarget[0].BlendEnable = TRUE;
            caustics.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
            caustics.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
            caustics.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            caustics.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
            caustics.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
            caustics.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            caustics.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            check(device->CreateBlendState(&caustics, causticBlend.GetAddressOf()), "Creating the caustics' blend");

            // The art's caustic web, tiling: mips so deep water can sample it softer, and so a river seen far
            // does not shimmer. Generated once, here, from caustics_map.hpp.
            D3D11_TEXTURE2D_DESC map{};
            map.Width = map.Height = CausticsMapSize;
            map.MipLevels = 0;
            map.ArraySize = 1;
            map.Format = DXGI_FORMAT_R8_UNORM;
            map.SampleDesc.Count = 1;
            map.Usage = D3D11_USAGE_DEFAULT;
            map.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            map.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            check(device->CreateTexture2D(&map, nullptr, causticsTexture.GetAddressOf()), "Creating the caustics map");
            check(device->CreateShaderResourceView(causticsTexture.Get(), nullptr, causticsView.GetAddressOf()), "Creating the caustics map's view");
            context->UpdateSubresource(causticsTexture.Get(), 0, nullptr, CausticsMap, CausticsMapSize, 0);
            context->GenerateMips(causticsView.Get());

            D3D11_SAMPLER_DESC mapSampler{};
            mapSampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            mapSampler.AddressU = mapSampler.AddressV = mapSampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
            mapSampler.MaxLOD = D3D11_FLOAT32_MAX;
            check(device->CreateSamplerState(&mapSampler, causticsSampler.GetAddressOf()), "Creating the caustics map's sampler");
        }
        catch (const std::exception& error)
        {
            log(std::string("The caustics are unavailable: ") + error.what());
            causticShader.Reset();
            causticBlend.Reset();
            causticConstants.Reset();
            causticsTexture.Reset();
            causticsView.Reset();
            causticsSampler.Reset();
        }

        return true;
    }
    catch (const std::exception& error)
    {
        log(std::string("Forward shadows unavailable: ") + error.what());
        lit.Reset();
        litSampler.Reset();
        blackCube.Reset();
        causticShader.Reset();
        causticBlend.Reset();
        causticConstants.Reset();

        return false;
    }
}

void drawCall(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call)
{
    if (call.indexed) context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
    else context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
}

// Whether `format` holds depth: what the game's shadow map is, at 8192 x 8192 in the forward pass.
bool isDepthFormat(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D16_UNORM:
        return true;
    default:
        return false;
    }
}

}

void configure(const Configuration& configuration) noexcept
{
    // Only the caustics follow a setting; the rest of the pass follows whether the rays run (`ray_tracing::active`)
    // and whether the frame has its shadow and reflection, so the effect plan's states cannot turn it off by accident.
    const auto& caustics = configuration.settings.waterCaustics;
    causticsEnabled = caustics.on;
    causticsIntensity = caustics.intensity;
}

void setCameras(const Cameras& next) noexcept
{
    std::lock_guard lock(stateMutex);
    cameras = next;
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    const auto* row = shader.row();
    if (!row || row->pass != game_shaders::GamePass::ForwardWater) return false;


    static std::atomic<bool> loggedNoShadow, loggedNoViewport;
    // The rays themselves, not the effect plan: path tracing can be running while the plan reads other
    // effects' states, and the traced shadow is what this pass needs.
    if (!ray_tracing::active())
    {
        static std::atomic<bool> loggedOff;
        if (!loggedOff.exchange(true)) log("Forward shadows: the rays are not running; the forward surfaces keep the game's shadow.");
        return false;
    }

    // The rays' shadow at the render size, if this frame has one.
    D3D11_VIEWPORT viewport{};
    UINT viewports = 1;
    context->RSGetViewports(&viewports, &viewport);
    if (!viewports)
    {
        if (!loggedNoViewport.exchange(true)) log("Forward shadows: no viewport on the forward surfaces' draws.");
        return false;
    }
    ID3D11ShaderResourceView* shadow = ray_tracing::sunShadow(static_cast<UINT>(viewport.Width), static_cast<UINT>(viewport.Height));
    if (!shadow && !loggedNoShadow.exchange(true))
        log("Forward shadows: no ray-traced sun shadow at " + std::to_string(static_cast<UINT>(viewport.Width)) + "x"
            + std::to_string(static_cast<UINT>(viewport.Height)) + "; the surfaces keep the game's shadow.");
    // The water's own sun shadow, traced at its surface where the reflections were: the shadow texture holds
    // the G-buffer's surface, the bed under the water, which the deferred lighting over the water needs.
    if (auto* waterShadow = ray_tracing::forwardShadow(static_cast<UINT>(viewport.Width), static_cast<UINT>(viewport.Height))) shadow = waterShadow;

    // The rays' reflection: with it, the game's cube map makes way; without it, the surface keeps its own.
    static std::atomic<bool> loggedNoReflection;
    auto* reflection = ray_tracing::reflection(static_cast<UINT>(viewport.Width), static_cast<UINT>(viewport.Height));
    if (!reflection && !loggedNoReflection.exchange(true))
        log("Forward shadows: no ray-traced reflection at " + std::to_string(static_cast<UINT>(viewport.Width)) + "x"
            + std::to_string(static_cast<UINT>(viewport.Height)) + "; the surfaces keep the game's cube map.");

    ComPtr<ID3D11Device> device;
    context->GetDevice(device.GetAddressOf());
    if (!device) return false;

    // The bed's depth, as the kick copied it before this frame's water drew, for the water column above it. Taken
    // before the state's lock: the kick holds ray tracing's when it fills the cameras.
    auto bed = ray_tracing::surfaceDepth(static_cast<UINT>(viewport.Width), static_cast<UINT>(viewport.Height));

    std::lock_guard lock(stateMutex);
    if (!ensurePipeline(device.Get(), context)) return false;

    // A row that does not name its slots - every glass but fox3dfw_glass: the game's shadow map is the largest
    // square depth texture this draw binds, and its reflection the only cube view. Read from what the draw
    // binds, so a glass the table has never seen still gets the rays' shadow and the rays' reflection.
    UINT shadowSlot = row->shadowSlot, cubeSlot = row->cubeSlot;
    if (shadowSlot == 0 || cubeSlot == 0)
    {
        ID3D11ShaderResourceView* bound[16]{};
        context->PSGetShaderResources(0, 16, bound);
        UINT largest = 0;
        for (UINT slot = 0; slot < 16; ++slot)
        {
            if (!bound[slot]) continue;
            ComPtr<ID3D11Resource> resource;
            ComPtr<ID3D11Texture2D> texture;
            bound[slot]->GetResource(resource.GetAddressOf());
            if (resource) resource.As(&texture);
            if (texture)
            {
                D3D11_TEXTURE2D_DESC desc{};
                texture->GetDesc(&desc);
                if (cubeSlot == 0 && (desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)) cubeSlot = slot;
                if (shadowSlot == 0 && isDepthFormat(desc.Format) && desc.Width == desc.Height && desc.Width >= 1024 && desc.Width > largest)
                {
                    shadowSlot = slot;
                    largest = desc.Width;
                }
            }
            bound[slot]->Release();
        }
    }
    const bool shading = shadow != nullptr && shadowSlot != 0;

    // The state the pass changes, to put back.
    ComPtr<ID3D11ShaderResourceView> gameShadow, gameCube;
    ComPtr<ID3D11Buffer> gameConstants;
    context->PSGetConstantBuffers(0, 1, gameConstants.GetAddressOf());
    ID3D11ShaderResourceView* gameTargets3[3]{};
    if (shading) context->PSGetShaderResources(shadowSlot, 1, gameShadow.GetAddressOf());
    context->PSGetShaderResources(0, 3, gameTargets3);
    const bool replacingCube = reflection != nullptr && cubeSlot != 0;
    if (replacingCube) context->PSGetShaderResources(cubeSlot, 1, gameCube.GetAddressOf());

    ComPtr<ID3D11SamplerState> gameSampler;
    context->PSGetSamplers(ShadowSamplerSlot, 1, gameSampler.GetAddressOf());
    ComPtr<ID3D11PixelShader> gamePixel;
    context->PSGetShader(gamePixel.GetAddressOf(), nullptr, nullptr);
    ComPtr<ID3D11BlendState> gameBlend;
    float factor[4]{};
    UINT sampleMask = 0;
    context->OMGetBlendState(gameBlend.GetAddressOf(), factor, &sampleMask);
    ComPtr<ID3D11DepthStencilState> gameDepth;
    UINT stencil = 0;
    context->OMGetDepthStencilState(gameDepth.GetAddressOf(), &stencil);

    // The surface without the map's shadow: the rays' takes its place below.
    if (shading)
    {
        static std::atomic<bool> logged{};
        if (!logged.exchange(true)) log(std::string("Forward shadows: ") + row->name + "'s draws leave the game's shadow map out; the surface takes no sun shadow.");
        context->PSSetShaderResources(shadowSlot, 1, lit.GetAddressOf());
        context->PSSetSamplers(ShadowSamplerSlot, 1, litSampler.GetAddressOf());
    }
    if (replacingCube) context->PSSetShaderResources(cubeSlot, 1, blackCube.GetAddressOf());

    drawCall(context, call);

    // The caustics on the bed the water shows: the same geometry once more, times the light its waves focus, over
    // the water's own output. The water's refraction carries the bed, so the multiply lands on the bed seen through
    // it - not on the bed before the water's blend, which the water's opacity hid in its deep parts. The rays'
    // reflection is added later, at the composite, so it keeps its own brightness. The game's depth test decides
    // exactly where the water drew; the pass writes no depth.
    if (causticsEnabled && bed && causticShader && causticBlend && causticConstants)
    {
        // The pass writes no depth: the game's own test decides where the water drew. A frame that cannot set it up
        // skips the caustics - they are an addition - and the water's own draw stands.
        D3D11_DEPTH_STENCIL_DESC noWriteDesc{};
        gameDepth->GetDesc(&noWriteDesc);
        noWriteDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (!noWriteDepth || std::memcmp(&noWriteDesc, &noWriteDepthDesc, sizeof(noWriteDesc)) != 0)
        {
            noWriteDepth.Reset();
            if (SUCCEEDED(device->CreateDepthStencilState(&noWriteDesc, noWriteDepth.GetAddressOf()))) noWriteDepthDesc = noWriteDesc;
        }

        // stateMutex is already held for the whole draw.
        const CausticParameters parameters{{cameras.dimensions[0], cameras.dimensions[1], cameras.frame, causticsIntensity.load()}};

        ComPtr<ID3D11Buffer> scene, gameSceneSlot;
        context->VSGetConstantBuffers(RiverSceneSlot, 1, scene.GetAddressOf());
        ComPtr<ID3D11SamplerState> gameMapSampler;
        if (noWriteDepth && scene && SUCCEEDED(write(context, causticConstants.Get(), parameters)))
        {
            context->PSGetConstantBuffers(CausticSceneSlot, 1, gameSceneSlot.GetAddressOf());
            context->PSSetConstantBuffers(CausticSceneSlot, 1, scene.GetAddressOf());
            // The game's s0, ours for the pass: the map's sampler, put back after.
            context->PSGetSamplers(0, 1, gameMapSampler.GetAddressOf());
            context->PSSetShader(causticShader.Get(), nullptr, 0);
            context->PSSetConstantBuffers(0, 1, causticConstants.GetAddressOf());
            context->PSSetShaderResources(0, 1, bed.GetAddressOf());
            if (causticsView) context->PSSetShaderResources(1, 1, causticsView.GetAddressOf());
            if (causticsSampler) context->PSSetSamplers(0, 1, causticsSampler.GetAddressOf());
            context->OMSetBlendState(causticBlend.Get(), nullptr, 0xFFFFFFFF);
            context->OMSetDepthStencilState(noWriteDepth.Get(), stencil);
            drawCall(context, call);
            context->OMSetDepthStencilState(gameDepth.Get(), stencil);
            context->OMSetBlendState(gameBlend.Get(), factor, sampleMask);
            context->PSSetShader(gamePixel.Get(), nullptr, 0);
            context->PSSetConstantBuffers(0, 1, gameConstants.GetAddressOf());
            context->PSSetConstantBuffers(CausticSceneSlot, 1, gameSceneSlot.GetAddressOf());
            context->PSSetSamplers(0, 1, gameMapSampler.GetAddressOf());
            // The game's t0 and t1 back: t1 is the river's normal map, which the water's surface reads next
            // (water::recordWater).
            context->PSSetShaderResources(0, 2, gameTargets3);
        }
    }


    // The rivers' and liquids' reflection: recorded as a surface for the glass module's second trace, which runs
    // after the scene and adds what it reflected at the exposure (glass.hpp) - the current camera, so it cannot lag.
    if (replacingCube)
    {
        static std::atomic<bool> loggedReflection{};
        if (!loggedReflection.exchange(true)) log("Forward shadows: the rivers' reflection leaves the cube map to the surface trace.");
        water::recordWater(context, call, *row);
    }

    // Back as the game had it.
    context->PSSetShader(gamePixel.Get(), nullptr, 0);
    if (shading) context->PSSetShaderResources(shadowSlot, 1, gameShadow.GetAddressOf());
    context->PSSetShaderResources(0, 3, gameTargets3);
    if (replacingCube) context->PSSetShaderResources(cubeSlot, 1, gameCube.GetAddressOf());
    for (auto*& view : gameTargets3)
        if (view) view->Release();
    if (shading) context->PSSetSamplers(ShadowSamplerSlot, 1, gameSampler.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, gameConstants.GetAddressOf());
    context->OMSetBlendState(gameBlend.Get(), factor, sampleMask);
    context->OMSetDepthStencilState(gameDepth.Get(), stencil);
    return true;
}



bool tracedShadowResources(ID3D11DeviceContext* context, ID3D11ShaderResourceView** whiteMap, ID3D11SamplerState** alwaysPass) noexcept
{
    ComPtr<ID3D11Device> device;
    context->GetDevice(device.GetAddressOf());
    if (!device) return false;

    std::lock_guard lock(stateMutex);
    if (!ensurePipeline(device.Get(), context)) return false;

    *whiteMap = lit.Get();
    *alwaysPass = litSampler.Get();
    return true;
}






}
