// The forward surfaces' shared half on Direct3D 11; forward_surfaces.hpp describes it.
#include "features/path_tracing/forward_surfaces.hpp"

#include "command_list_notes.hpp"
#include "d3d11_helpers.hpp"
#include "features/path_tracing/path_tracing.hpp"
#include "features/path_tracing/path_tracing_shaders.hpp"
#include "features/path_tracing/surface_shaders.hpp"
#include "log.hpp"
#include "passes/forward_scene.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "performance.hpp"

#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <utility>

namespace phantom_fx::forward_surfaces
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr float ClearColor[4]{};

struct CompositeParameters
{
    float strength, padding[3];
};

// What a deferred context, then its command list, recorded: a surface drawn, into the scene `scene`; the draw that ends the
// scene.
struct Notes
{
    bool hasSurfaces{}, isSceneEnd{};
    ComPtr<ID3D11RenderTargetView> scene;

    void merge(Notes later)
    {
        hasSurfaces |= later.hasSurfaces;
        isSceneEnd |= later.isSceneEnd;
        if (later.scene) scene = std::move(later.scene);
    }
};

CommandListNotes<Notes> notes;

// A surface target with the view that draws into it.
struct Target
{
    ComputeTexture texture;
    ComPtr<ID3D11RenderTargetView> renderTarget;
};

// ---- Direct3D, guarded by resourceMutex, never held through a Direct3D call on a recording thread ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11PixelShader> compositeShader;
ComPtr<ID3D11VertexShader> fullScreenShader;
ComPtr<ID3D11ComputeShader> guidesShader;
ComPtr<ID3D11PixelShader> glassShader, colorGlassShader, mirrorShader;
ComPtr<ID3D11PixelShader> waterShaders[2][2][2]; // [isLayeredRiver][hasTwoChannelNormal][hasRipples] (game_shaders.hpp)
ComPtr<ID3D11BlendState> surfaceBlend, colorOnly;
ComPtr<ID3D11DepthStencilState> behindGameDepth;
ComPtr<ID3D11Buffer> compositeParameters;
Target depthTarget, normalTarget, weightTarget;
ComputeTexture reflection;
// The scene, as the composite reads it while it draws into it.
ComPtr<ID3D11Texture2D> sceneCopy;
ComPtr<ID3D11ShaderResourceView> sceneCopyView;
bool hasPipelineFailed{};
// The immediate context's: the frame's lists ran a surface's draw, into this scene; the targets hold a frame's surfaces; the
// frame's surfaces reflect, and Ray Reconstruction has yet to hear of them.
bool hasSurfaces{}, areTargetsDrawn{}, hasReflected{};
ComPtr<ID3D11RenderTargetView> scene;

ComPtr<ID3D11PixelShader> surfaceShader(const char* source, const char* name, const D3D_SHADER_MACRO* defines = nullptr)
{
    return pixelShader(device.Get(), (std::string(forward_scene::Common) + surface_shaders::SurfaceOutput + source).c_str(), name, defines);
}

