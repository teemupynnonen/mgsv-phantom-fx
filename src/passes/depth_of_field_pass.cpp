#include "passes/depth_of_field_pass.hpp"

#include "d3d11_helpers.hpp"
#include "log.hpp"
#include "passes/depth_of_field_pass_shaders.hpp"

#include <wrl/client.h>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <utility>

namespace phantom_fx::depth_of_field_pass
{
namespace
{
using game_shaders::GamePass;
using Microsoft::WRL::ComPtr;

constexpr UINT DepthSlot = 1;                 // DOF_ScatterBakeFirst's inDepth
constexpr UINT CoverageSlot = 2, FarSlot = 3; // What shaders::Composite reads beside the game's t0 and t1
static_assert(FarSlot == CoverageSlot + 1, "the composite binds them together");

struct Target
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> view;
    UINT width{}, height{};
};

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11PixelShader> coverageShader, compositeShader;
Target coverage, farLayer;

// How far this thread has come through the depth of field's draws, which it records together. Each step
// needs the one before, so a frame that misses one is the game's own.
enum class Stage : std::uint8_t { None, Baked, FarDrawn };
thread_local Stage stage{};

// The game's targets and blend, put back when this goes out of scope.
class SavedOutput
{
public:
    explicit SavedOutput(ID3D11DeviceContext* context) : context(context), blend(boundBlend(context))
    {
        context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depth.GetAddressOf());
    }

    ~SavedOutput()
    {
        context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depth.Get());
        context->OMSetBlendState(blend.state.Get(), blend.factor, blend.sampleMask);
        for (auto* target : targets)
            if (target) target->Release();
    }

    SavedOutput(const SavedOutput&) = delete;
    SavedOutput& operator=(const SavedOutput&) = delete;

private:
    ID3D11DeviceContext* context;

    BoundBlend blend;
    ComPtr<ID3D11DepthStencilView> depth;
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
};

// Call with resourceMutex held.
void ensureTarget(Target& target, UINT width, UINT height, DXGI_FORMAT format, const char* what)
{
    if (target.texture && target.width == width && target.height == height) return;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Target made{.width = width, .height = height};
    check(device->CreateTexture2D(&desc, nullptr, made.texture.GetAddressOf()), what);
    check(device->CreateRenderTargetView(made.texture.Get(), nullptr, made.target.GetAddressOf()), what);
    check(device->CreateShaderResourceView(made.texture.Get(), nullptr, made.view.GetAddressOf()), what);
    target = std::move(made);
}

// The draw alone in `into`, unblended.
void drawInto(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, const Target& into)
{
    const SavedOutput saved(context);
    const WholeTargetViewport viewport(context, into.width, into.height);
    context->OMSetRenderTargets(1, into.target.GetAddressOf(), nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
}

// The game's constants and depth are bound for its own bake, which draws after.
void noteCoverage(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, ID3D11PixelShader* gameShader)
{
    stage = Stage::None;

    ComPtr<ID3D11ShaderResourceView> depth;
    context->PSGetShaderResources(DepthSlot, 1, depth.GetAddressOf());
    const auto [width, height] = sizeOf(depth.Get());
    std::lock_guard lock(resourceMutex);
    if (!width || !compositeShader) return;

    ensureTarget(coverage, width, height, DXGI_FORMAT_R8_UNORM, "Creating the depth of field's cover");
    context->PSSetShader(coverageShader.Get(), nullptr, 0);
    drawInto(context, call, coverage);
    context->PSSetShader(gameShader, nullptr, 0);
    stage = Stage::Baked;
}

// The game blends the far layer under the near one it drew; apart, its target keeps the near layer alone.
bool drawFarLayer(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call)
{
    if (stage != Stage::Baked) return false;

    ComPtr<ID3D11RenderTargetView> gameTarget;
    context->OMGetRenderTargets(1, gameTarget.GetAddressOf(), nullptr);
    const auto [width, height] = sizeOf(gameTarget.Get());
    std::lock_guard lock(resourceMutex);
    ensureTarget(farLayer, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the depth of field's far layer");
    drawInto(context, call, farLayer);
    stage = Stage::FarDrawn;

    return true;
}

bool composite(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, ID3D11PixelShader* gameShader)
{
    if (stage != Stage::FarDrawn) return false;

    stage = Stage::None;

    ComPtr<ID3D11ShaderResourceView> gameViews[2];
    ID3D11ShaderResourceView* bound[2]{};
    context->PSGetShaderResources(CoverageSlot, 2, bound);
    gameViews[0].Attach(bound[0]);
    gameViews[1].Attach(bound[1]);

    std::lock_guard lock(resourceMutex);
    ID3D11ShaderResourceView* const views[]{coverage.view.Get(), farLayer.view.Get()};
    context->PSSetShader(compositeShader.Get(), nullptr, 0);
    context->PSSetShaderResources(CoverageSlot, 2, views);
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    context->PSSetShader(gameShader, nullptr, 0);
    context->PSSetShaderResources(CoverageSlot, 2, gameViews[0].GetAddressOf());

    return true;
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    try
    {
        coverageShader = pixelShader(device.Get(), shaders::Coverage, "depth-of-field-coverage");
        // Last, so it marks a complete pipeline.
        compositeShader = pixelShader(device.Get(), shaders::Composite, "depth-of-field-composite");
    }
    catch (const std::exception& error)
    {
        log(std::string("The depth of field keeps the game's focus edges: ") + error.what());
    }
}

void detach() noexcept
{
    std::lock_guard lock(resourceMutex);
    coverageShader.Reset();
    compositeShader.Reset();
    coverage = {};
    farLayer = {};
    device.Reset();
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    const auto* row = shader.row();
    if (!row) return false;

    try
    {
        switch (row->pass)
        {
        case GamePass::DofBake:
            noteCoverage(context, call, shader.object());
            return false;
        case GamePass::DofFar:
            return drawFarLayer(context, call);
        case GamePass::DofComposite:
            return composite(context, call, shader.object());
        default:
            return false;
        }
    }
    catch (const std::exception& error)
    {
        stage = Stage::None;
        logOnce(std::string("Depth of field: ") + error.what());

        return false;
    }
}
}
