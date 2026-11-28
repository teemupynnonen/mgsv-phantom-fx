#include "features/hdr/hdr.hpp"

#include "d3d11_helpers.hpp"
#include "features/hdr/hdr_shaders.hpp"
#include "game_shaders.hpp"
#include "log.hpp"
#include "performance.hpp"
#include "shader_bytecode.hpp"

#include <Windows.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace phantom_fx::hdr
{
namespace
{
using Microsoft::WRL::ComPtr;
using game_shaders::GamePass;

constexpr DXGI_FORMAT GameSceneFormat = DXGI_FORMAT_B8G8R8A8_TYPELESS, SceneFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

// The renderer makes the scene target and its copy as its first two size-dependent targets
// (renderer +0x1E8 and +0x1F4): at start-up in 0x2BA1B0, and in every rebuild in 0x24B2A0. These are
// the return addresses of those calls (docs/DEVELOPMENT.md, "HDR").
constexpr std::uintptr_t SceneTargetCalls[]{0x2BA676, 0x2BA6DF, 0x24B2C1, 0x24B2D9};

// The resolves' cPSMaterial, the curve in the add-on's shaders, the output shader's constants, and
// the depth of field composite's copy of the scene.
constexpr UINT MaterialSlot = 4, CurveSlot = 8, OutputSlot = 2, DofSceneSlot = 1;

// TppTonemap as the game's shaders compile it, unoptimised: the opcodes of its 14 instructions (ge,
// movc, mul, mov, add, add, mov, add, mul, div, add, add, mul, add). The movc picks the linear part
// below the knee, the div is -1 / (a (x - b + c)), and the third mul weighs the curve's part.
constexpr std::uint32_t TonemapBlock[]{0x1D, 0x37, 0x38, 0x36, 0x00, 0x00, 0x36, 0x00, 0x38, 0x0E, 0x00, 0x00, 0x38, 0x00};
constexpr std::size_t MaskStep = 1, DivideStep = 9, CurveStep = 12;
constexpr std::uint32_t AndOpcode = 0x01, MinOpcode = 0x33, OpcodeMask = 0x7FF;
// The sky draws clamp to 1 within these many instructions after the block.
constexpr std::size_t ClampReach = 8;
constexpr float HalfMax = 65504;

// How the output shader writes the R10G10B10A2 back buffer (shaders/output.ps.hlsl).
enum class Encoding : std::uint32_t { Srgb, Pq };

struct OutputParameters
{
    float paperWhite, peak;
    Encoding encoding;
    float padding;
};

// A target the add-on made float, and what it keeps for it. Nothing here references the target, so
// the game's release destroys it as before; views of it are made where they are used.
struct Upgraded
{
    UINT width{}, height{};
    bool readAsSrgb{}; // The game made an sRGB shader resource view of it
    // Linear colour for the sRGB views to read, made at the first copy into the target.
    ComPtr<ID3D11Texture2D> twin;
    ComPtr<ID3D11ShaderResourceView> twinView;
    ComPtr<ID3D11UnorderedAccessView> twinAccess;
};

// A game shader the add-on made from patched bytecode, and the game's own, which the add-on's other
// parts identify it by. A sky shader, whose clamp to 1 stays with HDR output and is lifted in SDR, also
// has the other variant made, which binds in place of the game's while HDR output is the other way.
struct Patched
{
    std::vector<std::uint8_t> code, original;
    bool lifted{}; // The clamp in `code` is lifted
    ComPtr<ID3D11PixelShader> other;
};

// A sky shader as the game made it and its other variant, by the clamp.
struct SkyVariants
{
    ComPtr<ID3D11PixelShader> clampKept, clampLifted;
};

// The scene copy the depth of field composites over, kept in float: the game's is 8-bit.
struct DofCopy
{
    UINT width{}, height{};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
};

std::atomic<bool> floatScene{}, outputWanted{}, sceneFailed{}, unsupportedBuild{};
std::atomic<bool> twinsInUse{}, tenBitChain{};
// The HDR output the swap chain's colour space was last set for; none after a new swap chain.
std::mutex outputMutex;
std::optional<bool> appliedOutput;
std::atomic<Output> shown{Output::Off};
std::atomic<float> peakNits{}, paperWhiteNits{203}, displayPeak{};
// When neither the settings nor the display give a peak.
constexpr float FallbackPeakNits = 1000;

// What a thread's creation event decided, for the init event that follows on the same thread.
thread_local bool creatingTarget{}, creatingSrgbView{};
// The game's 8-bit copy of the scene the depth of field's composite reads, set by the copy before it
// on the same context.
thread_local ID3D11Resource* gameDofCopy{};

std::shared_mutex mutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11ComputeShader> captureShader, encodeShader, decodeShader;
// The grading without and with the light the tonemap took added back (HDR output), so HDR output can
// turn on and off.
ComPtr<ID3D11PixelShader> gradeShaders[2], reduceShader, outputShader;
DofCopy dofCopy;
ComPtr<ID3D11Buffer> outputBuffer, curveBuffer;
ComPtr<ID3D11ShaderResourceView> curveView;
ComPtr<ID3D11UnorderedAccessView> curveAccess;
std::unordered_map<ID3D11Resource*, Upgraded> targets;
// The game's sRGB views of the upgraded targets, with their target.
std::unordered_map<ID3D11View*, ID3D11Resource*> srgbViews;
// The game's pixel shaders made from patched bytecode, by the game's checksum and by the patched
// code; kept for the session.
std::map<DxbcHash, Patched> patchedShaders;
std::unordered_map<const void*, const Patched*> patchedByCode;
// The sky shaders by the object the game made and binds.
std::unordered_map<ID3D11PixelShader*, SkyVariants> skyShaders;
std::atomic<bool> hasSkyVariants{};

// Makes the shaders from their precompiled bytecode (compiled from source in a dev build) and the fixed buffers; call with the mutex held.
void createPipeline()
{
    // The encode reads and writes the float target in place.
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support{SceneFormat};
    if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support, sizeof(support)))
        || !(support.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD))
        throw std::runtime_error("the GPU cannot read R16G16B16A16_FLOAT through unordered access views.");

    captureShader = computeShader(device.Get(), shaders::Capture, "hdr-capture");
    encodeShader = computeShader(device.Get(), shaders::Encode, "hdr-encode");
    decodeShader = computeShader(device.Get(), shaders::Decode, "hdr-decode");
    gradeShaders[0] = pixelShader(device.Get(), shaders::Grade, "hdr-grade");
    gradeShaders[1] = pixelShader(device.Get(), shaders::GradeUpgrade, "hdr-grade-upgrade");
    reduceShader = pixelShader(device.Get(), shaders::Reduce, "hdr-reduce");
    outputShader = pixelShader(device.Get(), shaders::Output, "hdr-output");
    outputBuffer = makeConstantBuffer(device.Get(), sizeof(OutputParameters), "Creating the HDR output constants");

    // Zeros until the first resolve, which the shaders take as no tonemap.
    D3D11_BUFFER_DESC curve{};
    curve.ByteWidth = 16;
    curve.Usage = D3D11_USAGE_DEFAULT;
    curve.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    curve.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    curve.StructureByteStride = 16;
    const float zeros[4]{};
    const D3D11_SUBRESOURCE_DATA data{zeros};
    check(device->CreateBuffer(&curve, &data, curveBuffer.GetAddressOf()), "Creating the tonemap curve");
    check(device->CreateShaderResourceView(curveBuffer.Get(), nullptr, curveView.GetAddressOf()), "Creating the tonemap curve");
    check(device->CreateUnorderedAccessView(curveBuffer.Get(), nullptr, curveAccess.GetAddressOf()), "Creating the tonemap curve");
}