// Compiles the surfaces' shaders and makes the states once; call with resourceMutex held. Returns false when they are unavailable.
bool ensurePipeline()
{
    if (compositeShader) return true;
    if (!device || hasPipelineFailed) return false;

    try
    {
        const auto fullScreenCode = compile(path_tracing::shaders::FullScreen, "surfaces-full-screen", "vs_5_0");
        check(device->CreateVertexShader(fullScreenCode->GetBufferPointer(), fullScreenCode->GetBufferSize(), nullptr, fullScreenShader.ReleaseAndGetAddressOf()),
            "Creating the surfaces' vertex shader");
        compositeParameters = makeConstantBuffer(device.Get(), sizeof(CompositeParameters), "Creating the surfaces' constants");

        const auto guidesCode = compile(surface_shaders::ReconstructionGuides, "surfaces-guides", "cs_5_0");
        check(device->CreateComputeShader(guidesCode->GetBufferPointer(), guidesCode->GetBufferSize(), nullptr, guidesShader.ReleaseAndGetAddressOf()),
            "Creating the surfaces' guides");

        // The nearest surface is the last drawn: the game sorts what blends. The weights blend as the game blends the
        // surfaces, so a pane drawn twice weighs as it shows.
        D3D11_BLEND_DESC blend{};
        blend.IndependentBlendEnable = TRUE;
        for (auto& target : blend.RenderTarget) target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        auto& weights = blend.RenderTarget[2];
        weights.BlendEnable = TRUE;
        weights.SrcBlend = weights.SrcBlendAlpha = D3D11_BLEND_ONE;
        weights.DestBlend = weights.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        weights.BlendOp = weights.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        check(device->CreateBlendState(&blend, surfaceBlend.ReleaseAndGetAddressOf()), "Creating the surfaces' blend");

        D3D11_BLEND_DESC colour{};
        colour.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        check(device->CreateBlendState(&colour, colorOnly.ReleaseAndGetAddressOf()), "Creating the surfaces' composite blend");

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        check(device->CreateDepthStencilState(&depth, behindGameDepth.ReleaseAndGetAddressOf()), "Creating the surfaces' depth test");

        const D3D_SHADER_MACRO color[]{{"COLOR_TEXTURE", "1"}, {}};
        glassShader = surfaceShader(surface_shaders::GlassSurface, "glass-surface");
        colorGlassShader = surfaceShader(surface_shaders::GlassSurface, "glass-surface-color", color);
        mirrorShader = surfaceShader(surface_shaders::MirrorSurface, "mirror-surface");
        for (int layered = 0; layered < 2; ++layered)
            for (int twoChannel = 0; twoChannel < 2; ++twoChannel)
                for (int ripples = 0; ripples < 2; ++ripples)
                {
                    const D3D_SHADER_MACRO defines[]{{"LAYERED", layered ? "1" : "0"}, {"TWO_CHANNEL", twoChannel ? "1" : "0"}, {"RIPPLES", ripples ? "1" : "0"}, {}};
                    waterShaders[layered][twoChannel][ripples] = surfaceShader(surface_shaders::WaterSurface, "water-surface", defines);
                }

        // Last, so it marks a complete pipeline.
        compositeShader = pixelShader(device.Get(), surface_shaders::Composite, "surfaces-composite");
        return true;
    }
    catch (const std::exception& error)
    {
        hasPipelineFailed = true;
        log(std::string("The forward surfaces keep the game's reflections: ") + error.what());
        return false;
    }
}

// Which shader draws a game's surface into the targets.
ID3D11PixelShader* shaderOf(const game_shaders::GameShader& row)
{
    if (row.pass == game_shaders::GamePass::Mirror) return mirrorShader.Get();
    if (row.pass == game_shaders::GamePass::Glass) return row.hasColorTexture ? colorGlassShader.Get() : glassShader.Get();

    return waterShaders[row.isLayeredRiver][row.hasTwoChannelNormal][row.hasRipples].Get();
}

// Whether the module has its pipeline and targets of this size; call with resourceMutex held.
bool canRecord(UINT width, UINT height)
{
    return compositeShader && weightTarget.texture.width == width && weightTarget.texture.height == height;
}

Target makeTarget(UINT width, UINT height, DXGI_FORMAT format, const char* what)
{
    Target target{makeComputeTexture(device.Get(), width, height, format, what, SharedWithD3D12, D3D11_BIND_RENDER_TARGET), nullptr};
    check(device->CreateRenderTargetView(target.texture.texture.Get(), nullptr, target.renderTarget.GetAddressOf()), what);

    return target;
}

// The game's draw again, into the surface targets with `shader`, in its depth test without writing; its shader,
// targets and states put back.
void drawSurface(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, ID3D11PixelShader* shader, ID3D11RenderTargetView* const (&targets)[3],
    ID3D11BlendState* blend, ID3D11DepthStencilState* depthTest, ID3D11RenderTargetView* gameTarget, ID3D11DepthStencilView* gameDepth)
{
    ComPtr<ID3D11PixelShader> gameShader;
    ComPtr<ID3D11DepthStencilState> gameDepthTest;
    UINT stencilReference{};
    const auto gameBlend = boundBlend(context);
    context->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
    context->OMGetDepthStencilState(gameDepthTest.GetAddressOf(), &stencilReference);

    context->PSSetShader(shader, nullptr, 0);
    context->OMSetRenderTargets(3, targets, gameDepth);
    context->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(depthTest, 0);
    context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);

    context->OMSetDepthStencilState(gameDepthTest.Get(), stencilReference);
    context->OMSetBlendState(gameBlend.state.Get(), gameBlend.factor, gameBlend.sampleMask);
    context->OMSetRenderTargets(1, &gameTarget, gameDepth);
    context->PSSetShader(gameShader.Get(), nullptr, 0);
}

