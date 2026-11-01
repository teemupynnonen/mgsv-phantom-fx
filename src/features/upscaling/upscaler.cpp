#include "features/upscaling/upscaler.hpp"

#include "camera.hpp"
#include "clock.hpp"
#include "command_list_notes.hpp"
#include "d3d11_helpers.hpp"
#include "features/hdr/hdr.hpp"
#include "features/motion_blur/motion_blur.hpp"
#include "features/photo_mode/focus.hpp"
#include "features/upscaling/frames.hpp"
#include "features/path_tracing/glass.hpp"
#include "features/upscaling/fsr_upscaler.hpp"
#include "features/upscaling/fxaa.hpp"
#include "features/upscaling/ray_reconstruction.hpp"
#include "features/upscaling/render_size_plan.hpp"
#include "features/upscaling/upscaler_shaders.hpp"
#include "game_constants.hpp"
#include "game_loop.hpp"
#include "log.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "performance.hpp"
#include "render_scale.hpp"

#include <Windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace phantom_fx::upscaler
{
namespace
{
using Microsoft::WRL::ComPtr;
using game_shaders::GamePass, game_shaders::GameShader, game_shaders::GameShaders;

// FXAA's shader: the status says whether the game has made it.
constexpr const GameShader& FxaaShader = *std::ranges::find(GameShaders, GamePass::Fxaa, &GameShader::pass);

// The FXAA pass as recorded: it reads the scene copy and writes the scene target, with the depth copy
// bound. The upscaler reads the scene target and the depth copy before the grading.
struct FxaaPass
{
    ComPtr<ID3D11RenderTargetView> sceneTarget;
    ComPtr<ID3D11DepthStencilView> depth;
    std::uint64_t frame{};
};

// The engine's velocity for dynamic meshes, as MotionBlurCameraVelocity reads it.
struct ObjectVelocity
{
    ComPtr<ID3D11ShaderResourceView> view;
    std::uint64_t frame{};
};

// Frame generation's frame before the HUD, at the output size, as the back buffer would hold it: the
// graded image is copied into `source` before the UI draws over it, and GammaControl draws a second
// time from it into `target`, which Direct3D 12 shares. Its colour then matches the back buffer's
// wherever there is no HUD, which is how frame generation tells the HUD apart.
struct Hudless
{
    unsigned width{}, height{};
    ComPtr<ID3D11Texture2D> source, target;
    ComPtr<ID3D11ShaderResourceView> sourceView; // As GammaControl reads the scene target: sRGB, or float
    ComPtr<ID3D11RenderTargetView> targetView;   // As GammaControl writes the back buffer: sRGB, or R10G10B10A2 with HDR
};

// What a deferred context or command list recorded that the replacement needs.
struct Recorded
{
    // Holds the grading: the upscaler runs before the frame's first such list.
    bool grading{};
    // Holds the FXAA pass, the end of the grading; the hudless colour is copied after it.
    bool fxaa{};
    std::optional<ObjectVelocity> velocity;
    // Holds the depth of field's composite for this frame (Ray Reconstruction's depth of field guide).
    std::optional<std::uint64_t> dofFrame;
    // Draws on the UI target, which must be filled before the list runs.
    bool drawsOnUiTarget{};
    // Draws GammaControl into this hudless texture.
    std::shared_ptr<const Hudless> hudless;

    void merge(Recorded later)
    {
        grading = grading || later.grading;
        fxaa = fxaa || later.fxaa;
        if (later.velocity) velocity = std::move(later.velocity);
        if (later.dofFrame) dofFrame = later.dofFrame;
        drawsOnUiTarget = drawsOnUiTarget || later.drawsOnUiTarget;
        if (later.hudless) hudless = std::move(later.hudless);
    }
};

struct Target
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11UnorderedAccessView> access;
    UINT width{}, height{};
    bool shared{}; // Direct3D 12 opens it: FSR or frame generation reads it
};

struct InputConstants
{
    float previousFromCurrent[4][4];
    float jitterNdc[2];
    std::uint32_t renderSize[2];
    std::uint32_t objectMotion, linear, dofGuide, padding;
};

struct CompositeConstants
{
    std::uint32_t showMotion, showObjectMotion, showProbe;
    float sharpness;
    std::uint32_t objectMotion, linear, padding[2];
};

static_assert(static_cast<int>(DlssQuality::MaxPerformance) == NVSDK_NGX_PerfQuality_Value_MaxPerf
        && static_cast<int>(DlssQuality::Balanced) == NVSDK_NGX_PerfQuality_Value_Balanced
        && static_cast<int>(DlssQuality::MaxQuality) == NVSDK_NGX_PerfQuality_Value_MaxQuality
        && static_cast<int>(DlssQuality::UltraPerformance) == NVSDK_NGX_PerfQuality_Value_UltraPerformance
        && static_cast<int>(DlssQuality::UltraQuality) == NVSDK_NGX_PerfQuality_Value_UltraQuality
        && static_cast<int>(DlssQuality::Dlaa) == NVSDK_NGX_PerfQuality_Value_DLAA,
    "DlssQuality's values are NGX's.");

// What the DLSS feature was created for; any change makes a new one, with a new history. Not the plan's
// settings: a custom scale that rounds to the same size keeps the history.
struct FeatureKey
{
    render_scale::Size render, output;
    DlssQuality quality{DlssQuality::Dlaa};
    Preset preset{};

    bool operator==(const FeatureKey&) const = default;
};

struct Ngx
{
    bool initialized{};
    NVSDK_NGX_Parameter* parameters{};
    NVSDK_NGX_Handle* feature{};
    FeatureKey key{};
};

// ---- Shared state ----
std::filesystem::path dataDirectory; // The add-on's: the DLSS and FSR DLLs

// What configure gave: the upscaler's settings and the diagnostics, and what a cost measurement
// leaves out.
struct Configured
{
    Upscaling upscaling;
    Diagnostics diagnostics;
    bool supportedBuild{}, upscalerLeftOut{}, sharpeningLeftOut{};
};
std::mutex configuredMutex;
Configured configured;

// Read on every recording thread: DLSS replaces FXAA this frame.
std::atomic<bool> replacing{};
// The game presents through Direct3D 12 for frame generation.
std::atomic<bool> frameGeneration{};
// The current plan's jitter phases and texture LOD bias.
std::atomic<int> jitterPhases{8};
std::atomic<float> targetMipBias{};
// The last DLSS frame was Ray Reconstruction's; why Ray Reconstruction failed, after which DLSS runs
// for the session.
std::atomic<bool> reconstructing{};
std::optional<std::string> reconstructionFailure;

// Frame generation's hudless texture, made at a present for the output size; GammaControl's
// recording threads read it.
std::mutex hudlessMutex;
std::shared_ptr<const Hudless> hudless;
bool hudlessFailed{};

// Biased copies of the game's material samplers, by the game's sampler and the bias; read on every
// recording thread.
struct BiasedSampler
{
    ComPtr<ID3D11SamplerState> original, biased;
};
std::shared_mutex samplerMutex;
std::map<std::pair<ID3D11SamplerState*, float>, BiasedSampler> biasedSamplers;

// ---- Recording ----
CommandListNotes<Recorded> recordedLists;

// Taken after executionMutex where both are held.
std::mutex recordMutex;
// The latest FXAA pass recorded. The whole frame is recorded before it executes, so the frame's own
// is known when its grading starts.
std::optional<FxaaPass> recordedFxaa;
Frames frames;
// The scene constants a thread has mapped: Map, memcpy and Unmap run back to back on one thread.
thread_local ID3D11Resource* mappedScene{};
thread_local void* mappedSceneData{};

// Set while the replacement issues its own Direct3D calls through the game's context.
thread_local int bypass{};

struct Bypass
{
    Bypass() { ++bypass; }
    ~Bypass() { --bypass; }
    Bypass(const Bypass&) = delete;
    Bypass& operator=(const Bypass&) = delete;
};

// ---- Execution, on the thread running the immediate context ----
std::mutex executionMutex;
ComPtr<ID3D11Device> device, ngxDevice;
ComPtr<ID3D11DeviceContext> immediate, ngxContext;
ComPtr<ID3D11DeviceContext1> context1;
ComPtr<ID3DDeviceContextState> ownState;
ComPtr<ID3D11ComputeShader> inputShader;
ComPtr<ID3D11PixelShader> compositeShader;
ComPtr<ID3D11Buffer> inputConstants, compositeConstants;
ComPtr<ID3D11Texture2D> exposureTexture; // 1x1, the constant 1 DLSS is told as the exposure
Target colorTarget, depthTarget, motionTarget, outputTarget;
Target dofGuideTarget; // Ray Reconstruction's depth of field guide
ComPtr<ID3D11Resource> depthResource, sceneResource;
ComPtr<ID3D11ShaderResourceView> depthView, sceneView;
Ngx ngx;
UINT outputSize[2]{};
std::optional<RenderSizePlan> plan;
// The UI target holds this frame's image: the grading, the 2D pass and GammaControl can draw on it.
bool uiTargetFilled{};
// The frame the upscaler ran for, at its first command list of the grading.
std::uint64_t upscaledFrame{};
// Frame generation's hudless colour comes from `image` once the grading has drawn it: at the
// immediate context's next call after the FXAA pass's command list (`copyNext`).
struct PendingHudless
{
    std::shared_ptr<const Hudless> hudless;
    ComPtr<ID3D11Resource> image;
    bool copyNext{};
};
std::optional<PendingHudless> pendingHudless;
std::optional<ObjectVelocity> velocity;
// The frame the depth of field's composite last drew for.
std::optional<std::uint64_t> dofFrame;
std::optional<Frame> lastEvaluated;
LONGLONG lastEvaluation{};

// What frame generation gets from the frame being presented, filled as it executes.
struct Presented
{
    frame_generation::Inputs inputs;        // From DLSS's evaluation; not yet valid
    std::shared_ptr<const Hudless> hudless; // The graded image went into its source
    bool hudlessDrawn{};                    // A list drew GammaControl from it after that
} presented;

std::atomic<bool> failed{};
std::string failure, lastFallback;
std::uint64_t presents{}, lastEvaluatedPresent{}, evaluations{}, fallbacks{}, resets{}, cameraCuts{};
// Contexts' and lists' notes never handed over (CommandListNotes::present), for the diagnostic log.
std::uint64_t expiredNotes{};
Configured currentConfiguration()
{
    std::lock_guard lock(configuredMutex);
    return configured;
}

// Call with configuredMutex held.
void updateReplacing()
{
    replacing = configured.supportedBuild && !failed && configured.upscaling.mode != Mode::Off && !configured.diagnostics.gameFxaa
        && !configured.upscalerLeftOut;
    motion_blur::setVelocityOnly(replacing);
    fxaa::setForced(replacing);
}

// A frame DLSS cannot run on shows the engine's image as it is, without anti-aliasing.
bool fallback(std::string reason)
{
    ++fallbacks;
    logOnce("Not upscaled this frame: " + reason + ".");
    lastFallback = std::move(reason);
    return false;
}

void fail(std::string_view reason)
{
    failed = true;
    failure = reason;
    {
        std::lock_guard lock(configuredMutex);
        updateReplacing();
    }

    log("Upscaling stopped: " + failure);
}

// ---- Recording ----

std::uint64_t keyOf(ID3D11DeviceContext* context)
{
    return reinterpret_cast<std::uint64_t>(context);
}

void recordFxaa(ID3D11DeviceContext* context)
{
    FxaaPass pass;
    context->OMGetRenderTargets(1, pass.sceneTarget.GetAddressOf(), pass.depth.GetAddressOf());
    {
        std::lock_guard lock(recordMutex);
        pass.frame = frames.recordingFrame();
        recordedFxaa = std::move(pass);
    }

    recordedLists.note(keyOf(context), [](Recorded& notes) { notes.fxaa = true; });
}

void recordGrading(ID3D11DeviceContext* context)
{
    recordedLists.note(keyOf(context), [](Recorded& notes) { notes.grading = true; });
}

void recordVelocity(ID3D11DeviceContext* context)
{
    ObjectVelocity found;
    context->PSGetShaderResources(3, 1, found.view.GetAddressOf());
    {
        std::lock_guard lock(recordMutex);
        found.frame = frames.recordingFrame();
    }

    recordedLists.note(keyOf(context), [&](Recorded& notes) { notes.velocity = std::move(found); });
}

void recordDofComposite(ID3D11DeviceContext* context)
{
    std::uint64_t frame{};
    {
        std::lock_guard lock(recordMutex);
        frame = frames.recordingFrame();
    }

    recordedLists.note(keyOf(context), [&](Recorded& notes) { notes.dofFrame = frame; });
}

void recordUiTargetUse(ID3D11DeviceContext* context)
{
    recordedLists.note(keyOf(context), [](Recorded& notes) { notes.drawsOnUiTarget = true; });
}

// In exclusive fullscreen the game draws GammaControl into a viewport of its scene's size, centred in
// the back buffer (docs/DEVELOPMENT.md, "Render size"), so a scene below the output size shows 1:1 in a
// box. Returns the target's size when the bound draw does that.
std::optional<std::pair<UINT, UINT>> boxedGammaTarget(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> scene;
    context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
    context->PSGetShaderResources(0, 1, scene.GetAddressOf());
    const auto [width, height] = sizeOf(target.Get());
    const auto [sceneWidth, sceneHeight] = sizeOf(scene.Get());
    if (!sceneWidth || !sceneHeight || sceneWidth >= width || sceneHeight >= height) return std::nullopt;

    UINT count = 1;
    D3D11_VIEWPORT viewport{};
    context->RSGetViewports(&count, &viewport);
    if (viewport.Width != static_cast<float>(sceneWidth) || viewport.Height != static_cast<float>(sceneHeight)) return std::nullopt;

    return std::pair{width, height};
}

// GammaControl draws once more, from DLSS's image before the HUD into frame generation's hudless
// texture, with the game's own constants and sampler.
void drawHudless(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call)
{
    if (!frameGeneration.load(std::memory_order_relaxed)) return;

    std::shared_ptr<const Hudless> target;
    {
        std::lock_guard lock(hudlessMutex);
        target = hudless;
    }

    if (!target) return;

    ComPtr<ID3D11RenderTargetView> backBuffer;
    ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(1, backBuffer.GetAddressOf(), depth.GetAddressOf());
    const auto output = textureDesc(backBuffer.Get());
    const auto format = hdr::active() ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (output.Width != target->width || output.Height != target->height || output.Format != format) return;

    ComPtr<ID3D11ShaderResourceView> scene;
    context->PSGetShaderResources(0, 1, scene.GetAddressOf());
    context->PSSetShaderResources(0, 1, target->sourceView.GetAddressOf());
    context->OMSetRenderTargets(1, target->targetView.GetAddressOf(), nullptr);
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    context->OMSetRenderTargets(1, backBuffer.GetAddressOf(), depth.Get());
    context->PSSetShaderResources(0, 1, scene.GetAddressOf());

    recordedLists.note(keyOf(context), [&](Recorded& notes) { notes.hudless = std::move(target); });
}

// ---- Resources ----

ComPtr<ID3D11Buffer> constantBuffer(UINT size)
{
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = size;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> buffer;
    check(device->CreateBuffer(&desc, nullptr, buffer.GetAddressOf()), "Creating a DLSS constant buffer");
    return buffer;
}

void ensurePipeline()
{
    if (ownState) return;

    inputShader = computeShader(device.Get(), shaders::Inputs, "dlss-inputs");
    compositeShader = pixelShader(device.Get(), shaders::Composite, "dlss-composite");
    inputConstants = constantBuffer(sizeof(InputConstants));
    compositeConstants = constantBuffer(sizeof(CompositeConstants));

    // A separate context state lets DLSS run between the game's command lists without touching the
    // state the immediate context carries.
    ComPtr<ID3D11Device1> device1;
    check(device.As(&device1), "Finding Direct3D 11.1");
    const auto level = device->GetFeatureLevel();
    const UINT flags =
        (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    check(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, ownState.GetAddressOf()),
        "Creating the DLSS device context state");
}

void ensureTarget(Target& target, UINT width, UINT height, DXGI_FORMAT format, const char* name, bool shared = false)
{
    if (target.texture && target.width == width && target.height == height && target.shared == shared) return;

    target = {};
    auto made = makeComputeTexture(device.Get(), width, height, format, name, shared ? SharedWithD3D12 : 0);
    target = {std::move(made.texture), std::move(made.view), std::move(made.access), width, height, shared};
}

// Frame generation's hudless texture for the output size, made at a present before the frames that
// draw it. Holders of the previous one keep it until they are done.
void ensureHudless()
{
    if (!frameGeneration || !device || !outputSize[0] || !outputSize[1]) return;

    std::lock_guard lock(hudlessMutex);
    if (hudlessFailed || (hudless && hudless->width == outputSize[0] && hudless->height == outputSize[1])) return;

    auto created = std::make_shared<Hudless>();
    created->width = outputSize[0];
    created->height = outputSize[1];
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = created->width;
    desc.Height = created->height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    // The source takes the scene target's format, so DLSS's image copies into it.
    const bool hdrOn = hdr::active();
    desc.Format = hdr::sceneFormat();
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC sourceView{hdr::isSceneFloat() ? desc.Format : DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, D3D11_SRV_DIMENSION_TEXTURE2D};
    sourceView.Texture2D.MipLevels = 1;
    // The target holds the back buffer's bits; Streamline reads it as R8G8B8A8_UNORM, as the real swap
    // chain holds them, or R10G10B10A2_UNORM with HDR (frame_generation.cpp).
    D3D11_TEXTURE2D_DESC target = desc;
    target.Format = hdrOn ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_TYPELESS;
    target.BindFlags = D3D11_BIND_RENDER_TARGET;
    target.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    const D3D11_RENDER_TARGET_VIEW_DESC targetView{hdrOn ? target.Format : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D11_RTV_DIMENSION_TEXTURE2D};
    if (FAILED(device->CreateTexture2D(&desc, nullptr, created->source.GetAddressOf()))
        || FAILED(device->CreateShaderResourceView(created->source.Get(), &sourceView, created->sourceView.GetAddressOf()))
        || FAILED(device->CreateTexture2D(&target, nullptr, created->target.GetAddressOf()))
        || FAILED(device->CreateRenderTargetView(created->target.Get(), &targetView, created->targetView.GetAddressOf())))
    {
        hudless.reset();
        hudlessFailed = true;
        log("Could not create the hudless texture; frame generation stays paused.");

        return;
    }

    hudless = std::move(created);
    log("Created the hudless texture at " + std::to_string(outputSize[0]) + "x" + std::to_string(outputSize[1]) + " for frame generation.");
}

// The colour DLSS receives has the game's white at 1, tone-mapped or, in the float scene, linear light, so the
// exposure it is told is a constant 1. Handing that over as a texture, rather than leaving it out, keeps add-ons that read the
// evaluation (OptiScaler) from turning on DLSS's auto-exposure.
void ensureExposure()
{
    if (exposureTexture) return;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    constexpr float exposure = 1.0f;
    const D3D11_SUBRESOURCE_DATA data{&exposure, sizeof(exposure), sizeof(exposure)};
    check(device->CreateTexture2D(&desc, &data, exposureTexture.GetAddressOf()), "Creating the DLSS exposure texture");
}

// The depth copy the forward passes add to is only ever a depth target in the game, so it gets its own
// shader view. False when the game made it without one.
bool ensureDepthView(ID3D11DepthStencilView* depth)
{
    const auto resource = resourceOf(depth);
    if (resource == depthResource) return depthView != nullptr;

    depthResource = resource;
    depthView.Reset();
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc{};
    if (FAILED(resource.As(&texture))) return false;

    texture->GetDesc(&desc);
    if (!(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) || desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    viewDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.MipLevels = 1;

    return SUCCEEDED(device->CreateShaderResourceView(texture.Get(), &viewDesc, depthView.GetAddressOf()));
}

// The scene target, read as FXAA's view writes it: sRGB-encoded in 8 bits, or float.
bool ensureSceneView(ID3D11RenderTargetView* target)
{
    const auto resource = resourceOf(target);
    if (resource == sceneResource) return sceneView != nullptr;

    sceneResource = resource;
    sceneView.Reset();
    D3D11_RENDER_TARGET_VIEW_DESC targetDesc{};
    target->GetDesc(&targetDesc);
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    viewDesc.Format = targetDesc.Format;
    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.MipLevels = 1;

    return SUCCEEDED(device->CreateShaderResourceView(resource.Get(), &viewDesc, sceneView.GetAddressOf()));
}

// ---- NGX ----

void NVSDK_CONV ngxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    std::string text(message ? message : "");
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    log("NGX: " + text);
}

void releaseFeature()
{
    if (ngx.feature) NVSDK_NGX_D3D11_ReleaseFeature(ngx.feature);
    ngx.feature = nullptr;
    ngx.key = {};
}

void releaseNgx()
{
    releaseFeature();
    if (ngx.parameters) NVSDK_NGX_D3D11_DestroyParameters(ngx.parameters);
    ngx.parameters = nullptr;
    if (ngx.initialized && ngxDevice) NVSDK_NGX_D3D11_Shutdown1(ngxDevice.Get());
    ngx.initialized = false;
}

// Throws with a message for the player when DLSS cannot run on this PC.
void initializeNgx()
{
    if (ngx.initialized) return;

    // nvngx_dlss.dll sits beside the add-on, in the data directory.
    const auto modelDirectory = dataDirectory.wstring();
    const wchar_t* paths[]{modelDirectory.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = ngxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    auto result = NVSDK_NGX_D3D11_Init_with_ProjectID(frame_generation::NgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "Fox Engine 1.0.15", dataDirectory.c_str(),
        ngxDevice.Get(), &info);
    if (NVSDK_NGX_FAILED(result))
        throw std::runtime_error("NVIDIA NGX could not start (" + hex(static_cast<std::uint32_t>(result))
            + "). DLSS needs an NVIDIA RTX GPU and a current driver.");

    ngx.initialized = true;
    int available{};
    result = NVSDK_NGX_D3D11_GetCapabilityParameters(&ngx.parameters);
    if (NVSDK_NGX_SUCCEED(result)) NVSDK_NGX_Parameter_GetI(ngx.parameters, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (!available)
    {
        int needsDriver{};
        unsigned major{}, minor{};
        if (ngx.parameters)
        {
            NVSDK_NGX_Parameter_GetI(ngx.parameters, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
            NVSDK_NGX_Parameter_GetUI(ngx.parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
            NVSDK_NGX_Parameter_GetUI(ngx.parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
        }

        releaseNgx();
        throw std::runtime_error(needsDriver ? "DLSS needs NVIDIA driver " + std::to_string(major) + "." + std::to_string(minor) + " or newer."
                                             : std::string("DLSS is unavailable on this GPU. nvngx_dlss.dll must be beside the add-on."));
    }

    log("NGX initialized; DLSS is available. Model directory: " + dataDirectory.string());
}

// Throws with a message for the player when the chosen upscaler cannot run on this PC.
void initializeUpscaler(Upscaler upscaler)
{
    if (upscaler == Upscaler::Fsr) fsr_upscaler::start(device.Get(), dataDirectory);
    else initializeNgx();
}

// The upscaler not chosen gives back its memory, and both do while upscaling is off. The comparison
// with FXAA keeps the chosen one for a quick switch.
void releaseUnused(const Upscaling& wanted)
{
    if (wanted.mode == Mode::Off || wanted.upscaler != Upscaler::Dlss) releaseFeature();
    if (wanted.mode == Mode::Off || wanted.upscaler != Upscaler::Dlss || !wanted.rayReconstruction) ray_reconstruction::release();
    if (wanted.mode == Mode::Off || wanted.upscaler != Upscaler::Fsr) fsr_upscaler::stop();
}

// Ray Reconstruction runs for this frame: asked for, with DLSS, and with the guides the ray-traced
// scene's kick made (ray_tracing::reconstructionGuides). Starts it the first time; a PC it cannot start
// on keeps DLSS for the session.
std::optional<ray_tracing::ReconstructionGuides> reconstructionGuides(const Upscaling& wanted, UINT width, UINT height)
{
    if (wanted.upscaler != Upscaler::Dlss || !wanted.rayReconstruction || reconstructionFailure) return std::nullopt;

    auto guides = ray_tracing::reconstructionGuides(width, height);
    if (!guides || ray_reconstruction::started()) return guides;

    try
    {
        ray_reconstruction::start(device.Get(), dataDirectory);
    }
    catch (const std::exception& error)
    {
        reconstructionFailure = error.what();
        log("Ray Reconstruction is unavailable; DLSS runs instead: " + *reconstructionFailure);
        return std::nullopt;
    }

    return guides;
}

// DLSS's own optimal render size for the mode at `output`; none when NGX has none.
std::optional<render_scale::Size> dlssOptimalSize(const Upscaling& wanted, render_scale::Size output)
{
    const auto quality = dlssQuality(wanted.mode, nominalScale(wanted.upscaler, wanted.mode, wanted.customRenderScale));
    render_scale::Size size;
    UINT maxWidth{}, maxHeight{}, minWidth{}, minHeight{};
    float sharpness{};
    if (NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(ngx.parameters, output.width, output.height, static_cast<NVSDK_NGX_PerfQuality_Value>(quality),
            &size.width, &size.height, &maxWidth, &maxHeight, &minWidth, &minHeight, &sharpness)))
        return std::nullopt;

    return size;
}

// Returns true when the feature was created, which starts a new history.
bool ensureFeature(const RenderSizePlan& target, const Upscaling& wanted)
{
    const FeatureKey key{target.render, target.output, target.quality, wanted.dlssPreset};
    if (ngx.feature && ngx.key == key) return false;

    releaseFeature();
    for (const char* hint : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
        NVSDK_NGX_Parameter_SetUI(ngx.parameters, hint, static_cast<unsigned>(wanted.dlssPreset));

    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = target.render.width;
    create.Feature.InHeight = target.render.height;
    create.Feature.InTargetWidth = target.output.width;
    create.Feature.InTargetHeight = target.output.height;
    create.Feature.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(target.quality);
    // The motion vectors are per render pixel; reversed-Z depth; the colour is LDR, or in the float scene
    // linear light, the game's white at 1.
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (hdr::isSceneFloat()) create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    const auto result = NGX_D3D11_CREATE_DLSS_EXT(ngxContext.Get(), &ngx.feature, ngx.parameters, &create);
    if (NVSDK_NGX_FAILED(result))
    {
        ngx.feature = nullptr;
        throw std::runtime_error("Creating the DLSS feature failed (" + hex(static_cast<std::uint32_t>(result)) + ").");
    }

    ngx.key = key;
    log("Created DLSS " + std::string(label(wanted.mode, Upscaler::Dlss)) + ": " + std::to_string(target.render.width) + "x" + std::to_string(target.render.height)
        + " -> " + std::to_string(target.output.width) + "x" + std::to_string(target.output.height) + ", preset "
        + std::string(label(wanted.dlssPreset)) + ".");

    return true;
}

// The SDK's NGX_D3D11_EVALUATE_DLSS_EXT ends in NVSDK_NGX_D3D11_EvaluateFeature_C. Add-ons that
// follow a game's DLSS (RenoDX hooks the non-_C entry point) only see evaluations made through
// NVSDK_NGX_D3D11_EvaluateFeature, so set the same parameters and call that.
NVSDK_NGX_Result evaluateDlss(const NVSDK_NGX_D3D11_DLSS_Eval_Params& evaluation)
{
    auto* parameters = ngx.parameters;
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_Color, evaluation.Feature.pInColor);
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_Output, evaluation.Feature.pInOutput);
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_Depth, evaluation.pInDepth);
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_MotionVectors, evaluation.pInMotionVectors);
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_TransparencyMask, nullptr);
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_ExposureTexture, evaluation.pInExposureTexture);
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, nullptr);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_Jitter_Offset_X, evaluation.InJitterOffsetX);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_Jitter_Offset_Y, evaluation.InJitterOffsetY);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_Sharpness, 0.0f);
    NVSDK_NGX_Parameter_SetI(parameters, NVSDK_NGX_Parameter_Reset, evaluation.InReset);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_MV_Scale_X, evaluation.InMVScaleX);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_MV_Scale_Y, evaluation.InMVScaleY);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, evaluation.InFrameTimeDeltaInMsec);
    for (const char* base : {NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
             NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
             NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
             NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y})
        NVSDK_NGX_Parameter_SetUI(parameters, base, 0);
    NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, evaluation.InRenderSubrectDimensions.Width);
    NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, evaluation.InRenderSubrectDimensions.Height);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);

    return NVSDK_NGX_D3D11_EvaluateFeature(ngxContext.Get(), ngx.feature, parameters, nullptr);
}