// Whether the texture being made is the scene target or its copy, by the calls that lead to it.
bool madeForScene()
{
    void* frames[32]{};
    const auto count = RtlCaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)), frames, nullptr);
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    for (USHORT i = 0; i < count; ++i)
        for (const auto call : SceneTargetCalls)
            if (reinterpret_cast<std::uintptr_t>(frames[i]) == base + call) return true;

    return false;
}

float linearToSrgb(float value)
{
    value = std::max(value, 0.0f);
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1 / 2.4f) - 0.055f;
}

// The compute slots the add-on's passes touch (b1, t0 and u0), saved around them.
constexpr ComputeSlots PassSlots{.buffers = 2, .views = 1, .accesses = 1};

// At a deferred resolve: its tonemap curve, which the grading and DLSS read later in the frame.
void captureCurve(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11Buffer> material;
    context->PSGetConstantBuffers(MaterialSlot, 1, material.GetAddressOf());
    if (!material || !captureShader) return;

    SavedComputeState saved(context, PassSlots);
    context->CSSetConstantBuffers(1, 1, material.GetAddressOf());
    context->CSSetUnorderedAccessViews(0, 1, curveAccess.GetAddressOf(), nullptr);
    context->CSSetShader(captureShader.Get(), nullptr, 0);
    context->Dispatch(1, 1, 1);
}

