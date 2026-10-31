#include "features/depth_of_field/depth_of_field.hpp"

#include "d3d11_helpers.hpp"
#include "features/depth_of_field/depth_of_field_shaders.hpp"
#include "frame_latch.hpp"
#include "log.hpp"

#include <wrl/client.h>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>

namespace phantom_fx::depth_of_field
{
namespace
{
using game_shaders::GamePass;
using Microsoft::WRL::ComPtr;

constexpr UINT DepthSlot = 1;      // DOF_ScatterBakeFirst's inDepth
constexpr UINT CoverageSlot = 2;   // The first of Layers' slots, after the game's t0 and t1
constexpr UINT LayerSlotCount = 3; // t2 to t4

// What these shaders read beside the game's t0 and t1, in their slots.
struct Layers
{
    ID3D11ShaderResourceView* coverage{};
    ID3D11ShaderResourceView* light{};
    ID3D11ShaderResourceView* radii{}; // The prefiltered layer's alpha, for the fill
};

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11PixelShader> coverageShader, compositeShader, prefilterShader, gatherShader, wideFillShader, fineFillShader, lensCompositeShader;
DrawTarget coverage, farLayer, prefiltered, gathered, filled;

// How far this thread has come through the depth of field's draws, which it records together. Each step
// needs the one before, so a frame that misses one is the game's own. The far draw settles the frame's
// blur: FarDrawn the game's, LensReady the lens's.
enum class Stage : std::uint8_t { None, Baked, FarDrawn, LensReady };
thread_local Stage stage{};

FrameLatch<FarBlur> latch;

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

void bindLayers(ID3D11DeviceContext* context, const Layers& layers)
{
    ID3D11ShaderResourceView* const views[LayerSlotCount]{layers.coverage, layers.light, layers.radii};
    context->PSSetShaderResources(CoverageSlot, LayerSlotCount, views);
}

// The draw alone in `into`, unblended.
void drawInto(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, const DrawTarget& into)
{
    const SavedOutput saved(context);
    const WholeTargetViewport viewport(context, into.width, into.height);
    context->OMSetRenderTargets(1, into.target.GetAddressOf(), nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
}

// The game's constants and depth are bound for its own bake, which draws after.
void drawCoverage(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, ID3D11PixelShader* gameShader)
{
    stage = Stage::None;

    ComPtr<ID3D11ShaderResourceView> depth;
    context->PSGetShaderResources(DepthSlot, 1, depth.GetAddressOf());
    const auto [width, height] = sizeOf(depth.Get());
    std::lock_guard lock(resourceMutex);

    if (!width || !lensCompositeShader) return;

    ensureDrawTarget(device.Get(), coverage, width, height, DXGI_FORMAT_R16G16_FLOAT, "Creating the depth of field's cover");
    context->PSSetShader(coverageShader.Get(), nullptr, 0);
    drawInto(context, call, coverage);
    context->PSSetShader(gameShader, nullptr, 0);
    stage = Stage::Baked;
}

// Call with resourceMutex held.
void ensureLensTargets()
{
    const UINT width = coverage.width / 2, height = coverage.height / 2;
    constexpr DXGI_FORMAT Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ensureDrawTarget(device.Get(), prefiltered, width, height, Format, "Creating the depth of field's prefiltered light");
    ensureDrawTarget(device.Get(), gathered, width, height, Format, "Creating the depth of field's gather");
    ensureDrawTarget(device.Get(), filled, width, height, Format, "Creating the depth of field's fill");
    ensureDrawTarget(device.Get(), farLayer, width, height, Format, "Creating the depth of field's far layer");
}

// The game blends the far layer under the near one it drew; apart, its target keeps the near layer alone.
// A target that cannot be made throws while the stage is still Baked, so the game draws this and the rest.
bool drawFarLayer(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call)
{
    if (stage != Stage::Baked) return false;

    std::lock_guard lock(resourceMutex);

    // The lens makes its own far blur, so all it wants of this draw is the near layer left without it.
    if (latch.frame() == FarBlur::Lens)
    {
        ensureLensTargets();
        stage = Stage::LensReady;

        return true;
    }

    ComPtr<ID3D11RenderTargetView> gameTarget;
    context->OMGetRenderTargets(1, gameTarget.GetAddressOf(), nullptr);
    const auto [width, height] = sizeOf(gameTarget.Get());
    ensureDrawTarget(device.Get(), farLayer, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the depth of field's far layer");
    drawInto(context, call, farLayer);
    stage = Stage::FarDrawn;

    return true;
}

// One half-size pass into `into`. The slots are emptied first, as the last pass's input may be this one's
// target, and the inputs go in after the target, as a view of a bound target reads as nothing.
void drawLayer(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, const DrawTarget& into, ID3D11PixelShader* shader, const Layers& inputs)
{
    bindLayers(context, {});
    context->OMSetRenderTargets(1, into.target.GetAddressOf(), nullptr);
    bindLayers(context, inputs);
    context->PSSetShader(shader, nullptr, 0);
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
}

// Call with resourceMutex held. The composite's t0, t1 and constants stay bound throughout.
void drawLens(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call)
{
    const SavedOutput saved(context);
    const WholeTargetViewport viewport(context, prefiltered.width, prefiltered.height);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    drawLayer(context, call, prefiltered, prefilterShader.Get(), {.coverage = coverage.view.Get()});
    drawLayer(context, call, gathered, gatherShader.Get(), {.light = prefiltered.view.Get()});
    drawLayer(context, call, filled, wideFillShader.Get(), {.light = gathered.view.Get(), .radii = prefiltered.view.Get()});
    drawLayer(context, call, farLayer, fineFillShader.Get(), {.light = filled.view.Get(), .radii = prefiltered.view.Get()});
    bindLayers(context, {});
}

bool composite(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, ID3D11PixelShader* gameShader)
{
    if (stage != Stage::FarDrawn && stage != Stage::LensReady) return false;

    const bool isLens = stage == Stage::LensReady;
    stage = Stage::None;

    ComPtr<ID3D11ShaderResourceView> gameViews[LayerSlotCount];
    ID3D11ShaderResourceView* bound[LayerSlotCount]{};
    context->PSGetShaderResources(CoverageSlot, LayerSlotCount, bound);
    for (UINT i = 0; i < LayerSlotCount; ++i) gameViews[i].Attach(bound[i]);

    std::lock_guard lock(resourceMutex);
    if (isLens) drawLens(context, call);

    context->PSSetShader(isLens ? lensCompositeShader.Get() : compositeShader.Get(), nullptr, 0);
    bindLayers(context, {.coverage = coverage.view.Get(), .light = farLayer.view.Get()});
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    context->PSSetShader(gameShader, nullptr, 0);
    bindLayers(context, {gameViews[0].Get(), gameViews[1].Get(), gameViews[2].Get()});

    return true;
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    try
    {
        const D3D_SHADER_MACRO wideFill[]{{"RING_WIDTH", "0.105"}, {}}, fineFill[]{{"RING_WIDTH", "0.05"}, {}};
        coverageShader = pixelShader(device.Get(), shaders::Coverage, "depth-of-field-coverage");
        compositeShader = pixelShader(device.Get(), shaders::Composite, "depth-of-field-composite");
        prefilterShader = pixelShader(device.Get(), shaders::FarPrefilter, "depth-of-field-prefilter");
        gatherShader = pixelShader(device.Get(), shaders::FarGather, "depth-of-field-gather");
        wideFillShader = pixelShader(device.Get(), shaders::FarFill, "depth-of-field-wide-fill", wideFill);
        fineFillShader = pixelShader(device.Get(), shaders::FarFill, "depth-of-field-fine-fill", fineFill);
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
    wideFillShader.Reset();
    fineFillShader.Reset();
    lensCompositeShader.Reset();
    coverage = {};
    farLayer = {};
    prefiltered = {};
    gathered = {};
    filled = {};
    device.Reset();
}

void configure(const Configuration& configuration) noexcept
{
    latch.set(configuration.settings.depthOfField.farBlur);
}

void present() noexcept
{
    latch.present();
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
            drawCoverage(context, call, shader.object());
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