// ---- Post processing at the output size, on the recording threads ----

using Inputs = std::initializer_list<ui_redirect::OutputSizeDraw::Input>;

// A post-processing draw at the output size, from before the add-on's draw hooks to after them.
struct PostDraw
{
    std::optional<ui_redirect::OutputSizeDraw> redirected;
    // The grading's render-size target, the scene target, which then gets the graded image as well.
    ComPtr<ID3D11RenderTargetView> graded;
    // The game's input at t0, put back after a draw that read the add-on's in its place.
    ComPtr<ID3D11ShaderResourceView> gameInput;

    void clear()
    {
        redirected.reset();
        graded.Reset();
        gameInput.Reset();
    }
};
thread_local PostDraw postDraw;

// The full-screen triangle, the bilinear stretch and the motion blur's start, made once for the
// immediate context and the recording threads. The composite draws with the triangle too: its pixel
// shader reads only SV_Position.
struct StretchShaders
{
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel, blurSource;
    ComPtr<ID3D11SamplerState> sampler;
};
std::mutex stretchShaderMutex;
std::shared_ptr<const StretchShaders> stretchShaders;

std::shared_ptr<const StretchShaders> ensureStretch()
{
    std::lock_guard lock(stretchShaderMutex);
    if (stretchShaders || !device) return stretchShaders;

    auto made = std::make_shared<StretchShaders>();
    const auto code = compile(shaders::StretchVertex, "stretch-vertex", "vs_5_0");
    check(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, made->vertex.GetAddressOf()),
        "Creating the stretch vertex shader");
    made->pixel = pixelShader(device.Get(), shaders::Stretch, "stretch");
    made->blurSource = pixelShader(device.Get(), shaders::BlurSource, "blur-source");
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    check(device->CreateSamplerState(&sampler, made->sampler.GetAddressOf()), "Creating the stretch sampler");
    stretchShaders = std::move(made);

    return stretchShaders;
}

