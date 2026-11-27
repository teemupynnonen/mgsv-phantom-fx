// DLSS Frame Generation through Streamline; frame_generation.hpp describes the route.
#include "features/frame_generation/frame_generation.hpp"

#include "features/hdr/hdr.hpp"

#include "log.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_helpers.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_security.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>

namespace phantom_fx::frame_generation
{
namespace
{
const sl::ViewportHandle Viewport{0u};

// The Streamline functions the add-on calls, from sl.interposer.dll and the plugins it loads.
struct Api
{
    HMODULE module{};
    PFun_slInit* init{};
    PFun_slShutdown* shutdown{};
    PFun_slIsFeatureSupported* isFeatureSupported{};
    PFun_slSetD3DDevice* setDevice{};
    PFun_slUpgradeInterface* upgradeInterface{};
    PFun_slGetNativeInterface* getNativeInterface{};
    PFun_slGetFeatureFunction* getFeatureFunction{};
    PFun_slGetNewFrameToken* getFrameToken{};
    PFun_slSetTagForFrame* setTags{};
    PFun_slSetConstants* setConstants{};
    PFun_slDLSSGSetOptions* setOptions{};
    PFun_slDLSSGGetState* getState{};
    PFun_slReflexSetOptions* setReflexOptions{};
    PFun_slReflexGetState* getReflexState{};
    PFun_slReflexSleep* sleep{};
    PFun_slPCLSetMarker* setMarker{};
} api;

bool initialized{};
// Read by the game loop's and render threads for their markers.
std::atomic<bool> attached{};
// The game loop's thread marks the simulation; otherwise the presenting thread does.
std::atomic<bool> simulationMarked{};

// What the settings ask for, what the GPU allows and what Streamline was last told.
std::atomic<unsigned> wantedMultiplier{2};
std::atomic<bool> wantedDynamic{};
std::atomic<unsigned> maxMultiplier{};
std::atomic<bool> dynamicSupported{};
std::atomic<unsigned> presentedPerRendered{1};
struct Mode
{
    bool on{}, dynamic{};
    unsigned generatedFrames{1};

