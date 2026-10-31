#include "passes/depth_of_field_pass.hpp"

#include "d3d11_helpers.hpp"
#include "log.hpp"
#include "passes/depth_of_field_pass_shaders.hpp"

#include <wrl/client.h>
#include <atomic>
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
constexpr UINT LayerSlotCount = 3; // t2 to t4, which the lens's passes use

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
ComPtr<ID3D11PixelShader> coverageShader, compositeShader, prefilterShader, gatherShader, fillShader, lensCompositeShader;
Target coverage, farLayer, prefiltered, gathered;

// How far this thread has come through the depth of field's draws, which it records together. Each step
// needs the one before, so a frame that misses one is the game's own.
enum class Stage : std::uint8_t { None, Baked, FarDrawn };
thread_local Stage stage{};

// Read at the first draw alone, so a change in the menu cannot split a frame.
std::atomic<FarBlur> wantedFarBlur{FarBlur::Lens};
thread_local FarBlur farBlur{};

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
    farBlur = wantedFarBlur;

    ComPtr<ID3D11ShaderResourceView> depth;
    context->PSGetShaderResources(DepthSlot, 1, depth.GetAddressOf());
    const auto [width, height] = sizeOf(depth.Get());
    std::lock_guard lock(resourceMutex);
    if (!width || !lensCompositeShader) return;

    ensureTarget(coverage, width, height, DXGI_FORMAT_R16G16_FLOAT, "Creating the depth of field's cover");
    context->PSSetShader(coverageShader.Get(), nullptr, 0);
    drawInto(context, call, coverage);
    context->PSSetShader(gameShader, nullptr, 0);
    stage = Stage::Baked;
}

// The game blends the far layer under the near one it drew; apart, its target keeps the near layer alone.
bool drawFarLayer(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call)
{
    if (stage != Stage::Baked) return false;

    stage = Stage::FarDrawn;
    // The lens makes its own far blur, so all it wants of this draw is the near layer left without it.
    if (farBlur == FarBlur::Lens) return true;

    ComPtr<ID3D11RenderTargetView> gameTarget;
    context->OMGetRenderTargets(1, gameTarget.GetAddressOf(), nullptr);
    const auto [width, height] = sizeOf(gameTarget.Get());
    std::lock_guard lock(resourceMutex);
    ensureTarget(farLayer, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the depth of field's far layer");
    drawInto(context, call, farLayer);

    return true;
}

// One half-size pass into `into`. The slots are emptied first, as the last pass's input may be this one's
// target, and the inputs go in after the target, as a view of a bound target reads as nothing.
void drawLayer(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, const Target& into, ID3D11PixelShader* shader,
    ID3D11ShaderResourceView* slot2, ID3D11ShaderResourceView* slot3, ID3D11ShaderResourceView* slot4)
{
    ID3D11ShaderResourceView* const none[LayerSlotCount]{};
    ID3D11ShaderResourceView* const views[LayerSlotCount]{slot2, slot3, slot4};
    context->PSSetShaderResources(CoverageSlot, LayerSlotCount, none);
    context->OMSetRenderTargets(1, into.target.GetAddressOf(), nullptr);
    context->PSSetShaderResources(CoverageSlot, LayerSlotCount, views);
    context->PSSetShader(shader, nullptr, 0);
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
}

// Call with resourceMutex held. The composite's t0, t1 and constants stay bound throughout.
void drawLens(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call)
{
    const UINT width = coverage.width / 2, height = coverage.height / 2;
    constexpr DXGI_FORMAT Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ensureTarget(prefiltered, width, height, Format, "Creating the depth of field's prefiltered light");
    ensureTarget(gathered, width, height, Format, "Creating the depth of field's gather");
    ensureTarget(farLayer, width, height, Format, "Creating the depth of field's far layer");

    const SavedOutput saved(context);
    const WholeTargetViewport viewport(context, width, height);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    drawLayer(context, call, prefiltered, prefilterShader.Get(), coverage.view.Get(), nullptr, nullptr);
    drawLayer(context, call, gathered, gatherShader.Get(), nullptr, prefiltered.view.Get(), nullptr);
    drawLayer(context, call, farLayer, fillShader.Get(), nullptr, gathered.view.Get(), prefiltered.view.Get());

    ID3D11ShaderResourceView* const none[LayerSlotCount]{};
    context->PSSetShaderResources(CoverageSlot, LayerSlotCount, none);
}

bool composite(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, ID3D11PixelShader* gameShader)
{
    if (stage != Stage::FarDrawn) return false;

    stage = Stage::None;

    ComPtr<ID3D11ShaderResourceView> gameViews[LayerSlotCount];
    ID3D11ShaderResourceView* bound[LayerSlotCount]{};
    context->PSGetShaderResources(CoverageSlot, LayerSlotCount, bound);
    for (UINT i = 0; i < LayerSlotCount; ++i) gameViews[i].Attach(bound[i]);

    std::lock_guard lock(resourceMutex);
    const bool isLens = farBlur == FarBlur::Lens;
    if (isLens) drawLens(context, call);

    ID3D11ShaderResourceView* const views[]{coverage.view.Get(), farLayer.view.Get()};
    context->PSSetShader(isLens ? lensCompositeShader.Get() : compositeShader.Get(), nullptr, 0);
    context->PSSetShaderResources(CoverageSlot, 2, views);
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    context->PSSetShader(gameShader, nullptr, 0);
    ID3D11ShaderResourceView* const restored[LayerSlotCount]{gameViews[0].Get(), gameViews[1].Get(), gameViews[2].Get()};
    context->PSSetShaderResources(CoverageSlot, LayerSlotCount, restored);

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
        compositeShader = pixelShader(device.Get(), shaders::Composite, "depth-of-field-composite");
        prefilterShader = pixelShader(device.Get(), shaders::FarPrefilter, "depth-of-field-prefilter");
        gatherShader = pixelShader(device.Get(), shaders::FarGather, "depth-of-field-gather");
        fillShader = pixelShader(device.Get(), shaders::FarFill, "depth-of-field-fill");
        // Last, so it marks a complete pipeline.
        lensCompositeShader = pixelShader(device.Get(), shaders::LensComposite, "depth-of-field-lens-composite");
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
    prefilterShader.Reset();
    gatherShader.Reset();
    fillShader.Reset();
    lensCompositeShader.Reset();
    coverage = {};
    farLayer = {};
    prefiltered = {};
    gathered = {};
    device.Reset();
}

void configure(const Configuration& configuration) noexcept
{
    wantedFarBlur = configuration.settings.depthOfField.farBlur;
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