// The bindings a stretch changes on a recording context, the game's put back when this goes out of
// scope.
class SavedStretchState
{
public:
    explicit SavedStretchState(ID3D11DeviceContext* context) : context(context)
    {
        context->IAGetInputLayout(layout.GetAddressOf());
        context->IAGetPrimitiveTopology(&topology);
        context->VSGetShader(vertex.GetAddressOf(), nullptr, nullptr);
        context->PSGetShader(pixel.GetAddressOf(), nullptr, nullptr);
        context->PSGetShaderResources(0, InputCount, inputs);
        context->PSGetSamplers(0, 1, sampler.GetAddressOf());
        context->RSGetState(rasterizer.GetAddressOf());
        context->OMGetBlendState(blend.GetAddressOf(), blendFactor, &sampleMask);
        context->OMGetRenderTargets(TargetCount, targets, depth.GetAddressOf());
    }

    ~SavedStretchState()
    {
        context->IASetInputLayout(layout.Get());
        context->IASetPrimitiveTopology(topology);
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->PSSetShader(pixel.Get(), nullptr, 0);
        context->PSSetShaderResources(0, InputCount, inputs);
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        context->RSSetState(rasterizer.Get());
        context->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
        context->OMSetRenderTargets(TargetCount, targets, depth.Get());

        for (auto* input : inputs)
            if (input) input->Release();

        for (auto* target : targets)
            if (target) target->Release();
    }