// The twin the sRGB views of `target` read; call with the mutex held exclusively.
void ensureTwin(Upgraded& target)
{
    if (target.twin) return;

    auto made = makeComputeTexture(device.Get(), target.width, target.height, SceneFormat, "Creating a decoded twin");
    target.twin = std::move(made.texture);
    target.twinView = std::move(made.view);
    target.twinAccess = std::move(made.access);
    twinsInUse = true;
    log("Float scene: the reflections read a decoded copy of the scene at " + std::to_string(target.width) + "x" + std::to_string(target.height) + ".");
}

float floatAt(const std::uint8_t* bytes, std::size_t offset)
{
    float value{};
    std::memcpy(&value, bytes + offset, sizeof(value));

    return value;
}

// The first min within ClampReach instructions from `first`: its literals of 1 become HalfMax.
void liftClamp(std::uint8_t* bytes, const std::vector<Instruction>& steps, std::size_t first)
{
    for (std::size_t step = first; step < std::min(steps.size(), first + ClampReach); ++step)
    {
        if (steps[step].opcode != MinOpcode) continue;

        for (const auto literal : immediatesOf(bytes, steps[step]))
            for (std::size_t component = 0; component < 4; ++component)
                if (floatAt(bytes, literal + 4 * component) == 1) std::memcpy(bytes + literal + 4 * component, &HalfMax, 4);

        return;
    }
}

// The scene in light rather than through TppTonemap: in each TppTonemap block the mask that keeps
// the linear part below the knee takes its linear value whatever the comparison, and the curve's part
// is ANDed with the zero left beside it, as a multiply by zero would keep the curve's pole as NaN.
// The sky draws are the only shaders with a clamp to 1 after the block. With HDR output it stays:
// above it, the sun's sprite shows its own profile, a ring around a dimmer centre, instead of the
// disc the clamp makes. In SDR it is lifted (shouldLiftClamp), as the grading's tonemap then clamps
// where the game's did: kept, the sky's light from 1 to 4 would all show as 0.81, never as white.
// Nullopt when the shader has no TppTonemap.
std::optional<std::vector<std::uint8_t>> withoutTonemap(const void* code, std::size_t size, bool shouldLiftClamp)
{
    std::vector<std::uint8_t> patched(static_cast<const std::uint8_t*>(code), static_cast<const std::uint8_t*>(code) + size);
    auto* bytes = patched.data();
    const auto program = instructionsOf(bytes, size);
    if (!program || program->size() < std::size(TonemapBlock)) return std::nullopt;

    const auto& steps = *program;
    bool found{};
    for (std::size_t first = 0; first + std::size(TonemapBlock) <= steps.size(); ++first)
    {
        bool block = true;
        for (std::size_t step = 0; step < std::size(TonemapBlock) && block; ++step) block = steps[first + step].opcode == TonemapBlock[step];
        if (!block) continue;

        // The mask's literals are (1 where kept, 0) and (0, 0, 0, 0); the curve's numerator is -1.
        const auto mask = immediatesOf(bytes, steps[first + MaskStep]), divide = immediatesOf(bytes, steps[first + DivideStep]);
        if (mask.size() != 2 || divide.empty() || floatAt(bytes, divide[0]) != -1) continue;

        bool literals = true;
        for (std::size_t component = 0; component < 4; ++component)
        {
            const float kept = floatAt(bytes, mask[0] + 4 * component), otherwise = floatAt(bytes, mask[1] + 4 * component);
            literals = literals && (kept == 0 || kept == 1) && otherwise == 0;
        }

        if (!literals) continue;

        std::memcpy(bytes + mask[1], bytes + mask[0], 16);
        const auto curve = steps[first + CurveStep].offset;
        std::uint32_t token{};
        std::memcpy(&token, bytes + curve, 4);
        token = (token & ~OpcodeMask) | AndOpcode;
        std::memcpy(bytes + curve, &token, 4);
        found = true;
        if (shouldLiftClamp) liftClamp(bytes, steps, first + std::size(TonemapBlock));
    }

    if (!found) return std::nullopt;

    resign(patched);

    return patched;
}

// The depth of field's float copy of the scene at `scene`'s size; call with the mutex held exclusively.
void ensureDofCopy(UINT width, UINT height)
{
    if (dofCopy.texture && dofCopy.width == width && dofCopy.height == height) return;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = SceneFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    DofCopy made{width, height};
    check(device->CreateTexture2D(&desc, nullptr, made.texture.GetAddressOf()), "Creating the depth of field's float copy");
    check(device->CreateShaderResourceView(made.texture.Get(), nullptr, made.view.GetAddressOf()), "Creating the depth of field's float copy");
    dofCopy = std::move(made);
}

