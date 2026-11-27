#include "features/lamp_contact_shadows/lamp_contact_shadows.hpp"

#include "constant_capture.hpp"
#include "d3d11_helpers.hpp"
#include "features/lamp_contact_shadows/lamp_contact_shadows_shaders.hpp"
#include "game_lights.hpp"
#include "log.hpp"
#include "passes/contact_shadow_trace.hpp"
#include "performance.hpp"

#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace phantom_fx::lamp_contact_shadows
{
namespace
{
using Microsoft::WRL::ComPtr;
using namespace game_lights;

constexpr UINT DepthSlot = 2;  // t2 of the light passes
constexpr UINT NormalSlot = 0; // t0 of the light passes: the G-buffer's normals
constexpr UINT SamplerSlot = 0; // s0: the shadow shader's own, for its gather
constexpr UINT BoundsSlot = 6; // b6 of the light shadow shader: the add-on's options

// A march step per two pixels of the reach, as renodx-mgsv sizes its steps to the reach, and no more
// than the shader's own loop takes.
constexpr float MinSteps = 16, MaxSteps = 64, PixelsPerStep = 2;
// The caster size the march counts within, in metres, as renodx-mgsv's spot shadows take it. Its own,
// not Contact shadow thickness: that one is a share of the depth left to the far plane, which grows
// with the distance until it passes the bound a caster may sit in and leaves no window at all - and it
// is the sun's window width, so raising it would narrow the lamp's march where it widens the sun's.
constexpr float ThicknessMetres = 0.005f;

std::atomic<bool> wantedRunning{}, pipelineFailed{};

// The light whose shadow a context's last stencil-marking draw put in the light targets' alpha, by
// its position, for the lighting draw that follows.
std::mutex markedMutex;
std::unordered_map<ID3D11DeviceContext*, std::array<float, 3>> marked;

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11PixelShader> lightShadowShader;
ComPtr<ID3D11Buffer> optionsBuffer;
// The light shadow's draw: alpha only, over the light volume the game is already drawing.
ComPtr<ID3D11BlendState> alphaOnly;
// The same draw with the game's depth test off: it writes alpha, not depth.
ComPtr<ID3D11DepthStencilState> noDepth;
// The volume's far faces only, as the game's lighting draw takes them: its marking draw culls none.
ComPtr<ID3D11RasterizerState> farFaces;
// The shadow shader's gather reads the depth buffer through this, in the slot the game leaves it.
ComPtr<ID3D11SamplerState> pointClamp;
// The game's additive light blend, by the state it binds, and the same scaled by the target's alpha.
std::unordered_map<ID3D11BlendState*, std::pair<ComPtr<ID3D11BlendState>, ComPtr<ID3D11BlendState>>> shadowedBlends;

// cLampTrace (b6) of the light shadow shader. The viewport is the depth buffer's size, which is the
// render size the reach and the march's pixel lookups are in.
struct Options
{
    float reachPixels, thickness, contrast, fadeMetres;
    float rangeMetres, steps, viewportWidth, viewportHeight;
    float useNormals; // 1 when the marking draw has the G-buffer's normals at t0, 0 when it has not
    float padding[3];
};
static_assert(sizeof(Options) % 16 == 0);

// Makes the shaders once from their precompiled bytecode (compiled from source in a dev build); call with resourceMutex held. Returns false when they are unavailable.
bool ensurePipeline()
{
    if (lightShadowShader) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
        check(device->CreateBlendState(&blend, alphaOnly.ReleaseAndGetAddressOf()), "Creating the light shadow blend");

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        check(device->CreateDepthStencilState(&depth, noDepth.ReleaseAndGetAddressOf()), "Creating the light shadow depth state");

        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_FRONT;
        rasterizer.DepthClipEnable = TRUE;
        check(device->CreateRasterizerState(&rasterizer, farFaces.ReleaseAndGetAddressOf()), "Creating the light shadow rasterizer state");

        optionsBuffer = makeConstantBuffer(device.Get(), sizeof(Options), "Creating the lamp contact shadow options");

        const D3D11_SAMPLER_DESC sampler{.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT,
            .AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
            .AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
            .AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
            .ComparisonFunc = D3D11_COMPARISON_NEVER,
            .MaxLOD = D3D11_FLOAT32_MAX};
        check(device->CreateSamplerState(&sampler, pointClamp.ReleaseAndGetAddressOf()), "Creating the lamp shadow sampler");

        // Last, so it marks a complete pipeline.
        lightShadowShader = pixelShader(device.Get(), shaders::LightShadow, "lamp-light-shadow");
        log("Lamp contact shadow shader ready.");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        lightShadowShader.Reset();
        log(std::string("Lamp contact shadows unavailable: ") + error.what());

        return false;
    }
}

// The light's position in view space.
std::array<float, 3> positionOf(const game_constants::LampLightConstants& light)
{
    return {light.position[0], light.position[1], light.position[2]};
}

// Whether a light that reaches `radius` metres from `centre` can touch the pass's screen at `size`
// pixels: its sphere is off the screen, or wholly behind the camera, where the game's own lighting
// draw reaches nothing and there is no shadow to leave. An unending reach covers the screen.
bool reachesScreen(const game_constants::SceneConstants& scene, const std::array<float, 3>& centre, float radius, std::array<int, 2> size)
{
    if (!std::isfinite(radius) || radius <= 0) return true;
    if (centre[2] + radius <= 0) return false;
    if (centre[2] <= radius) return true; // The camera plane crosses the sphere, so part of it is in view.

    return sphereOnScreen(scene, centre, radius, {static_cast<float>(size[0]), static_cast<float>(size[1])}).has_value();
}

void noteMarked(ID3D11DeviceContext* context, const std::array<float, 3>& position)
{
    std::lock_guard lock(markedMutex);
    marked[context] = position;
}

// Whether `context`'s last marking draw was for the light at `position`; it is then used up.
bool takeMarked(ID3D11DeviceContext* context, const std::array<float, 3>& position)
{
    std::lock_guard lock(markedMutex);
    const auto found = marked.find(context);
    if (found == marked.end()) return false;

    const bool same = found->second == position;
    marked.erase(found);

    return same;
}

// The game's additive light blend (ONE, ONE) with its light scaled by the target's alpha, which it
// keeps; null for any other blend. Call with resourceMutex held.
ID3D11BlendState* scaledByAlpha(ID3D11BlendState* game)
{
    if (!game) return nullptr;

    if (const auto found = shadowedBlends.find(game); found != shadowedBlends.end()) return found->second.second.Get();

    D3D11_BLEND_DESC desc{};
    game->GetDesc(&desc);
    const UINT targets = desc.IndependentBlendEnable ? 2 : 1;
    ComPtr<ID3D11BlendState> shadowed;
    for (UINT i = 0; i < targets; ++i)
    {
        auto& target = desc.RenderTarget[i];
        if (!target.BlendEnable || target.SrcBlend != D3D11_BLEND_ONE || target.DestBlend != D3D11_BLEND_ONE || target.BlendOp != D3D11_BLEND_OP_ADD)
        {
            shadowedBlends.emplace(game, std::pair{ComPtr<ID3D11BlendState>(game), ComPtr<ID3D11BlendState>()});
            return nullptr;
        }

        target.SrcBlend = D3D11_BLEND_DEST_ALPHA;
        target.SrcBlendAlpha = D3D11_BLEND_ZERO;
        target.DestBlendAlpha = D3D11_BLEND_ONE;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }

    check(device->CreateBlendState(&desc, shadowed.GetAddressOf()), "Creating a shadowed light blend");
    shadowedBlends.emplace(game, std::pair{ComPtr<ID3D11BlendState>(game), shadowed});

    return shadowed.Get();
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    if (wantedRunning) ensurePipeline();
}

void detach() noexcept
{
    {
        std::lock_guard lock(resourceMutex);
        lightShadowShader.Reset();
        alphaOnly.Reset();
        noDepth.Reset();
        farFaces.Reset();
        pointClamp.Reset();
        optionsBuffer.Reset();
        shadowedBlends.clear();
        device.Reset();
        pipelineFailed = false;
    }

    std::lock_guard lock(markedMutex);
    marked.clear();
}

void configure(const Configuration& configuration) noexcept
{
    const bool running = configuration.effects.runs(Effect::LampContactShadows);
    wantedRunning = running;
    if (!running) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

Health health()
{
    if (pipelineFailed) return Health::unavailable("Lamp contact shadows are unavailable (see PhantomFX.log).");

    return {};
}

// At the marking draw (colour writes off), the game's draw is made, then the light's volume is drawn
// once more with the shadow shader, to put the march's shadow in the alpha of both light targets
// wherever the light can reach. The second draw takes the volume's far faces, as the lighting draw
// after it does, so it covers exactly the pixels that read the alpha. The game's marking draw culls
// none, so its own state would draw both faces: twice the pixels for the same image.
bool drawMarking(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, Falloff falloff, const BoundBlend& gameBlend)
{
    const auto scene = constant_capture::pixelShaderReads<game_constants::SceneConstants>(context);
    const auto light = constant_capture::pixelShaderReads<game_constants::LampLightConstants>(context);
    if (!scene || !light)
    {
        logOnce("Shadows: a light without its constants keeps the game's shadows.");
        return false;
    }

    const auto position = positionOf(*light);

    ComPtr<ID3D11ShaderResourceView> depth;
    ComPtr<ID3D11ShaderResourceView> normals;
    context->PSGetShaderResources(DepthSlot, 1, depth.GetAddressOf());
    context->PSGetShaderResources(NormalSlot, 1, normals.GetAddressOf());
    const auto shape = contact_shadow_trace::shape();
    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;

    if (!depth)
    {
        logOnce("Shadows: a light without its depth keeps the game's shadows.");
        return false;
    }

    // A light that stands wholly past the contact shadows' fade keeps the game's draws: its shadow
    // would be lit everywhere, and so would one whose reach cannot touch the screen.
    const float range = eitherShapeRangeOf(*light, falloff);
    const auto size = contact_shadow_trace::depthSize(depth.Get());
    if (contact_shadow_trace::beyondFade(position[2], range, shape.fadeMetres)) return false;
    if (!reachesScreen(*scene, position, range, size)) return false;

    const Options options{.reachPixels = shape.reachPixels,
        .thickness = ThicknessMetres,
        .contrast = shape.contrast,
        .fadeMetres = shape.fadeMetres,
        .rangeMetres = range,
        .steps = std::clamp(shape.reachPixels / PixelsPerStep, MinSteps, MaxSteps),
        .viewportWidth = static_cast<float>(size[0]),
        .viewportHeight = static_cast<float>(size[1]),
        .useNormals = normals ? 1.f : 0.f};
    if (FAILED(write(context, optionsBuffer.Get(), options))) return false;

    static std::atomic<bool> loggedNormals{};
    if (!loggedNormals.exchange(true))
        log(normals ? "Shadows: the lamp march skips the pixels that face away from the light."
                    : "Shadows: a light without the G-buffer's normals marches every pixel it covers.");

    ComPtr<ID3D11PixelShader> gameShader;
    ComPtr<ID3D11Buffer> gameOptions;
    ComPtr<ID3D11SamplerState> gameSampler;
    ComPtr<ID3D11DepthStencilState> gameDepth;
    UINT stencilReference{};
    ComPtr<ID3D11RasterizerState> gameRasterizer;
    context->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
    context->PSGetConstantBuffers(BoundsSlot, 1, gameOptions.GetAddressOf());
    context->PSGetSamplers(SamplerSlot, 1, gameSampler.GetAddressOf());
    context->OMGetDepthStencilState(gameDepth.GetAddressOf(), &stencilReference);
    context->RSGetState(gameRasterizer.GetAddressOf());

    context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);

    performance::Timed timed(context, performance::Pass::LampShadows);
    context->PSSetShader(lightShadowShader.Get(), nullptr, 0);
    context->PSSetConstantBuffers(BoundsSlot, 1, optionsBuffer.GetAddressOf());
    context->PSSetSamplers(SamplerSlot, 1, pointClamp.GetAddressOf());
    context->OMSetBlendState(alphaOnly.Get(), gameBlend.factor, gameBlend.sampleMask);
    context->OMSetDepthStencilState(noDepth.Get(), 0);
    context->RSSetState(farFaces.Get());
    context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
    timed.end();

    context->PSSetShader(gameShader.Get(), nullptr, 0);
    context->PSSetConstantBuffers(BoundsSlot, 1, gameOptions.GetAddressOf());
    context->PSSetSamplers(SamplerSlot, 1, gameSampler.GetAddressOf());
    context->OMSetBlendState(gameBlend.state.Get(), gameBlend.factor, gameBlend.sampleMask);
    context->OMSetDepthStencilState(gameDepth.Get(), stencilReference);
    context->RSSetState(gameRasterizer.Get());
    noteMarked(context, position);

    return true;
}

ID3D11BlendState* takeShadowedBlend(ID3D11DeviceContext* context, ID3D11BlendState* gameBlend)
{
    const auto light = constant_capture::pixelShaderReads<game_constants::LampLightConstants>(context);
    if (!light || !takeMarked(context, positionOf(*light))) return nullptr;

    std::lock_guard lock(resourceMutex);
    auto* shadowed = scaledByAlpha(gameBlend);
    if (!shadowed) logOnce("Shadows: a light with another blend than (one, one) keeps the game's shadows.");

    return shadowed;
}

void contextFinished(ID3D11DeviceContext* context) noexcept
{
    std::lock_guard lock(markedMutex);
    marked.erase(context);
}

void present() noexcept
{
    std::lock_guard lock(markedMutex);
    marked.clear();
}
}