    SavedStretchState(const SavedStretchState&) = delete;
    SavedStretchState& operator=(const SavedStretchState&) = delete;

private:
    static constexpr UINT InputCount = 2, TargetCount = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;

    ID3D11DeviceContext* context;

    ComPtr<ID3D11InputLayout> layout;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ID3D11ShaderResourceView* inputs[InputCount]{};
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
    float blendFactor[4]{};
    UINT sampleMask{};
    ID3D11RenderTargetView* targets[TargetCount]{};
    ComPtr<ID3D11DepthStencilView> depth;
};

// Binds the stretch's triangle, `pixel` and the bilinear sampler, with `inputs` from t0 and `target`
// whole; then draws.
void drawStretch(ID3D11DeviceContext* context, const StretchShaders& stretch, ID3D11PixelShader* pixel,
    std::initializer_list<ID3D11ShaderResourceView*> inputs, ID3D11RenderTargetView* target)
{
    const auto size = textureDesc(target);
    const WholeTargetViewport viewport(context, size.Width, size.Height);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(stretch.vertex.Get(), nullptr, 0);
    context->PSSetShader(pixel, nullptr, 0);
    context->PSSetShaderResources(0, static_cast<UINT>(inputs.size()), inputs.begin());
    context->PSSetSamplers(0, 1, stretch.sampler.GetAddressOf());
    context->RSSetState(nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->Draw(3, 0);
}

// The graded image at the output size, stretched into the render-size scene target on `context`. The
// game copies the scene after grading for its 2D filters (chromatic aberration, radial blur, water
// drops), which then show the graded image under them, as before.
void drawGradedScene(ID3D11DeviceContext* context, ID3D11RenderTargetView* scene)
{
    const auto output = ui_redirect::output();
    const auto stretch = ensureStretch();
    if (!output.view || !stretch) return;

    const SavedStretchState saved(context);
    drawStretch(context, *stretch, stretch->pixel.Get(), {output.view.Get()}, scene);
}

std::uint64_t currentRecordingFrame()
{
    std::lock_guard lock(recordMutex);
    return frames.recordingFrame();
}

// A game target one of the motion blur's draws wrote on a recording thread, by address only, and the
// frame being recorded then. The game draws into the same targets every frame, so the address alone
// would match an earlier frame's draw.
struct DrawnGameTarget
{
    std::uint64_t frame{};
    const ID3D11Resource* target{};

    bool isReadBy(ID3D11ShaderResourceView* view) const
    {
        return target && frame == currentRecordingFrame() && resourceOf(view).Get() == target;
    }
};

// The game's motion blur on one recording thread: the game's half-size target its last gather pass
// wrote, and the surface at half the output size that holds that pass's result.
struct BlurChain
{
    DrawnGameTarget gameTarget;
    ui_redirect::Surface surface{};
};
thread_local BlurChain blurChain;

// Whether `source` is the result of the last gather pass on this thread, in the frame being recorded.
// Without the frame, a frame that missed its composite would leave the next frame's first pass taken
// for its second.
bool isBlurResult(ID3D11ShaderResourceView* source)
{
    return blurChain.gameTarget.isReadBy(source);
}

// The motion blur's gathers read its velocity and depth bake here.
constexpr UINT BlurBakeSlot = 5;

// The game's bake target on this recording thread, where the add-on last baked beside it at half the
// output size.
thread_local DrawnGameTarget gameBake;

// Whether the gather on `context` reads the game's bake that the add-on also made at half the output
// size.
bool hasOutputSizeBake(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11ShaderResourceView> bake;
    context->PSGetShaderResources(BlurBakeSlot, 1, bake.GetAddressOf());
    return gameBake.isReadBy(bake.Get());
}

// The game's last copy on this thread: its target, by address only, and what it copied. Without
// upscaling, the motion blur's first pass starts from the scene the game halved for it.
struct LastCopy
{
    const ID3D11Resource* target{};
    ComPtr<ID3D11ShaderResourceView> source;
};
thread_local LastCopy lastCopy;

void noteCopy(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11RenderTargetView> target;
    context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
    lastCopy.target = resourceOf(target.Get()).Get();
    context->PSGetShaderResources(0, 1, lastCopy.source.ReleaseAndGetAddressOf());
}

// The game's motion blur goes through the add-on's surfaces while upscaling (gatherBlur), and with the
// scene in float at every upscaler setting: the game's own are 8-bit, which clips highlights while the
// camera moves. Otherwise the game's passes stay as they are.
bool isBlurRedirected()
{
    return ui_redirect::isUpscaling() ? replacing.load(std::memory_order_relaxed) : hdr::isSceneFloat();
}

// What the blur's first pass starts from in place of `gameHalf`, the game's 8-bit half of the scene:
// DLSS's image at the output size while upscaling, or else the scene the game halved. Null when the
// copy that made `gameHalf` was not seen.
ComPtr<ID3D11ShaderResourceView> blurScene(ID3D11ShaderResourceView* gameHalf)
{
    if (ui_redirect::isUpscaling()) return ui_redirect::output().view;
    if (lastCopy.target && lastCopy.target == resourceOf(gameHalf).Get()) return lastCopy.source;

    return nullptr;
}

// While upscaling, the motion blur's velocity and depth are also baked at half the output size, for the
// gathers there (gatherBlur). They point-sample the bake for their depth and velocity tests, so at half
// the render size a moving silhouette stepped by a bake pixel: up to three of theirs. The bake reads its
// inputs by texture coordinate and none of its constants depends on its own size, so the game's stay.
// The game's bake still draws, for the gathers to fall back to.
void bakeBlur(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call)
{
    ComPtr<ID3D11RenderTargetView> target;
    context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
    if (!target) return;

    const ui_redirect::OutputSizeDraw redirected(context, ui_redirect::Surface::BlurBake, Inputs{});
    if (!redirected) return;

    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    gameBake = {currentRecordingFrame(), resourceOf(target.Get()).Get()};
}

// While upscaling, the game's motion blur gathers at half the output size from DLSS's image, as it
// does at half the render size without upscaling. At half the render size it gathered from the
// jittered scene before DLSS, and its composite stretched that over the output: pixelated. Its first
// pass reads the add-on's halved image instead of the game's halved scene, the second the first's
// result, and the composite the second's (beforePostDraw). Both read the add-on's bake when it was
// made. The game's constants stay: they place the samples in texture coordinates, so the blur reaches
// as far on screen.
bool gatherBlur(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11RenderTargetView> target;
    context->PSGetShaderResources(0, 1, source.GetAddressOf());
    context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
    if (!source || !target) return false;

    using enum ui_redirect::Surface;
    const bool isFirstPass = !isBlurResult(source.Get());
    if (isFirstPass)
    {
        const auto scene = blurScene(source.Get());
        const auto blur = ui_redirect::surfaceViews(Blur);
        const auto stretch = ensureStretch();
        if (!scene || !blur.target || !stretch) return false;

        const SavedStretchState saved(context);
        drawStretch(context, *stretch, stretch->blurSource.Get(), {scene.Get(), source.Get()}, blur.target.Get());
    }

    const auto input = isFirstPass ? Blur : blurChain.surface;
    const auto output = input == Blur ? BlurTwin : Blur;
    if (hasOutputSizeBake(context)) postDraw.redirected.emplace(context, output, Inputs{{0, input}, {BlurBakeSlot, BlurBake}});
    else postDraw.redirected.emplace(context, output, Inputs{{0, input}});
    if (!*postDraw.redirected)
    {
        postDraw.clear();
        return false;
    }

    blurChain = {{currentRecordingFrame(), resourceOf(target.Get()).Get()}, output};
    if (ui_redirect::isUpscaling()) recordUiTargetUse(context);

    return true;
}

// Without upscaling, the blur's composite stays on the game's scene and reads the add-on's result at
// t0 in place of the game's; afterPostDraw puts the game's back.
bool composeBlurInPlace(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11ShaderResourceView> source;
    context->PSGetShaderResources(0, 1, source.GetAddressOf());
    if (!isBlurResult(source.Get())) return false;

    const auto blur = ui_redirect::surfaceViews(blurChain.surface);
    blurChain = {};
    if (!blur.view) return false;

    context->PSSetShaderResources(0, 1, blur.view.GetAddressOf());
    postDraw.gameInput = std::move(source);

    return true;
}

// The motion blur's draws, which HDR redirects at every upscaler setting (isBlurRedirected). Returns
// true when the add-on makes the draw: the gathers, and the composite without upscaling.
bool redirectBlur(ID3D11DeviceContext* context, GamePass pass, const ui_redirect::DrawCall& call)
{
    if (!isBlurRedirected()) return false;
    if (pass == GamePass::MotionBlur) return gatherBlur(context);

    if (pass == GamePass::MotionBlurBake)
    {
        if (ui_redirect::isUpscaling()) bakeBlur(context, call);
        return false;
    }

    if (pass != GamePass::Copy || ui_redirect::isUpscaling()) return false;

    if (composeBlurInPlace(context)) return true;

    // Any other copy may be the one that halves the scene for the blur's first pass.
    noteCopy(context);

    return false;
}

// ---- Per frame, on the immediate context ----

// Runs the replacement's Direct3D work in its own context state, so the game's is untouched.
template<class Work> void inOwnState(Work&& work)
{
    Bypass guard;
    ComPtr<ID3DDeviceContextState> gameState;
    context1->SwapDeviceContextState(ownState.Get(), gameState.GetAddressOf());

    const auto restore = [&]
    {
        immediate->ClearState();
        context1->SwapDeviceContextState(gameState.Get(), nullptr);
    };

    try
    {
        work();
    }
    catch (...)
    {
        restore();
        throw;
    }

    restore();
}

// `sceneBeforeDof`, when given, also makes Ray Reconstruction's depth of field guide (dofGuideTarget).
void runInputs(const Frame& current, const camera::Matrix& previousFromCurrent, ID3D11ShaderResourceView* objectVelocity, bool linear,
    ID3D11ShaderResourceView* sceneBeforeDof, ID3D11ShaderResourceView* glassDepth)
{
    InputConstants constants{};
    for (int i = 0; i < 16; ++i) constants.previousFromCurrent[i / 4][i % 4] = static_cast<float>(previousFromCurrent[i]);
    const auto ndc = camera::jitterNdc(current.jitter, static_cast<float>(colorTarget.width), static_cast<float>(colorTarget.height));
    constants.jitterNdc[0] = ndc[0];
    constants.jitterNdc[1] = ndc[1];
    constants.renderSize[0] = colorTarget.width;
    constants.renderSize[1] = colorTarget.height;
    constants.objectMotion = objectVelocity != nullptr;
    constants.linear = linear;
    constants.dofGuide = sceneBeforeDof != nullptr;
    immediate->UpdateSubresource(inputConstants.Get(), 0, nullptr, &constants, 0, 0);

    ID3D11ShaderResourceView* inputs[]{sceneView.Get(), depthView.Get(), objectVelocity, sceneBeforeDof, glassDepth};
    ID3D11UnorderedAccessView* outputs[]{colorTarget.access.Get(), depthTarget.access.Get(), motionTarget.access.Get(),
        sceneBeforeDof ? dofGuideTarget.access.Get() : nullptr};
    immediate->CSSetShader(inputShader.Get(), nullptr, 0);
    immediate->CSSetConstantBuffers(0, 1, inputConstants.GetAddressOf());
    immediate->CSSetShaderResources(0, static_cast<UINT>(std::size(inputs)), inputs);
    immediate->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(outputs)), outputs, nullptr);
    immediate->Dispatch(groups(colorTarget.width), groups(colorTarget.height), 1);
    immediate->ClearState();
}