// The game copies the scene into its 8-bit copy for the depth of field: the same scene goes into the
// float copy, which the composite then reads.
void copyForDof(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11RenderTargetView> target;
    context->PSGetShaderResources(0, 1, source.GetAddressOf());
    context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
    const auto scene = resourceOf(source.Get()), copy = resourceOf(target.Get());
    const auto copyDesc = textureDesc(copy.Get());
    if (!scene || !copy || copyDesc.Format != GameSceneFormat) return;

    ComPtr<ID3D11Texture2D> texture;
    {
        std::unique_lock lock(mutex);
        const auto found = targets.find(scene.Get());
        if (found == targets.end() || found->second.width != copyDesc.Width || found->second.height != copyDesc.Height) return;

        ensureDofCopy(copyDesc.Width, copyDesc.Height);
        texture = dofCopy.texture;
    }

    const performance::Timed timed(context, performance::Pass::HdrDecode);
    context->CopyResource(texture.Get(), scene.Get());
    gameDofCopy = copy.Get();
}

// The depth of field's composite reads the float copy in place of the game's.
void composeDofOverFloat(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11ShaderResourceView> bound;
    context->PSGetShaderResources(DofSceneSlot, 1, bound.GetAddressOf());
    if (!gameDofCopy || resourceOf(bound.Get()).Get() != gameDofCopy) return;

    ComPtr<ID3D11ShaderResourceView> view;
    {
        std::shared_lock lock(mutex);
        view = dofCopy.view;
    }

    context->PSSetShaderResources(DofSceneSlot, 1, view.GetAddressOf());
}
}

void enable() noexcept
{
    floatScene = true;
    log("The scene targets are made float; HDR output follows the settings.");
}

bool isSceneFloat() noexcept
{
    return floatScene.load(std::memory_order_relaxed);
}

bool active() noexcept
{
    return isSceneFloat() && outputWanted.load(std::memory_order_relaxed);
}

Output output() noexcept
{
    if (!outputWanted.load(std::memory_order_relaxed)) return Output::Off;
    // The float scene could not start, or this build of the game is not supported.
    if (sceneFailed || unsupportedBuild) return Output::Unavailable;
    // Started without the float scene: HDR needs the restart the menu asks for.
    if (!isSceneFloat()) return Output::Off;

    return shown;
}

float peakNitsFor(float chosen) noexcept
{
    const float reported = displayPeak;
    return chosen > 0 ? chosen : reported > 0 ? reported : FallbackPeakNits;
}

void attach(ID3D11Device* gameDevice) noexcept
{
    if (!isSceneFloat()) return;

    std::unique_lock lock(mutex);
    device = gameDevice;
    try
    {
        createPipeline();
    }
    catch (const std::exception& error)
    {
        // The game has made none of its targets yet, so it runs with its own 8-bit scene.
        log(std::string(active() ? "HDR output is unavailable: " : "The scene stays 8-bit: ") + error.what());
        floatScene = false;
        sceneFailed = true;
    }
}

void detach() noexcept
{
    shown = Output::Off;
    sceneFailed = false;
    twinsInUse = false;
    tenBitChain = false;
    {
        std::lock_guard outputLock(outputMutex);
        appliedOutput.reset();
    }
    std::unique_lock lock(mutex);
    targets.clear();
    srgbViews.clear();
    hasSkyVariants = false;
    skyShaders.clear();
    patchedByCode.clear();
    patchedShaders.clear();
    dofCopy = {};
    captureShader.Reset();
    encodeShader.Reset();
    decodeShader.Reset();
    for (auto& shader : gradeShaders) shader.Reset();
    reduceShader.Reset();
    outputShader.Reset();
    outputBuffer.Reset();
    curveView.Reset();
    curveAccess.Reset();
    curveBuffer.Reset();
    device.Reset();
}

void configure(const Configuration& configuration) noexcept
{
    // Kept whatever the scene is: the settings come before enable() at start, and HDR output must apply
    // to the first frame. active() and output() take the scene into account.
    outputWanted = configuration.settings.hdr.on;
    unsupportedBuild = !configuration.supportedBuild;
    peakNits = configuration.settings.hdr.peakNits;
    paperWhiteNits = configuration.settings.hdr.paperWhiteNits;
}

bool upgradeSwapChain(DXGI_FORMAT& format, DXGI_SWAP_EFFECT& effect, UINT& bufferCount) noexcept
{
    // In SDR too, so HDR output can turn on later: the swap chain's format and swap effect are set
    // when it is made, and the game holds views of its back buffer that a resize would need released.
    if (!isSceneFloat()) return false;

    format = BackBufferFormat;
    // HDR needs flip model, which takes at least two buffers.
    if (effect == DXGI_SWAP_EFFECT_DISCARD || effect == DXGI_SWAP_EFFECT_SEQUENTIAL) effect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    bufferCount = std::max(bufferCount, 2u);
    tenBitChain = true;
    logOnce("Float scene: the swap chain is R10G10B10A2 with flip model, so HDR output can turn on and off.");

    return true;
}