    bool operator==(const Mode&) const = default;
} applied;

std::mutex statusMutex;
std::string statusText{"Off."};
std::uint64_t presented{};

void setStatus(std::string text)
{
    std::lock_guard lock(statusMutex);
    statusText = std::move(text);
}

void check(sl::Result result, const char* what)
{
    if (result != sl::Result::eOk) throw std::runtime_error(std::string(what) + " failed (" + sl::getResultAsStr(result) + ").");
}

// Logs a failure once per call site; per-frame calls would otherwise flood the log.
void report(sl::Result result, const char* what)
{
    if (result == sl::Result::eOk) return;

    static std::mutex loggedMutex;
    static std::string logged;
    std::lock_guard lock(loggedMutex);
    if (logged.find(what) != std::string::npos) return;

    logged += what;
    log(std::string(what) + " failed (" + sl::getResultAsStr(result) + ").");
}

// Streamline's warnings and errors also go to PhantomFX.log, up to a limit: some repeat every frame.
// sl.log beside the add-on keeps all of them.
void logMessage(sl::LogType type, const char* message)
{
    static std::atomic<int> forwarded{};
    if (type == sl::LogType::eInfo || ++forwarded > 50) return;

    std::string text = std::string("Streamline: ") + message;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    log(text);
}

template<class Function> void resolve(Function*& function, const char* name)
{
    function = reinterpret_cast<Function*>(GetProcAddress(api.module, name));
    if (!function) throw std::runtime_error(std::string("sl.interposer.dll lacks ") + name + ".");
}

template<class Function> void resolve(sl::Feature feature, Function*& function, const char* name)
{
    void* address{};
    check(api.getFeatureFunction(feature, name, address), name);
    function = reinterpret_cast<Function*>(address);
}

sl::FrameToken* token(std::uint32_t frame)
{
    sl::FrameToken* result{};
    return api.getFrameToken(result, &frame) == sl::Result::eOk ? result : nullptr;
}

sl::float4x4 matrix(const std::array<float, 16>& values)
{
    sl::float4x4 result;
    for (int row = 0; row < 4; ++row) result[row] = sl::float4(values[row * 4], values[row * 4 + 1], values[row * 4 + 2], values[row * 4 + 3]);

    return result;
}

sl::float3 vector(const std::array<float, 3>& values)
{
    return sl::float3(values[0], values[1], values[2]);
}

sl::Constants constants(const Inputs& inputs)
{
    const auto& camera = inputs.camera;
    sl::Constants values{};
    values.cameraViewToClip = matrix(camera.viewToClip);
    values.clipToCameraView = matrix(camera.clipToView);
    values.clipToLensClip = matrix({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    values.clipToPrevClip = matrix(camera.clipToPreviousClip);
    values.prevClipToClip = matrix(camera.previousClipToClip);
    values.jitterOffset = sl::float2(inputs.jitter[0], inputs.jitter[1]);
    values.mvecScale = sl::float2(1.0f / static_cast<float>(inputs.renderWidth), 1.0f / static_cast<float>(inputs.renderHeight));
    values.cameraPinholeOffset = sl::float2(0, 0);
    values.cameraPos = vector(camera.position);
    values.cameraRight = vector(camera.right);
    values.cameraUp = vector(camera.up);
    values.cameraFwd = vector(camera.forward);
    values.cameraNear = camera.nearPlane;
    values.cameraFar = camera.farPlane;
    values.cameraFOV = camera.verticalFov;
    values.cameraAspectRatio = camera.aspectRatio;
    values.depthInverted = sl::Boolean::eTrue;
    values.cameraMotionIncluded = sl::Boolean::eTrue;
    values.motionVectors3D = sl::Boolean::eFalse;
    values.reset = inputs.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    // The motion pass removes the jitter (docs/DEVELOPMENT.md, "Motion vectors").
    values.motionVectorsJittered = sl::Boolean::eFalse;

    return values;
}

// Tells Streamline what to generate when the settings or the frame's inputs change it.
void applyMode(bool valid)
{
    const auto multiplier = wantedMultiplier.load();
    const auto most = maxMultiplier.load();
    Mode wanted;
    wanted.dynamic = wantedDynamic;
    wanted.on = valid && (wanted.dynamic || multiplier >= 2);
    if (!wanted.dynamic && multiplier >= 2) wanted.generatedFrames = (most ? std::min(multiplier, most) : multiplier) - 1;
    if (!wanted.on) wanted = {};
    if (wanted == applied) return;

    sl::DLSSGOptions options{};
    options.mode = !wanted.on ? sl::DLSSGMode::eOff : wanted.dynamic ? sl::DLSSGMode::eDynamic : sl::DLSSGMode::eOn;
    options.numFramesToGenerate = wanted.generatedFrames;
    // Frames without DLSS (menus, loading) turn generation off briefly; keep its memory meanwhile.
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    // The hudless texture is typeless, so its sRGB view can draw it (upscaler.cpp, "hudless"); with the
    // float scene's swap chain it has the back buffer's R10G10B10A2.
    options.hudLessBufferFormat = hdr::tenBitBackBuffer() ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    // Dynamic generation follows the display's refresh rate.
    options.dynamicTargetFrameRate = 0;
    report(api.setOptions(Viewport, options), "Setting the frame generation mode");
    applied = wanted;
}

void marker(sl::PCLMarker which, std::uint32_t frame)
{
    if (auto* frameToken = token(frame)) report(api.setMarker(which, *frameToken), "Setting a PC Latency marker");
}

// Reflex's report: from the simulation's start to the GPU finishing the frame, averaged over its
// recent frames. A frame is complete only when its markers on every thread carry the same number.
std::string latency()
{
    static sl::ReflexState state;
    if (api.getReflexState(state) != sl::Result::eOk || !state.latencyReportAvailable) return " Reflex reports no latency.";

    std::uint64_t total{}, frames{};
    for (const auto& entry : state.frameReport)
        if (entry.simStartTime && entry.gpuRenderEndTime > entry.simStartTime)
        {
            total += entry.gpuRenderEndTime - entry.simStartTime;
            ++frames;
        }

    if (!frames) return " Reflex has no complete frames.";

    return " Latency " + std::to_string((total / frames + 500) / 1000) + " ms.";
}

void updateStatus()
{
    if (++presented % 30) return;

    sl::DLSSGState state{};
    if (api.getState(Viewport, state, nullptr) != sl::Result::eOk) return setStatus("Streamline does not report its state.");

    maxMultiplier = state.numFramesToGenerateMax + 1;
    dynamicSupported = state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
    const bool generating = state.status == sl::DLSSGStatus::eOk && (wantedDynamic || wantedMultiplier >= 2) && applied.on;
    presentedPerRendered = generating ? std::max(1u, static_cast<unsigned>(state.numFramesActuallyPresented)) : 1u;
    if (state.status != sl::DLSSGStatus::eOk)
    {
        std::string reasons;
        const auto add = [&](sl::DLSSGStatus flag, const char* reason)
        {
            if (!(state.status & flag)) return;
            if (!reasons.empty()) reasons += "; ";
            reasons += reason;
        };
        add(sl::DLSSGStatus::eFailResolutionTooLow, "the resolution is too low");
        add(sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime, "Reflex is not detected");
        add(sl::DLSSGStatus::eFailHDRFormatNotSupported, "the HDR format is not supported");
        add(sl::DLSSGStatus::eFailCommonConstantsInvalid, "the camera constants are invalid");
        add(sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled, "the back buffer index is not queried");

        return setStatus("Not generating: " + (reasons.empty() ? std::string("unknown reason") : reasons) + ".");
    }

    if (!wantedDynamic && wantedMultiplier < 2) return setStatus("Off." + latency());
    if (!applied.on) return setStatus("Paused: DLSS or DLAA is not running on this frame." + latency());

    setStatus(std::string(applied.dynamic ? "Generating dynamically: " : "Generating: ") + std::to_string(state.numFramesActuallyPresented)
        + " frames presented per rendered frame." + latency());
}
}

bool start(const std::filesystem::path& directory, IDXGIAdapter* adapter) noexcept
{
    try
    {
        const auto interposer = directory / L"sl.interposer.dll";
        if (!std::filesystem::exists(interposer)) throw std::runtime_error("sl.interposer.dll is missing beside the add-on.");
        // Streamline loads only plugins NVIDIA signed; its loader is checked the same way.
        if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) throw std::runtime_error("sl.interposer.dll is not signed by NVIDIA.");

        api.module = LoadLibraryExW(interposer.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!api.module) throw std::runtime_error("Loading sl.interposer.dll failed.");

        resolve(api.init, "slInit");
        resolve(api.shutdown, "slShutdown");
        resolve(api.isFeatureSupported, "slIsFeatureSupported");
        resolve(api.setDevice, "slSetD3DDevice");
        resolve(api.upgradeInterface, "slUpgradeInterface");
        resolve(api.getNativeInterface, "slGetNativeInterface");
        resolve(api.getFeatureFunction, "slGetFeatureFunction");
        resolve(api.getFrameToken, "slGetNewFrameToken");
        resolve(api.setTags, "slSetTagForFrame");
        resolve(api.setConstants, "slSetConstants");

        // Manual hooking: Streamline sees only the device, queue and swap chain the presenter gives it.
        const auto plugins = directory.wstring();
        const wchar_t* paths[]{plugins.c_str()};
        const sl::Feature features[]{sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
        sl::Preferences preferences{};
        preferences.pathsToPlugins = paths;
        preferences.numPathsToPlugins = 1;
        preferences.pathToLogsAndData = paths[0];
        preferences.logMessageCallback = &logMessage;
        preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking
            | sl::PreferenceFlags::eUseDXGIFactoryProxy | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
        preferences.featuresToLoad = features;
        preferences.numFeaturesToLoad = static_cast<std::uint32_t>(std::size(features));
        preferences.engine = sl::EngineType::eCustom;
        preferences.engineVersion = "Fox Engine 1.0.15";
        preferences.projectId = NgxProjectId;
        check(api.init(preferences, sl::kSDKVersion), "Starting Streamline");
        initialized = true;

        DXGI_ADAPTER_DESC description{};
        if (FAILED(adapter->GetDesc(&description))) throw std::runtime_error("Reading the GPU's description failed.");

        sl::AdapterInfo info{};
        info.deviceLUID = reinterpret_cast<std::uint8_t*>(&description.AdapterLuid);
        info.deviceLUIDSizeInBytes = sizeof(LUID);
        if (const auto result = api.isFeatureSupported(sl::kFeatureDLSS_G, info); result != sl::Result::eOk)
            throw std::runtime_error(std::string("DLSS Frame Generation is not supported here (") + sl::getResultAsStr(result)
                + "): it needs an RTX 40 or newer GPU, a recent driver and hardware-accelerated GPU scheduling.");

        setStatus("Starting with the game's swap chain.");

        return true;
    }
    catch (const std::exception& error)
    {
        stop();
        setStatus(error.what());
        log(error.what());

        return false;
    }
}

bool attach(ID3D12Device* device) noexcept
{
    try
    {
        check(api.setDevice(device), "Giving Streamline the Direct3D 12 device");
        resolve(sl::kFeatureDLSS_G, api.setOptions, "slDLSSGSetOptions");
        resolve(sl::kFeatureDLSS_G, api.getState, "slDLSSGGetState");
        resolve(sl::kFeatureReflex, api.setReflexOptions, "slReflexSetOptions");
        resolve(sl::kFeatureReflex, api.getReflexState, "slReflexGetState");
        resolve(sl::kFeatureReflex, api.sleep, "slReflexSleep");
        resolve(sl::kFeaturePCL, api.setMarker, "slPCLSetMarker");
        // Frame generation paces its frames with Reflex.
        sl::ReflexOptions reflex{};
        reflex.mode = sl::ReflexMode::eLowLatency;
        check(api.setReflexOptions(reflex), "Turning on Reflex");
        attached = true;
        simulationMarked = false;
        applied = {};

        return true;
    }
    catch (const std::exception& error)
    {
        setStatus(error.what());
        log(error.what());

        return false;
    }
}

IUnknown* upgrade(IUnknown* native) noexcept
{
    // The proxy holds its own reference to the native object; when Streamline makes none, the
    // object comes back as it is and gets one for the caller.
    void* object = native;
    if (api.upgradeInterface(&object) != sl::Result::eOk) return nullptr;
    if (object == native) native->AddRef();

    return static_cast<IUnknown*>(object);
}

IUnknown* nativeOf(IUnknown* proxy) noexcept
{
    void* object{};
    return api.getNativeInterface(proxy, &object) == sl::Result::eOk ? static_cast<IUnknown*>(object) : nullptr;
}

void stop() noexcept
{
    // The game loop's and render threads stop marking first.
    attached = false;
    if (initialized) api.shutdown();
    initialized = false;
    applied = {};
}

void beginSimulation(std::uint32_t frame) noexcept
{
    if (!attached) return;

    simulationMarked = true;
    if (auto* frameToken = token(frame)) report(api.sleep(*frameToken), "Reflex sleep");
    marker(sl::PCLMarker::eSimulationStart, frame);
}

void endSimulation(std::uint32_t frame) noexcept
{
    if (attached) marker(sl::PCLMarker::eSimulationEnd, frame);
}

void beginRender(std::uint32_t frame) noexcept
{
    if (attached && frame) marker(sl::PCLMarker::eRenderSubmitStart, frame);
}

void tag(ID3D12GraphicsCommandList* list, const Inputs& inputs, ID3D12Resource* depth, ID3D12Resource* motion,
    ID3D12Resource* hudless) noexcept
{
    if (!attached || !inputs.valid || !inputs.frame) return;

    auto* frameToken = token(inputs.frame);
    if (!frameToken) return;

    // The shared textures change with the next frame, so Streamline copies them now, in `list`.
    sl::Resource depthResource{sl::ResourceType::eTex2d, depth, D3D12_RESOURCE_STATE_COMMON};
    sl::Resource motionResource{sl::ResourceType::eTex2d, motion, D3D12_RESOURCE_STATE_COMMON};
    sl::Resource hudlessResource{sl::ResourceType::eTex2d, hudless, D3D12_RESOURCE_STATE_COMMON};
    const sl::Extent render{0, 0, inputs.renderWidth, inputs.renderHeight};
    const sl::ResourceTag tags[]{
        {&depthResource, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &render},
        {&motionResource, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &render},
        {&hudlessResource, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow},
    };
    report(api.setTags(*frameToken, Viewport, tags, static_cast<std::uint32_t>(std::size(tags)), list), "Tagging the frame's inputs");
}

void beginPresent(const Inputs& inputs) noexcept
{
    if (!attached) return;

    applyMode(inputs.valid && inputs.frame != 0);
    updateStatus();
    auto* frameToken = inputs.frame ? token(inputs.frame) : nullptr;
    if (!frameToken) return;

    if (inputs.valid) report(api.setConstants(constants(inputs), *frameToken, Viewport), "Setting the camera constants");

    marker(sl::PCLMarker::eRenderSubmitEnd, inputs.frame);
    marker(sl::PCLMarker::ePresentStart, inputs.frame);
}

void endPresent(const Inputs& inputs) noexcept
{
    if (!attached || !inputs.frame) return;

    marker(sl::PCLMarker::ePresentEnd, inputs.frame);
    // Without the game loop's hook the next frame's sleep and simulation are marked here, which
    // delays the frame after the game has read its input.
    if (simulationMarked) return;

    const auto next = inputs.frame + 1;
    if (auto* frameToken = token(next)) report(api.sleep(*frameToken), "Reflex sleep");
    marker(sl::PCLMarker::eSimulationStart, next);
    marker(sl::PCLMarker::eSimulationEnd, next);
}

void select(unsigned multiplier, bool dynamic) noexcept
{
    wantedMultiplier = multiplier;
    wantedDynamic = dynamic;
}

unsigned presentedPerFrame() noexcept
{
    return presentedPerRendered;
}

Capabilities capabilities() noexcept
{
    return {maxMultiplier, dynamicSupported};
}

std::string status()
{
    std::lock_guard lock(statusMutex);
    return statusText;
}
}