void runComposite(ID3D11RenderTargetView* destination, ID3D11ShaderResourceView* objectVelocity, const Diagnostics& options, float sharpness,
    bool linear)
{
    const auto view = options.debugView;
    const bool isShadowView = view == DebugView::ShadowRays || view == DebugView::ShadowStarts;
    const CompositeConstants constants{
        .showMotion = view == DebugView::MotionVectors,
        .showObjectMotion = view == DebugView::ObjectMotion,
        .showProbe = isShadowView,
        .sharpness = sharpness,
        .objectMotion = objectVelocity != nullptr,
        .linear = linear,
    };
    immediate->UpdateSubresource(compositeConstants.Get(), 0, nullptr, &constants, 0, 0);

    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(outputTarget.width), static_cast<float>(outputTarget.height), 0, 1};
    auto* shadowRays = isShadowView
                           ? ray_tracing::shadowDebug(colorTarget.width, colorTarget.height)
                       : view == DebugView::LightHistory || view == DebugView::LightValidation
                           ? ray_tracing::lightDebug(colorTarget.width, colorTarget.height)
                           : nullptr;
    ID3D11ShaderResourceView* inputs[]{outputTarget.view.Get(), motionTarget.view.Get(), objectVelocity, shadowRays};
    immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    immediate->VSSetShader(ensureStretch()->vertex.Get(), nullptr, 0);
    immediate->PSSetShader(compositeShader.Get(), nullptr, 0);
    immediate->PSSetConstantBuffers(0, 1, compositeConstants.GetAddressOf());
    immediate->PSSetShaderResources(0, static_cast<UINT>(std::size(inputs)), inputs);
    immediate->OMSetRenderTargets(1, &destination, nullptr);
    immediate->RSSetViewports(1, &viewport);
    immediate->Draw(3, 0);
}

// Without a DLSS image this frame, the UI target gets the engine's scene stretched, as GammaControl
// would have shown it, so the UI never draws over a stale frame.
void stretchToUiTarget(const ui_redirect::Output& output)
{
    const auto stretch = ensureStretch();
    inOwnState([&]
    {
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(output.width), static_cast<float>(output.height), 0, 1};
        immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        immediate->VSSetShader(stretch->vertex.Get(), nullptr, 0);
        immediate->PSSetShader(stretch->pixel.Get(), nullptr, 0);
        immediate->PSSetShaderResources(0, 1, output.scene.GetAddressOf());
        immediate->PSSetSamplers(0, 1, stretch->sampler.GetAddressOf());
        immediate->OMSetRenderTargets(1, output.target.GetAddressOf(), nullptr);
        immediate->RSSetViewports(1, &viewport);
        immediate->Draw(3, 0);
    });
}