bool tenBitBackBuffer() noexcept
{
    return tenBitChain.load(std::memory_order_relaxed);
}

namespace
{
// Sets the colour space for the HDR output wanted, when it is not what was last set. The display is
// asked each time, so one whose HDR was turned on in Windows since is followed. Call with outputMutex
// held; returns true when the colour space was set.
bool applyOutput(IDXGISwapChain* chain)
{
    const bool wanted = active();
    if (!tenBitBackBuffer() || !chain || appliedOutput == wanted) return false;

    ComPtr<IDXGISwapChain3> chain3;
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&chain3))))
    {
        appliedOutput = wanted;
        logOnce("HDR: the swap chain has no colour spaces (IDXGISwapChain3).");
        return false;
    }

    // The display shows HDR when Windows has it on for the output that holds the window.
    bool hdrDisplay{};
    float maxNits{};
    ComPtr<IDXGIOutput> output;
    ComPtr<IDXGIOutput6> output6;
    DXGI_OUTPUT_DESC1 outputDesc{};
    if (SUCCEEDED(chain->GetContainingOutput(output.GetAddressOf())) && SUCCEEDED(output.As(&output6))
        && SUCCEEDED(output6->GetDesc1(&outputDesc)))
    {
        hdrDisplay = outputDesc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        maxNits = outputDesc.MaxLuminance;
        displayPeak = maxNits > 0 ? maxNits : 0;
    }

    const bool hdr10 = wanted && hdrDisplay;
    UINT support{};
    const auto space = hdr10 ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (FAILED(chain3->CheckColorSpaceSupport(space, &support)) || !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)
        || FAILED(chain3->SetColorSpace1(space)))
    {
        // The colour space as it was, which is what shown and the output shader keep; the change is
        // tried again at the next present.
        logOnce(hdr10 ? "HDR: the swap chain cannot present HDR10, so the game shows SDR." : "HDR: the swap chain refused the sRGB colour space.");
        return false;
    }

    appliedOutput = wanted;
    shown = hdr10 ? Output::Hdr10 : Output::Sdr;
    if (hdr10) log("HDR: the swap chain presents HDR10; the display reports " + std::to_string(std::lround(maxNits)) + " nits at most.");
    else if (wanted) log("HDR: Windows HDR is off for this display, so the game shows SDR.");
    else log("HDR output is off: the swap chain presents sRGB.");

    return true;
}
}

void swapChainReady(IDXGISwapChain* chain) noexcept
{
    std::lock_guard lock(outputMutex);
    appliedOutput.reset();
    // A swap chain starts in sRGB (DXGI's default); applyOutput gives it the wanted one when it can.
    shown = Output::Sdr;
    applyOutput(chain);
}

bool present(IDXGISwapChain* chain) noexcept
{
    if (!tenBitBackBuffer()) return false;

    std::lock_guard lock(outputMutex);

    return applyOutput(chain);
}

bool upgradeTexture(DXGI_FORMAT& format, bool& unorderedAccess) noexcept
{
    creatingTarget = false;
    if (!isSceneFloat() || format != GameSceneFormat || !madeForScene()) return false;

    format = SceneFormat;
    unorderedAccess = true;
    creatingTarget = true;

    return true;
}

void textureCreated(ID3D11Resource* texture) noexcept
{
    if (!std::exchange(creatingTarget, false)) return;

    const auto desc = textureDesc(texture);
    if (desc.Format != SceneFormat || !(desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)) return;

    try
    {
        std::unique_lock lock(mutex);
        targets[texture] = Upgraded{desc.Width, desc.Height};
    }
    catch (...)
    {
        return;
    }

    log("Float scene: a scene target at " + std::to_string(desc.Width) + "x" + std::to_string(desc.Height) + " is R16G16B16A16_FLOAT.");
}

void resourceDestroyed(ID3D11Resource* resource) noexcept
{
    {
        std::shared_lock lock(mutex);
        if (!targets.contains(resource)) return;
    }

    std::unique_lock lock(mutex);
    targets.erase(resource);
}