// What the second trace reflected, added over `target`; call with resourceMutex held.
void composite(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target, float strength)
{
    const auto sceneTexture = resourceOf(target);
    auto desc = textureDesc(sceneTexture.Get());
    const auto copyDesc = textureDesc(sceneCopy.Get());
    if (copyDesc.Width != desc.Width || copyDesc.Height != desc.Height || copyDesc.Format != desc.Format)
    {
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = 0;
        check(device->CreateTexture2D(&desc, nullptr, sceneCopy.ReleaseAndGetAddressOf()), "Creating the surfaces' copy of the scene");

        // Read as it is drawn: the scene's sRGB-encoded values, not decoded.
        D3D11_RENDER_TARGET_VIEW_DESC targetDesc{};
        target->GetDesc(&targetDesc);
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
        viewDesc.Format = targetDesc.Format;
        viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipLevels = 1;
        check(device->CreateShaderResourceView(sceneCopy.Get(), &viewDesc, sceneCopyView.ReleaseAndGetAddressOf()), "Creating the surfaces' copy of the scene");
    }

    check(write(immediate, compositeParameters.Get(), CompositeParameters{strength}), "Writing the surfaces' constants");
    immediate->CopyResource(sceneCopy.Get(), sceneTexture.Get());

    ID3D11ShaderResourceView* const inputs[]{sceneCopyView.Get(), weightTarget.texture.view.Get(), reflection.view.Get(),
        normalTarget.texture.view.Get()};
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1};
    immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    immediate->IASetInputLayout(nullptr);
    immediate->VSSetShader(fullScreenShader.Get(), nullptr, 0);
    immediate->GSSetShader(nullptr, nullptr, 0);
    immediate->PSSetShader(compositeShader.Get(), nullptr, 0);
    immediate->PSSetConstantBuffers(0, 1, compositeParameters.GetAddressOf());
    immediate->PSSetShaderResources(0, 4, inputs);
    immediate->OMSetRenderTargets(1, &target, nullptr);
    immediate->OMSetBlendState(colorOnly.Get(), nullptr, 0xFFFFFFFF);
    immediate->OMSetDepthStencilState(nullptr, 0);
    immediate->RSSetState(nullptr);
    immediate->RSSetViewports(1, &viewport);
    immediate->Draw(3, 0);

    ID3D11ShaderResourceView* unbound[4]{};
    immediate->PSSetShaderResources(0, 4, unbound);
    immediate->OMSetRenderTargets(0, nullptr, nullptr);
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    std::lock_guard lock(resourceMutex);
    device = gameDevice;
}

void detach() noexcept
{
    notes.clear();

    std::lock_guard lock(resourceMutex);
    compositeShader.Reset();
    fullScreenShader.Reset();
    guidesShader.Reset();
    glassShader.Reset();
    colorGlassShader.Reset();
    mirrorShader.Reset();
    for (auto& layered : waterShaders)
        for (auto& channels : layered)
            for (auto& shader : channels) shader.Reset();
    surfaceBlend.Reset();
    colorOnly.Reset();
    behindGameDepth.Reset();
    compositeParameters.Reset();
    depthTarget = normalTarget = weightTarget = {};
    reflection = {};
    sceneCopy.Reset();
    sceneCopyView.Reset();
    scene.Reset();
    device.Reset();
    hasPipelineFailed = hasSurfaces = areTargetsDrawn = hasReflected = false;
}

void present() noexcept
{
    notes.present();
}

void note(ID3D11DeviceContext* context, game_shaders::DrawnShader& shader) noexcept
{
    try
    {
        const auto* row = shader.row();
        if (!row || row->pass != game_shaders::GamePass::Exposure || !path_tracing::isReflecting()) return;

        notes.note(reinterpret_cast<std::uint64_t>(context), [](Notes& contextNotes) { contextNotes.isSceneEnd = true; });
    }
    catch (...)
    {
    }
}

bool accepts(ID3D11DeviceContext* context) noexcept
{
    try
    {
        ComPtr<ID3D11RenderTargetView> gameTarget;
        ComPtr<ID3D11DepthStencilView> gameDepth;
        context->OMGetRenderTargets(1, gameTarget.GetAddressOf(), gameDepth.GetAddressOf());
        const auto [width, height] = sizeOf(gameTarget.Get());

        std::lock_guard lock(resourceMutex);
        return canRecord(width, height);
    }
    catch (...)
    {
        return false;
    }
}

bool record(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, const game_shaders::GameShader& row) noexcept
{
    try
    {
        ComPtr<ID3D11RenderTargetView> gameTarget;
        ComPtr<ID3D11DepthStencilView> gameDepth;
        context->OMGetRenderTargets(1, gameTarget.GetAddressOf(), gameDepth.GetAddressOf());
        const auto [width, height] = sizeOf(gameTarget.Get());

        ComPtr<ID3D11PixelShader> shader;
        ComPtr<ID3D11BlendState> blend;
        ComPtr<ID3D11DepthStencilState> depthTest;
        ComPtr<ID3D11RenderTargetView> depth, normal, weight;
        {
            std::lock_guard lock(resourceMutex);
            if (!canRecord(width, height)) return false;

            shader = shaderOf(row);
            blend = surfaceBlend;
            depthTest = behindGameDepth;
            depth = depthTarget.renderTarget;
            normal = normalTarget.renderTarget;
            weight = weightTarget.renderTarget;
        }

        ID3D11RenderTargetView* const targets[3]{depth.Get(), normal.Get(), weight.Get()};
        drawSurface(context, call, shader.Get(), targets, blend.Get(), depthTest.Get(), gameTarget.Get(), gameDepth.Get());
        notes.note(reinterpret_cast<std::uint64_t>(context), [&](Notes& contextNotes) {
            contextNotes.hasSurfaces = true;
            contextNotes.scene = gameTarget;
        });

        return true;
    }
    catch (...)
    {
        return false;
    }
}