// After the depth of field and before the grading: the scene target's image into DLSS or FSR, and
// their output back over it (DLAA) or into the UI target, which the grading then draws on at the
// output size.
bool upscale(const FxaaPass& pass, const Frame& current)
{
    const auto setup = currentConfiguration();
    auto wanted = setup.upscaling;
    const auto& options = setup.diagnostics;
    if (setup.sharpeningLeftOut) wanted.sharpness = 0;

    // Each frame is recorded before it executes; a later frame's uploads would have started a new one.
    if (pass.frame != current.index) return fallback("the frame's command lists were not recorded in step with their execution");
    if (!current.view) return fallback("the frame has no main camera");
    if (!pass.sceneTarget || !pass.depth) return fallback("the FXAA pass has an unexpected binding");

    const auto target = textureDesc(pass.sceneTarget.Get()), depth = textureDesc(pass.depth.Get());
    const UINT width = target.Width, height = target.Height;
    if (!width || depth.Width != width || depth.Height != height) return fallback("the FXAA pass's textures differ in size");
    if (current.view->width != static_cast<float>(width) || current.view->height != static_cast<float>(height))
        return fallback("the camera's viewport is not the FXAA pass's size");
    if (!ensureDepthView(pass.depth.Get())) return fallback("the depth copy cannot be read");
    if (!ensureSceneView(pass.sceneTarget.Get())) return fallback("the scene target cannot be read");

    // The upscaling modes run at the planned size, DLAA at the engine's own; after a mode change the
    // engine reaches either once it has rebuilt its targets.
    RenderSizePlan planned{.render = {width, height}, .output = {width, height}, .quality = DlssQuality::Dlaa};
    if (upscales(wanted.mode))
    {
        if (!plan) return fallback("no upscaling plan yet");
        if (width != plan->render.width || height != plan->render.height) return fallback("waiting for the requested render size");

        planned = *plan;
    }
    else if (width < outputSize[0] && height < outputSize[1])
        return fallback("waiting for the requested render size");

    // Upscaled frames go to the UI target, which the grading and the UI then draw on; DLAA writes over
    // the scene target.
    const bool upscaled = planned.render != planned.output;
    const auto uiTarget = upscaled ? ui_redirect::output() : ui_redirect::Output{};
    if (upscaled && (uiTarget.width != planned.output.width || uiTarget.height != planned.output.height)) return fallback("the UI target is not ready");

    ensurePipeline();
    initializeUpscaler(wanted.upscaler);
    // FSR and Ray Reconstruction read their inputs and write their output through Direct3D 12. FSR takes
    // linear colour, as DLSS does in its HDR mode on the float scene; Ray Reconstruction needs it. Linear
    // colour needs more than 8 bits.
    const bool fsr = wanted.upscaler == Upscaler::Fsr;
    auto guides = reconstructionGuides(wanted, width, height);
    if (guides && !hdr::isSceneFloat()) guides.reset();
    const bool shared = fsr || guides.has_value();
    const bool linear = fsr || hdr::isSceneFloat();
    ensureTarget(colorTarget, width, height, linear ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM,
        "Creating the upscaler's colour texture", shared);
    ensureTarget(depthTarget, width, height, DXGI_FORMAT_R32_FLOAT, "Creating the upscaler's depth texture", shared || frameGeneration);
    ensureTarget(motionTarget, width, height, DXGI_FORMAT_R16G16_FLOAT, "Creating the upscaler's motion texture", shared || frameGeneration);
    ensureTarget(outputTarget, planned.output.width, planned.output.height, DXGI_FORMAT_R16G16B16A16_FLOAT,
        "Creating the upscaler's output texture", shared);

    // A gap means the upscaler missed frames; its history and the previous camera are stale.
    bool reset = !lastEvaluated || lastEvaluated->index + 1 != current.index;
    if (!reset && camera::cameraCut(*lastEvaluated->view, *current.view))
    {
        reset = true;
        ++cameraCuts;
    }

    const auto& previous = reset ? *current.view : *lastEvaluated->view;
    const auto reprojection = camera::previousFromCurrent(previous, *current.view);
    auto* objectVelocity = velocity && velocity->frame == current.index ? velocity->view.Get() : nullptr;
    if (objectVelocity && textureDesc(objectVelocity).Width != width) objectVelocity = nullptr;

    const auto now = clock::ticksNow();
    const auto frameTime = lastEvaluation ? static_cast<float>(clock::millisecondsOf(now - lastEvaluation)) : 16.7f;

    // Frame generation's hudless colour is the graded image there, before the UI draws over it.
    const auto destination = upscaled ? uiTarget.target : pass.sceneTarget;
    const auto destinationDesc = textureDesc(destination.Get());
    std::shared_ptr<const Hudless> hudlessSource;
    if (frameGeneration)
    {
        std::lock_guard lock(hudlessMutex);
        hudlessSource = hudless;
    }

    if (hudlessSource
        && (destinationDesc.Width != hudlessSource->width || destinationDesc.Height != hudlessSource->height
            || destinationDesc.Format != hdr::sceneFormat()))
        hudlessSource.reset();

    // The game's depth of field drew this frame: Ray Reconstruction gets its guide, which preset D cannot take.
    ComPtr<ID3D11ShaderResourceView> sceneBeforeDof;
    if (guides && dofFrame == current.index && wanted.reconstructionPreset != upscaler::ReconstructionPreset::D)
        sceneBeforeDof = hdr::sceneBeforeDof(width, height);

    if (sceneBeforeDof) ensureTarget(dofGuideTarget, width, height, DXGI_FORMAT_R16_FLOAT, "Creating Ray Reconstruction's depth of field guide", true);

    // Ray Reconstruction and DLSS each keep their own history: the one the other left the frame before
    // starts over.
    bool reconstructed{};
    inOwnState([&]
    {
        performance::Timed timed(immediate.Get(), performance::Pass::UpscalerInputs);
        ComPtr<ID3D11ShaderResourceView> glassDepth;
        if (guides) glassDepth = glass::guideReconstruction(immediate.Get(), guides->normalAccess.Get(), ray_tracing::reflectionDistanceAccess(width, height));

        runInputs(current, reprojection, objectVelocity, linear, sceneBeforeDof.Get(), glassDepth.Get());
        timed.next(performance::Pass::Upscaler);
        if (guides)
        {
            const auto camera = camera::streamlineCamera(previous, *current.view);
            ray_reconstruction::Inputs inputs;
            inputs.color = colorTarget.texture.Get();
            inputs.depth = depthTarget.texture.Get();
            inputs.motion = motionTarget.texture.Get();
            inputs.output = outputTarget.texture.Get();
            inputs.normalRoughness = guides->normalRoughness.Get();
            inputs.diffuseAlbedo = guides->diffuseAlbedo.Get();
            inputs.specularAlbedo = guides->specularAlbedo.Get();
            inputs.dofGuide = sceneBeforeDof ? dofGuideTarget.texture.Get() : nullptr;
            inputs.specularHitDistance = ray_tracing::reflectionDistance(width, height);
            inputs.worldToView = camera.worldToView;
            inputs.viewToClip = camera.viewToClip;
            inputs.jitter[0] = current.jitter[0];
            inputs.jitter[1] = current.jitter[1];
            inputs.frameTime = frameTime;
            inputs.quality = static_cast<int>(planned.quality);
            inputs.preset = wanted.reconstructionPreset;
            inputs.hdr = true;
            inputs.reset = reset || !reconstructing;
            try
            {
                reset = ray_reconstruction::evaluate(inputs) || reset;
                ray_tracing::reconstructed();
                reconstructed = true;
            }
            catch (const std::exception& error)
            {
                // DLSS runs in its place, from this frame for the session.
                reconstructionFailure = error.what();
                log("Ray Reconstruction failed; DLSS runs instead: " + *reconstructionFailure);
                ray_reconstruction::release();
            }
        }

        if (fsr)
        {
            fsr_upscaler::Inputs inputs;
            inputs.color = colorTarget.texture.Get();
            inputs.depth = depthTarget.texture.Get();
            inputs.motion = motionTarget.texture.Get();
            inputs.output = outputTarget.texture.Get();
            inputs.jitter[0] = current.jitter[0];
            inputs.jitter[1] = current.jitter[1];
            inputs.frameTime = frameTime;
            inputs.cameraNear = current.view->nearPlane;
            inputs.cameraFar = current.view->farPlane;
            inputs.verticalFov = static_cast<float>(2 * std::atan(1 / current.view->projection[5]));
            inputs.hdr = hdr::isSceneFloat();
            inputs.reset = reset;
            reset = fsr_upscaler::evaluate(inputs) || reset;
        }
        else if (!reconstructed)
        {
            ensureExposure();
            reset = ensureFeature(planned, wanted) || reset;

            NVSDK_NGX_D3D11_DLSS_Eval_Params evaluation{};
            evaluation.Feature.pInColor = colorTarget.texture.Get();
            evaluation.Feature.pInOutput = outputTarget.texture.Get();
            evaluation.pInDepth = depthTarget.texture.Get();
            evaluation.pInMotionVectors = motionTarget.texture.Get();
            evaluation.pInExposureTexture = exposureTexture.Get();
            evaluation.InJitterOffsetX = current.jitter[0];
            evaluation.InJitterOffsetY = current.jitter[1];
            evaluation.InRenderSubrectDimensions = {width, height};
            evaluation.InReset = reset || reconstructing ? 1 : 0;
            evaluation.InMVScaleX = 1;
            evaluation.InMVScaleY = 1;
            evaluation.InFrameTimeDeltaInMsec = frameTime;
            const auto result = evaluateDlss(evaluation);
            immediate->ClearState();
            if (NVSDK_NGX_FAILED(result)) throw std::runtime_error("DLSS evaluation failed (" + hex(static_cast<std::uint32_t>(result)) + ").");
        }

        timed.next(performance::Pass::UpscalerOutput);
        runComposite(destination.Get(), objectVelocity, options, wanted.sharpness, linear);
    });
    reconstructing = reconstructed;
    focus::depthReady(immediate.Get(), depthTarget.texture.Get(), *current.view);
    pendingHudless.reset();
    if (hudlessSource) pendingHudless = PendingHudless{hudlessSource, resourceOf(destination.Get())};

    if (frameGeneration)
    {
        auto& inputs = presented.inputs;
        inputs = {};
        inputs.frame = current.gameFrame;
        inputs.depth = depthTarget.texture.Get();
        inputs.motion = motionTarget.texture.Get();
        inputs.renderWidth = width;
        inputs.renderHeight = height;
        inputs.jitter[0] = current.jitter[0];
        inputs.jitter[1] = current.jitter[1];
        inputs.reset = reset;
        inputs.camera = camera::streamlineCamera(previous, *current.view);
        presented.hudless.reset();
        presented.hudlessDrawn = false;
    }

    if (upscaled) uiTargetFilled = true;
    if (reset) ++resets;
    if (++evaluations == 1)
        log("First " + std::string(label(wanted.upscaler)) + " frame: " + std::to_string(width) + "x" + std::to_string(height) + " -> " + std::to_string(planned.output.width) + "x"
            + std::to_string(planned.output.height) + (objectVelocity ? ", with" : ", without") + " the engine's object velocity.");

    lastEvaluated = current;
    lastEvaluation = now;
    lastEvaluatedPresent = presents;

    return true;
}