bool upgradeView(ID3D11Resource* resource, DXGI_FORMAT& format) noexcept
{
    creatingSrgbView = false;
    if (!isSceneFloat()) return false;

    bool upgraded{};
    {
        std::shared_lock lock(mutex);
        upgraded = targets.contains(resource);
    }

    if (upgraded)
    {
        if (format != DXGI_FORMAT_B8G8R8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) return false;

        creatingSrgbView = format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        format = SceneFormat;

        return true;
    }

    // The game draws GammaControl into the back buffer through an R8G8B8A8_UNORM_SRGB view, which the
    // R10G10B10A2 back buffer cannot have.
    if (!tenBitBackBuffer()) return false;

    const bool eightBit = format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        || format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (!eightBit || textureDesc(resource).Format != BackBufferFormat) return false;

    format = BackBufferFormat;

    return true;
}

void viewCreated(ID3D11View* view) noexcept
{
    if (!std::exchange(creatingSrgbView, false) || !view) return;

    try
    {
        const auto resource = resourceOf(view);
        std::unique_lock lock(mutex);
        const auto target = targets.find(resource.Get());
        if (target == targets.end()) return;

        srgbViews[view] = resource.Get();
        ComPtr<ID3D11ShaderResourceView> shaderView;
        if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&shaderView)))) target->second.readAsSrgb = true;
    }
    catch (...)
    {
    }
}

void viewDestroyed(ID3D11View* view) noexcept
{
    {
        std::shared_lock lock(mutex);
        if (!srgbViews.contains(view)) return;
    }

    std::unique_lock lock(mutex);
    srgbViews.erase(view);
}

const std::vector<std::uint8_t>* patchedShader(const void* code, std::size_t size) noexcept
{
    if (!isSceneFloat() || !code) return nullptr;

    try
    {
        const auto hash = dxbcHashOf(code, size);
        if (!hash) return nullptr;

        std::unique_lock lock(mutex);
        if (const auto found = patchedShaders.find(*hash); found != patchedShaders.end()) return &found->second.code;

        const bool draw2D = draw2DSource(std::string_view(static_cast<const char*>(code), size), "_ps.hlsl");
        const bool lift = !active();
        const auto patched = draw2D ? saturateOutput(code, size) : withoutTonemap(code, size, lift);
        if (!patched)
        {
            if (draw2D) logOnce("Float scene: the 2D pass shader " + toHex(*hash) + " keeps its writes unclamped.");
            return nullptr;
        }

        // Made once here first, so a patch Direct3D refuses leaves the game its own shader.
        ComPtr<ID3D11PixelShader> made;
        if (!device || FAILED(device->CreatePixelShader(patched->data(), patched->size(), nullptr, made.GetAddressOf())))
        {
            logOnce("Float scene: Direct3D refused the patched shader " + toHex(*hash) + "; it keeps the game's.");
            return nullptr;
        }

        // The sky's other variant, for HDR output turned the other way; none for shaders without the clamp.
        ComPtr<ID3D11PixelShader> other;
        if (!draw2D)
            if (const auto variant = withoutTonemap(code, size, !lift); variant && *variant != *patched
                && FAILED(device->CreatePixelShader(variant->data(), variant->size(), nullptr, other.GetAddressOf())))
            {
                other.Reset();
                logOnce("Float scene: Direct3D refused the other sky variant of " + toHex(*hash) + "; its clamp follows the setting at start.");
            }

        const auto* original = static_cast<const std::uint8_t*>(code);
        auto& entry = patchedShaders[*hash] = Patched{std::move(*patched), std::vector(original, original + size), lift, std::move(other)};
        patchedByCode[entry.code.data()] = &entry;
        log(std::string("Float scene: ") + (draw2D ? "clamped the writes of 2D pass shader " : "took TppTonemap out of shader ") + toHex(*hash)
            + (entry.other ? ", with both sky clamps." : "."));

        return &entry.code;
    }
    catch (...)
    {
        return nullptr;
    }
}

std::pair<const void*, std::size_t> originalOf(const void* code, std::size_t size) noexcept
{
    if (!isSceneFloat()) return {code, size};

    std::shared_lock lock(mutex);
    const auto found = patchedByCode.find(code);
    if (found == patchedByCode.end()) return {code, size};

    return {found->second->original.data(), found->second->original.size()};
}

void pixelShaderCreated(const void* code, ID3D11PixelShader* made) noexcept
{
    if (!isSceneFloat() || !code || !made) return;

    try
    {
        ComPtr<ID3D11PixelShader> other;
        std::vector<std::uint8_t> original;
        {
            std::unique_lock lock(mutex);
            const auto found = patchedByCode.find(code);
            if (found == patchedByCode.end() || !found->second->other) return;

            const auto& entry = *found->second;
            other = entry.other;
            original = entry.original;
            auto& variants = skyShaders[made];
            (entry.lifted ? variants.clampLifted : variants.clampKept) = made;
            (entry.lifted ? variants.clampKept : variants.clampLifted) = other;
            hasSkyVariants = true;
        }

        // The other variant is the same game shader to the add-on's parts.
        game_shaders::pixelShaderCreated(original.data(), original.size(), other.Get());
    }
    catch (...)
    {
    }
}