void recorded(std::uint64_t into, std::uint64_t from) noexcept
{
    notes.recorded(into, from);
}

void forget(std::uint64_t contextOrList) noexcept
{
    notes.forget(contextOrList);
}

ComPtr<ID3D11ShaderResourceView> surfaceWeights(UINT width, UINT height) noexcept
{
    std::lock_guard lock(resourceMutex);
    const auto& weights = weightTarget.texture;
    return weights.width == width && weights.height == height ? weights.view : nullptr;
}

SurfaceTextures beginFrame(ID3D11DeviceContext* immediate, UINT width, UINT height) noexcept
{
    try
    {
        std::lock_guard lock(resourceMutex);
        if (!ensurePipeline()) return {};

        if (weightTarget.texture.width != width || weightTarget.texture.height != height)
        {
            depthTarget = makeTarget(width, height, DXGI_FORMAT_R32_FLOAT, "Creating the surfaces' depth");
            normalTarget = makeTarget(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the surfaces' normals");
            weightTarget = makeTarget(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the surfaces' weights");
            reflection = makeComputeTexture(device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the surfaces' reflections", SharedWithD3D12);
            areTargetsDrawn = true;
        }

        if (std::exchange(areTargetsDrawn, false))
            for (const auto* target : {&depthTarget, &normalTarget, &weightTarget}) immediate->ClearRenderTargetView(target->renderTarget.Get(), ClearColor);

        hasSurfaces = hasReflected = false;
        scene.Reset();

        return {depthTarget.texture.texture.Get(), normalTarget.texture.texture.Get(), reflection.texture.Get()};
    }
    catch (const std::exception& error)
    {
        logOnce(std::string("The forward surfaces keep the game's reflections: ") + error.what());
        return {};
    }
}

void executing(ID3D11DeviceContext* context, std::uint64_t list) noexcept
{
    const auto noted = notes.take(list);
    if (!noted) return;

    try
    {
        ComPtr<ID3D11RenderTargetView> target;
        {
            std::lock_guard lock(resourceMutex);
            if (noted->hasSurfaces)
            {
                hasSurfaces = areTargetsDrawn = true;
                scene = noted->scene;
            }

            if (!noted->isSceneEnd || !std::exchange(hasSurfaces, false)) return;

            target = scene;
        }

        // Without resourceMutex: the kick takes it inside the ray tracing's lock.
        if (!target || !ray_tracing::traceSurfaces()) return;

        std::lock_guard lock(resourceMutex);
        const performance::Timed timed(context, performance::Pass::PathTracedLight);
        composite(context, target.Get(), path_tracing::reflectionStrength());
        hasReflected = true;
        logOnce("The forward surfaces: their reflections are traced, in place of the game's.");
    }
    catch (const std::exception& error)
    {
        logOnce(std::string("The forward surfaces: no reflections this frame: ") + error.what());
    }
}

ComPtr<ID3D11ShaderResourceView> guideReconstruction(ID3D11DeviceContext* immediate, ID3D11UnorderedAccessView* normals, ID3D11UnorderedAccessView* hitDistance) noexcept
{
    std::lock_guard lock(resourceMutex);
    if (!std::exchange(hasReflected, false) || !normals || !hitDistance) return nullptr;

    ID3D11ShaderResourceView* inputs[]{normalTarget.texture.view.Get(), weightTarget.texture.view.Get(), reflection.view.Get()};
    ID3D11UnorderedAccessView* outputs[]{depthTarget.texture.access.Get(), normals, hitDistance};
    immediate->CSSetShader(guidesShader.Get(), nullptr, 0);
    immediate->CSSetShaderResources(0, 3, inputs);
    immediate->CSSetUnorderedAccessViews(0, 3, outputs, nullptr);
    immediate->Dispatch((weightTarget.texture.width + 7) / 8, (weightTarget.texture.height + 7) / 8, 1);
    immediate->ClearState();

    return depthTarget.texture.view;
}
}