// Before the frame's first command list of the grading runs; its later ones leave it be.
void upscaleBeforeGrading()
{
    Frame current;
    std::optional<FxaaPass> pass;
    {
        std::lock_guard lock(recordMutex);
        current = frames.current();
        pass = recordedFxaa;
    }

    if (upscaledFrame == current.index) return;

    upscaledFrame = current.index;

    bool evaluated{};
    if (replacing)
    {
        try
        {
            evaluated = pass ? upscale(*pass, current) : fallback("no FXAA pass was recorded");
        }
        catch (const std::exception& error)
        {
            fail(error.what());
        }
    }

    const bool shouldJitterNext = evaluated && !currentConfiguration().diagnostics.noJitter;
    std::lock_guard lock(recordMutex);
    frames.upscaled(shouldJitterNext);
}

// The graded image into frame generation's hudless texture, before the UI draws over it.
void copyHudless()
{
    const auto copy = std::move(*pendingHudless);
    pendingHudless.reset();
    Bypass guard;
    immediate->CopyResource(copy.hudless->source.Get(), copy.image.Get());
    presented.hudless = copy.hudless;
}

// Before a command list that draws on the UI target runs. DLSS has written it this frame, or the
// engine's scene is stretched into it.
void fillUiTarget()
{
    if (uiTargetFilled) return;

    const auto output = ui_redirect::output();
    if (!output.target || !output.scene)
    {
        logOnce("The UI target is not ready; a frame shows only the UI.");
        return;
    }

    ensurePipeline();
    stretchToUiTarget(output);
    uiTargetFilled = true;
}

// Chooses the render size for the selected mode and asks the engine for it; Off and DLAA leave the
// engine its own. On the immediate context's thread, once per frame.
void updatePlan()
{
    const auto setup = currentConfiguration();
    const auto& wanted = setup.upscaling;
    // Ray Reconstruction asks for many more jitter positions than DLSS: at least 32.
    const int leastPhases = wanted.upscaler == Upscaler::Dlss && wanted.rayReconstruction && !reconstructionFailure ? 32 : 0;
    if (!setup.supportedBuild || failed || !upscales(wanted.mode) || !render_scale::hooked() || !outputSize[0] || !outputSize[1])
    {
        plan.reset();
        jitterPhases = std::max(camera::jitterPhases(1.0f), leastPhases);
        targetMipBias = replacing ? mipBias(1.0f, wanted.textureBiasOffset) : 0.0f;
        render_scale::request({}, {});
        return;
    }

    const render_scale::Size output{outputSize[0], outputSize[1]};
    if (!plan || !plan->isFor(wanted, output))
    {
        // NGX knows DLSS's optimal render sizes; Custom keeps its own scale.
        std::optional<render_scale::Size> dlssOptimal;
        if (wanted.upscaler == Upscaler::Dlss)
        {
            initializeNgx();
            if (wanted.mode != Mode::Custom) dlssOptimal = dlssOptimalSize(wanted, output);
        }

        plan = planRenderSize(wanted, output, dlssOptimal);
        log(std::string(label(wanted.upscaler)) + " " + std::string(label(wanted.mode, wanted.upscaler)) + " renders at " + std::to_string(plan->render.width) + "x" + std::to_string(plan->render.height)
            + " for " + std::to_string(plan->output.width) + "x" + std::to_string(plan->output.height) + ".");
    }

    const float scale = static_cast<float>(plan->render.width) / static_cast<float>(plan->output.width);
    jitterPhases = std::max(camera::jitterPhases(scale), leastPhases);
    targetMipBias = mipBias(scale, wanted.textureBiasOffset);
    render_scale::request(plan->render, plan->output);
}

// Fox Engine's material samplers repeat their textures; the screen-space passes sample with point or
// clamped samplers, and a bias would also shift their explicit mip reads.
bool materialSampler(const D3D11_SAMPLER_DESC& sampler)
{
    const auto tiles = [](D3D11_TEXTURE_ADDRESS_MODE mode) { return mode == D3D11_TEXTURE_ADDRESS_WRAP || mode == D3D11_TEXTURE_ADDRESS_MIRROR; };
    return sampler.MaxLOD > sampler.MinLOD && tiles(sampler.AddressU) && tiles(sampler.AddressV) && sampler.Filter != D3D11_FILTER_MIN_MAG_MIP_POINT
        && !D3D11_DECODE_IS_COMPARISON_FILTER(sampler.Filter);
}

// The biased copy of a material sampler, made once per sampler and bias; null for other samplers.
// Copies are kept, with the game's sampler, until detach(), so a bound one is never released.
ID3D11SamplerState* biasedCopy(ID3D11SamplerState* sampler, float bias)
{
    const std::pair key{sampler, bias};
    {
        std::shared_lock lock(samplerMutex);
        const auto found = biasedSamplers.find(key);
        if (found != biasedSamplers.end()) return found->second.biased.Get();
    }

    BiasedSampler entry{sampler, nullptr};
    D3D11_SAMPLER_DESC desc{};
    sampler->GetDesc(&desc);
    if (materialSampler(desc) && device)
    {
        desc.MipLODBias = std::clamp(desc.MipLODBias + bias, D3D11_MIP_LOD_BIAS_MIN, D3D11_MIP_LOD_BIAS_MAX);
        device->CreateSamplerState(&desc, entry.biased.GetAddressOf());
        log("Texture LOD bias " + std::to_string(bias) + " on a material sampler (filter " + hex(desc.Filter) + ", anisotropy "
            + std::to_string(desc.MaxAnisotropy) + ").");
    }

    std::unique_lock lock(samplerMutex);

    return biasedSamplers.try_emplace(key, std::move(entry)).first->second.biased.Get();
}

}

void initialize(const std::filesystem::path& directory) noexcept
{
    dataDirectory = directory;
}

void configure(const Configuration& configuration) noexcept
{
    const auto& upscaling = configuration.settings.upscaling;
    std::lock_guard lock(configuredMutex);
    // Choosing a mode or upscaler again retries after a failure.
    if (upscaling.mode != configured.upscaling.mode || upscaling.upscaler != configured.upscaling.upscaler) failed = false;

    configured = {upscaling, configuration.diagnostics, configuration.supportedBuild, configuration.leftOut.upscaler, configuration.leftOut.sharpening};
    updateReplacing();
}

bool replaces() noexcept
{
    return replacing;
}

void attach(ID3D11Device* gameDevice, ID3D11DeviceContext* context, ID3D11DeviceContext* wrapped) noexcept
{
    std::lock_guard lock(executionMutex);
    device = gameDevice;
    immediate = context;
    ngxContext = wrapped ? wrapped : context;
    ngxContext->GetDevice(ngxDevice.ReleaseAndGetAddressOf());
    if (FAILED(immediate.As(&context1))) fail("Direct3D 11.1 is not available.");
    ui_redirect::attach(gameDevice);
}

void detach() noexcept
{
    std::lock_guard lock(executionMutex);
    replacing = false;
    targetMipBias = 0;
    motion_blur::setVelocityOnly(false);
    fxaa::setForced(false);
    render_scale::request({}, {});
    ui_redirect::detach();
    pendingHudless.reset();
    velocity.reset();
    dofFrame.reset();
    lastEvaluated.reset();
    presented = {};
    plan.reset();
    releaseNgx();
    ray_reconstruction::stop();
    fsr_upscaler::stop();

    {
        std::lock_guard hudlessLock(hudlessMutex);
        hudless.reset();
        hudlessFailed = false;
    }

    colorTarget = depthTarget = motionTarget = outputTarget = dofGuideTarget = {};
    exposureTexture.Reset();
    depthResource.Reset();
    depthView.Reset();
    sceneResource.Reset();
    sceneView.Reset();
    {
        std::lock_guard shaderLock(stretchShaderMutex);
        stretchShaders.reset();
    }

    inputShader.Reset();
    compositeShader.Reset();
    inputConstants.Reset();
    compositeConstants.Reset();
    ownState.Reset();
    context1.Reset();
    ngxContext.Reset();
    ngxDevice.Reset();
    immediate.Reset();
    device.Reset();
    recordedLists.clear();
    {
        std::lock_guard recordLock(recordMutex);
        recordedFxaa.reset();
    }

    std::unique_lock samplerLock(samplerMutex);
    biasedSamplers.clear();
}

void setOutputSize(unsigned width, unsigned height) noexcept
{
    {
        std::lock_guard lock(executionMutex);
        outputSize[0] = width;
        outputSize[1] = height;
    }

    ui_redirect::setOutputSize(width, height);
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    if (bypass) return false;

    try
    {
        // The UI target is used whenever the engine's targets are smaller than the output, whatever the
        // mode, so the UI and GammaControl always agree on where the frame is.
        if (ui_redirect::drawing2D(context) && ui_redirect::draw2D(context, call))
        {
            recordUiTargetUse(context);

            return true;
        }

        if (call.indexed || call.count != 3 || call.instances != 1) return false;

        const auto* row = shader.row();
        if (!row) return false;

        if (row->pass == GamePass::GammaControl)
        {
            // With the scene in float every GammaControl draw, the hudless one too, encodes for the display.
            const hdr::OutputShader output(context);
            // Out of the fullscreen box, over the whole target: the add-on's draws read output-size
            // images, and the game's own then stretches its scene as it does windowed.
            std::optional<WholeTargetViewport> wholeTarget;
            if (const auto size = boxedGammaTarget(context)) wholeTarget.emplace(context, size->first, size->second);
            drawHudless(context, call);

            if (ui_redirect::drawGamma(context, call))
            {
                recordUiTargetUse(context);

                return true;
            }

            if (output.bound() || wholeTarget)
            {
                context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);

                return true;
            }
        }

        if (!replacing.load(std::memory_order_relaxed)) return false;

        // DLSS ran before the grading; FXAA would only blur its image.
        if (row->pass == GamePass::Fxaa)
        {
            recordFxaa(context);

            return true;
        }
    }
    catch (...)
    {
    }

    return false;
}