ID3D11PixelShader* shaderFor(ID3D11PixelShader* bound) noexcept
{
    if (!hasSkyVariants.load(std::memory_order_relaxed) || !bound) return nullptr;

    std::shared_lock lock(mutex);
    const auto found = skyShaders.find(bound);
    if (found == skyShaders.end()) return nullptr;

    auto* wanted = (active() ? found->second.clampKept : found->second.clampLifted).Get();

    return wanted != bound ? wanted : nullptr;
}

bool beforeDraw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    if (!isSceneFloat() || call.indexed || call.count != 3 || call.instances != 1) return false;

    try
    {
        // The resolves with TppTonemap give its curve; CopyRenderBuffer copies the scene into the depth of
        // field's copy, and DOF_ScatterCompositeFinal draws the depth of field over it.
        if (const auto* row = shader.row())
        {
            if (row->pass == GamePass::Resolve && row->hasTppTonemap) captureCurve(context);
            else if (row->pass == GamePass::Copy) copyForDof(context);
            else if (row->pass == GamePass::DofComposite) composeDofOverFloat(context);
        }

        ComPtr<ID3D11RenderTargetView> target;
        context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
        {
            std::shared_lock lock(mutex);
            if (!target || !srgbViews.contains(target.Get())) return false;
        }

        ComPtr<ID3D11BlendState> blend;
        FLOAT factor[4]{};
        UINT mask{};
        context->OMGetBlendState(blend.GetAddressOf(), factor, &mask);
        if (!blend) return true;

        D3D11_BLEND_DESC desc{};
        blend->GetDesc(&desc);
        // CopyDepthBuffer binds the scene target without writing it.
        if (!desc.RenderTarget[0].RenderTargetWriteMask) return false;
        if (desc.RenderTarget[0].BlendEnable)
        {
            logOnce("Float scene: a blended draw through an sRGB view of the scene is left as the game draws it.");
            return false;
        }

        return true;
    }
    catch (...)
    {
        return false;
    }
}