bool beforePostDraw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    if (bypass || call.indexed || call.count != 3 || call.instances != 1) return false;

    try
    {
        const auto* row = shader.row();
        if (!row) return false;

        const auto pass = row->pass;
        if (redirectBlur(context, pass, call)) return true;
        if (!replacing.load(std::memory_order_relaxed)) return false;

        if (pass == GamePass::CameraVelocity)
        {
            recordVelocity(context);
            return false;
        }

        if (pass == GamePass::DofComposite)
        {
            recordDofComposite(context);
            return false;
        }

        // CopyRenderBuffer is a plain copy the game makes anywhere; the layer's composite shares its
        // list with the LUT build.
        if (pass == GamePass::LutBuild || pass == GamePass::Grade || pass == GamePass::Fxaa) recordGrading(context);
        if (pass != GamePass::Copy && pass != GamePass::Grade) return false;

        ComPtr<ID3D11RenderTargetView> target;
        context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
        if (!ui_redirect::renderSized(target.Get())) return false;

        using enum ui_redirect::Surface;
        auto& redirected = postDraw.redirected;
        if (pass == GamePass::Copy)
        {
            // The copies into a render-size target in every recorded frame: the depth of field's copy
            // of the scene, before DLSS, then over it the half-size layer (lens flares) and, with motion
            // blur on, the blur's half-size result, which the add-on's surface holds instead.
            ComPtr<ID3D11ShaderResourceView> source;
            context->PSGetShaderResources(0, 1, source.GetAddressOf());
            if (ui_redirect::renderSized(source.Get())) return false;

            if (isBlurResult(source.Get())) redirected.emplace(context, Scene, Inputs{{0, blurChain.surface}});
            else redirected.emplace(context, Scene, Inputs{});
            blurChain = {};
        }
        else
        {
            ui_redirect::copySurface(context, Copy, Scene);
            redirected.emplace(context, Scene, Inputs{{0, Copy}});
            postDraw.graded = target;
        }

        if (!*redirected)
        {
            postDraw.clear();
            return false;
        }

        recordUiTargetUse(context);

        return true;
    }
    catch (...)
    {
        postDraw.clear();
        return false;
    }
}

void afterPostDraw(ID3D11DeviceContext* context) noexcept
{
    if (postDraw.gameInput) context->PSSetShaderResources(0, 1, postDraw.gameInput.GetAddressOf());

    const auto graded = std::move(postDraw.graded);
    postDraw.clear();
    if (!graded) return;

    try
    {
        drawGradedScene(context, graded.Get());
    }
    catch (...)
    {
    }
}

bool clearTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* view, const float color[4]) noexcept
{
    if (bypass || !ui_redirect::clearTarget(context, view, color)) return false;

    try
    {
        recordUiTargetUse(context);
    }
    catch (...)
    {
    }

    return true;
}

void depthStencilCleared(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, UINT flags, float depth, UINT8 stencil) noexcept
{
    if (!bypass) ui_redirect::depthStencilCleared(context, view, flags, depth, stencil);
}

bool biasSamplers(ID3D11DeviceContext* context, unsigned count, ID3D11SamplerState* const* samplers, ID3D11SamplerState** biased) noexcept
{
    // The 2D pass draws at the output size.
    const auto bias = targetMipBias.load(std::memory_order_relaxed);
    if (bypass || bias == 0 || !samplers || count > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT || ui_redirect::drawing2D(context)) return false;

    try
    {
        bool replaced{};
        for (unsigned i = 0; i < count; ++i)
        {
            biased[i] = samplers[i];
            auto* copy = samplers[i] ? biasedCopy(samplers[i], bias) : nullptr;
            if (!copy) continue;

            biased[i] = copy;
            replaced = true;
        }

        return replaced;
    }
    catch (...)
    {
        return false;
    }
}

void bufferMapped(ID3D11Resource* buffer, void* data) noexcept
{
    if (bypass || !data) return;

    D3D11_RESOURCE_DIMENSION dimension{};
    buffer->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) return;

    D3D11_BUFFER_DESC desc{};
    static_cast<ID3D11Buffer*>(buffer)->GetDesc(&desc);
    if (desc.ByteWidth != sizeof(game_constants::SceneConstants) || !(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return;

    mappedScene = buffer;
    mappedSceneData = data;
}

void bufferUnmapped(ID3D11Resource* buffer) noexcept
{
    if (!mappedScene || buffer != mappedScene) return;

    auto& scene = *static_cast<game_constants::SceneConstants*>(mappedSceneData);
    mappedScene = nullptr;
    mappedSceneData = nullptr;

    try
    {
        const auto kind = camera::classify(scene);
        if (kind == camera::SceneView::Other) return;

        std::lock_guard lock(recordMutex);
        frames.sceneWritten(scene, kind, {.gameFrame = game_loop::currentFrame(), .canJitter = replacing.load(), .jitterPhases = jitterPhases.load()});
    }
    catch (...)
    {
    }
}

void recorded(std::uint64_t into, std::uint64_t from) noexcept
{
    recordedLists.recorded(into, from);
}

void forget(std::uint64_t contextOrList) noexcept
{
    recordedLists.forget(contextOrList);
}

void executing(ID3D11DeviceContext* context, std::uint64_t list) noexcept
{
    if (bypass) return;

    try
    {
        std::lock_guard lock(executionMutex);
        if (context != immediate.Get()) return;

        if (pendingHudless && pendingHudless->copyNext) copyHudless();

        {
            std::lock_guard recordLock(recordMutex);
            frames.executed();
        }

        auto entry = recordedLists.take(list);
        if (!entry) return;

        if (entry->velocity) velocity = std::move(entry->velocity);
        if (entry->dofFrame) dofFrame = entry->dofFrame;
        // The hudless colour was copied before this list, so its GammaControl draws this frame's.
        if (entry->hudless && entry->hudless == presented.hudless) presented.hudlessDrawn = true;
        if (entry->grading) upscaleBeforeGrading();
        // The grading is done once the FXAA pass's list has run; the UI draws in a later one.
        if (entry->fxaa && pendingHudless) pendingHudless->copyNext = true;

        if (entry->drawsOnUiTarget) fillUiTarget();
    }
    catch (const std::exception& error)
    {
        fail(error.what());
    }
}

void present() noexcept
{
    std::lock_guard lock(executionMutex);
    pendingHudless.reset();
    {
        std::lock_guard recordLock(recordMutex);
        frames.presented(lastEvaluatedPresent == presents);
    }

    ++presents;
    uiTargetFilled = false;
    presented = {};
    expiredNotes += recordedLists.present();

    try
    {
        releaseUnused(currentConfiguration().upscaling);
        updatePlan();
        ensureHudless();
    }
    catch (const std::exception& error)
    {
        fail(error.what());
    }

    if constexpr (DiagnosticLog)
    {
        if (replacing && presents % 1800 == 0)
            log("Status: " + std::to_string(evaluations) + " upscaled frames, " + std::to_string(fallbacks) + " without, "
                + std::to_string(resets) + " history resets (" + std::to_string(cameraCuts) + " at camera cuts); last fallback: "
                + (lastFallback.empty() ? "none" : lastFallback) + "; " + std::to_string(expiredNotes) + " contexts' or lists' notes expired.");
    }
}

void setFrameGeneration(bool on) noexcept
{
    frameGeneration = on;
}

frame_generation::Inputs frameGenerationInputs() noexcept
{
    std::lock_guard lock(executionMutex);
    auto inputs = presented.inputs;
    inputs.valid = frameGeneration && replacing && lastEvaluatedPresent == presents && presented.hudless && presented.hudlessDrawn;
    if (inputs.valid) inputs.hudless = presented.hudless->target.Get();

    return inputs;
}

Status status()
{
    Status result;
    result.fxaaRecognised = game_shaders::firstCreated(FxaaShader) != nullptr;

    std::lock_guard lock(executionMutex);
    const auto setup = currentConfiguration();
    result.supportedBuild = setup.supportedBuild;
    result.reconstructionAvailable = !reconstructionFailure;
    result.rayReconstruction = setup.upscaling.upscaler == Upscaler::Dlss && reconstructing;
    result.upscalerName = setup.upscaling.upscaler == Upscaler::Fsr ? fsr_upscaler::name()
                          : result.rayReconstruction                 ? "DLSS Ray Reconstruction"
                                                                     : "DLSS";
    result.failed = failed;
    result.failure = failure;
    result.lastFallback = lastFallback;
    result.running = evaluations && lastEvaluatedPresent + 2 >= presents;
    result.upscalingAvailable = render_scale::hooked();
    result.renderWidth = colorTarget.width;
    result.renderHeight = colorTarget.height;
    result.outputWidth = outputTarget.width;
    result.outputHeight = outputTarget.height;
    result.mipBias = targetMipBias;
    result.evaluations = evaluations;
    result.fallbacks = fallbacks;
    result.resets = resets;

    if (lastEvaluated)
    {
        result.jitter[0] = lastEvaluated->jitter[0];
        result.jitter[1] = lastEvaluated->jitter[1];
    }

    return result;
}
}