void encode(ID3D11DeviceContext* context) noexcept
{
    try
    {
        ID3D11RenderTargetView* bound[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ComPtr<ID3D11DepthStencilView> depth;
        context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, bound, depth.GetAddressOf());
        ComPtr<ID3D11RenderTargetView> views[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) views[i].Attach(bound[i]);

        const auto resource = resourceOf(views[0].Get());
        ComPtr<ID3D11UnorderedAccessView> access;
        UINT width{}, height{};
        {
            std::shared_lock lock(mutex);
            const auto target = targets.find(resource.Get());
            if (target == targets.end() || !encodeShader) return;

            width = target->second.width;
            height = target->second.height;
            check(device->CreateUnorderedAccessView(resource.Get(), nullptr, access.GetAddressOf()), "Viewing the scene target");
        }

        // The target cannot be a render target and an unordered access view at once.
        context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
        {
            SavedComputeState saved(context, PassSlots);
            const performance::Timed timed(context, performance::Pass::HdrEncode);
            context->CSSetUnorderedAccessViews(0, 1, access.GetAddressOf(), nullptr);
            context->CSSetShader(encodeShader.Get(), nullptr, 0);
            context->Dispatch(groups(width), groups(height), 1);
        }

        context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, bound, depth.Get(), 0,
            D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
    }
    catch (const std::exception& error)
    {
        logOnce(std::string("Float scene: ") + error.what());
    }
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    if (!isSceneFloat() || call.indexed || call.count != 3 || call.instances != 1) return false;

    try
    {
        const auto* row = shader.row();
        if (!row) return false;

        ID3D11PixelShader* replacement{};
        if (row->pass == GamePass::Grade) replacement = gradeShaders[active() ? 1 : 0].Get();
        else if (row->pass == GamePass::Reduce)
        {
            // Only the reduction of the scene itself; the game runs it again on its own result.
            ComPtr<ID3D11ShaderResourceView> source;
            context->PSGetShaderResources(0, 1, source.GetAddressOf());
            std::shared_lock lock(mutex);
            if (targets.contains(resourceOf(source.Get()).Get())) replacement = reduceShader.Get();
        }

        if (!replacement) return false;

        ComPtr<ID3D11ShaderResourceView> slot;
        context->PSGetShaderResources(CurveSlot, 1, slot.GetAddressOf());
        context->PSSetShader(replacement, nullptr, 0);
        context->PSSetShaderResources(CurveSlot, 1, curveView.GetAddressOf());
        performance::Timed timed(context, performance::Pass::HdrGrade);
        context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
        timed.end();
        context->PSSetShader(shader.object(), nullptr, 0);
        context->PSSetShaderResources(CurveSlot, 1, slot.GetAddressOf());

        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool clearTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* view, const float color[4]) noexcept
{
    if (!isSceneFloat() || !view) return false;

    {
        std::shared_lock lock(mutex);
        if (!srgbViews.contains(view)) return false;
    }

    const float encoded[4]{linearToSrgb(color[0]), linearToSrgb(color[1]), linearToSrgb(color[2]), color[3]};
    context->ClearRenderTargetView(view, encoded);

    return true;
}

bool copy(ID3D11DeviceContext* context, ID3D11Resource* source, ID3D11Resource* destination) noexcept
{
    if (!isSceneFloat()) return false;

    try
    {
        ComPtr<ID3D11ShaderResourceView> sourceView;
        ComPtr<ID3D11UnorderedAccessView> twinAccess;
        UINT width{}, height{};
        {
            std::shared_lock lock(mutex);
            const auto target = targets.find(destination);
            if (target == targets.end() || !target->second.readAsSrgb) return false;
        }

        {
            std::unique_lock lock(mutex);
            const auto target = targets.find(destination);
            if (target == targets.end() || !decodeShader) return false;

            ensureTwin(target->second);
            twinAccess = target->second.twinAccess;
            width = target->second.width;
            height = target->second.height;
            check(device->CreateShaderResourceView(destination, nullptr, sourceView.GetAddressOf()), "Viewing the scene copy");
        }

        const performance::Timed timed(context, performance::Pass::HdrDecode);
        context->CopyResource(destination, source);
        SavedComputeState saved(context, PassSlots);
        context->CSSetShaderResources(0, 1, sourceView.GetAddressOf());
        context->CSSetUnorderedAccessViews(0, 1, twinAccess.GetAddressOf(), nullptr);
        context->CSSetShader(decodeShader.Get(), nullptr, 0);
        context->Dispatch(groups(width), groups(height), 1);

        return true;
    }
    catch (const std::exception& error)
    {
        logOnce(std::string("Float scene: ") + error.what());
        return false;
    }
}

bool replaceViews(unsigned count, ID3D11ShaderResourceView* const* views, ID3D11ShaderResourceView** replaced) noexcept
{
    if (!twinsInUse.load(std::memory_order_relaxed) || !views) return false;

    bool any{};
    std::shared_lock lock(mutex);
    for (unsigned i = 0; i < count; ++i)
    {
        replaced[i] = views[i];
        const auto view = views[i] ? srgbViews.find(views[i]) : srgbViews.end();
        if (view == srgbViews.end()) continue;

        const auto target = targets.find(view->second);
        if (target == targets.end() || !target->second.twinView) continue;

        replaced[i] = target->second.twinView.Get();
        any = true;
    }

    return any;
}

OutputShader::OutputShader(ID3D11DeviceContext* target) noexcept
{
    if (!isSceneFloat() || !outputShader) return;

    // The back buffer is R10G10B10A2, encoded here: in PQ while HDR output is on and the display shows
    // HDR, sRGB otherwise.
    const auto encoding = active() && shown == Output::Hdr10 ? Encoding::Pq : Encoding::Srgb;

    const OutputParameters parameters{paperWhiteNits, peakNitsFor(peakNits), encoding, 0};
    if (FAILED(write(target, outputBuffer.Get(), parameters))) return;

    target->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
    target->PSGetConstantBuffers(OutputSlot, 1, gameBuffer.GetAddressOf());
    target->PSSetShader(outputShader.Get(), nullptr, 0);
    target->PSSetConstantBuffers(OutputSlot, 1, outputBuffer.GetAddressOf());
    context = target;
}

OutputShader::~OutputShader()
{
    if (!context) return;

    context->PSSetShader(gameShader.Get(), nullptr, 0);
    context->PSSetConstantBuffers(OutputSlot, 1, gameBuffer.GetAddressOf());
}

DXGI_FORMAT sceneFormat() noexcept
{
    return isSceneFloat() ? SceneFormat : GameSceneFormat;
}

ComPtr<ID3D11ShaderResourceView> sceneBeforeDof(UINT width, UINT height) noexcept
{
    std::shared_lock lock(mutex);
    if (dofCopy.width != width || dofCopy.height != height) return nullptr;

    return dofCopy.view;
}
}
