// FSR upscaling on Direct3D 12 for the Direct3D 11 game; fsr_upscaler.hpp describes the route.
#include "features/upscaling/fsr_upscaler.hpp"

#include "features/upscaling/d3d12_host.hpp"
#include "fidelityfx.hpp"
#include "log.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <api/include/dx12/ffx_api_dx12.h>
#include <upscalers/include/ffx_upscale.h>
#include <memory>
#include <string>

namespace phantom_fx::fsr_upscaler
{
namespace
{
// What the FSR context was created for.
struct ContextKey
{
    UINT renderWidth{}, renderHeight{}, outputWidth{}, outputHeight{};
    bool hdr{};

    bool operator==(const ContextKey&) const = default;
};

struct State
{
    State(const fidelityfx::Api& api, ID3D11Device* device) : api(&api), host(device, "FSR") {}

    const fidelityfx::Api* api;
    d3d12_host::Host host;

    ffxContext context{};
    ContextKey key;
    std::string version;
    OpenedTexture color, depth, motion, output;
};
std::unique_ptr<State> state;

void logMessage(uint32_t type, const wchar_t* message)
{
    std::string text(type == FFX_API_MESSAGE_TYPE_ERROR ? "FSR error: " : "FSR warning: ");
    for (const wchar_t* c = message; c && *c; ++c) text += *c < 128 ? static_cast<char>(*c) : '?';
    log(text);
}

void destroyContext(State& s)
{
    if (!s.context) return;

    // FSR's resources may still be in use by the last upscale.
    s.host.waitForLast();
    s.api->destroyContext(&s.context, nullptr);
    s.context = nullptr;
}

// FSR keeps its history at the sizes it was created for, so a new mode or output size needs a new
// context. Returns true when it made one.
bool ensureContext(State& s, const ContextKey& key)
{
    if (s.context && s.key == key) return false;

    destroyContext(s);
    ffxCreateContextDescUpscale create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    // Fox Engine's projection is reversed-Z with a finite far plane.
    create.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (key.hdr) create.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    create.maxRenderSize = {key.renderWidth, key.renderHeight};
    create.maxUpscaleSize = {key.outputWidth, key.outputHeight};
    create.fpMessage = &logMessage;
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = s.host.device();
    ffxCreateContextDescUpscaleVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;
    create.header.pNext = &backend.header;
    backend.header.pNext = &version.header;
    fidelityfx::check(s.api->createContext(&s.context, &create.header, nullptr), "Creating the FSR context");
    s.key = key;

    const auto running = fidelityfx::version(*s.api, s.context);
    log("Created FSR " + (running.empty() ? std::string("(unknown version)") : running) + ": " + std::to_string(key.renderWidth) + "x"
        + std::to_string(key.renderHeight) + " -> " + std::to_string(key.outputWidth) + "x" + std::to_string(key.outputHeight)
        + (key.hdr ? ", HDR." : "."));
    s.version = running;

    return true;
}

}

void start(ID3D11Device* device, const std::filesystem::path& directory)
{
    if (state) return;

    state = std::make_unique<State>(fidelityfx::load(directory), device);
    log("FSR started on Direct3D 12.");
}

bool evaluate(const Inputs& inputs)
{
    auto& s = *state;
    D3D11_TEXTURE2D_DESC color{}, output{};
    inputs.color->GetDesc(&color);
    inputs.output->GetDesc(&output);
    const bool created = ensureContext(s, {color.Width, color.Height, output.Width, output.Height, inputs.hdr});

    s.host.evaluate([&](d3d12_host::Evaluation& evaluation) {
        ffxDispatchDescUpscale dispatch{};
        dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        dispatch.commandList = evaluation.list();
        dispatch.color = ffxApiGetResourceDX12(evaluation.read(s.color, inputs.color, "Opening the FSR colour"), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.depth = ffxApiGetResourceDX12(evaluation.read(s.depth, inputs.depth, "Opening the FSR depth"), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.motionVectors = ffxApiGetResourceDX12(evaluation.read(s.motion, inputs.motion, "Opening the FSR motion"), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.output = ffxApiGetResourceDX12(evaluation.write(s.output, inputs.output, "Opening the FSR output"), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch.jitterOffset = {inputs.jitter[0], inputs.jitter[1]};
        dispatch.motionVectorScale = {1, 1};
        dispatch.renderSize = {color.Width, color.Height};
        dispatch.upscaleSize = {output.Width, output.Height};
        // The composite sharpens with RCAS itself, as for DLSS.
        dispatch.enableSharpening = false;
        dispatch.frameTimeDelta = inputs.frameTime;
        dispatch.preExposure = 1;
        dispatch.reset = inputs.reset || created;
        dispatch.cameraNear = inputs.cameraNear;
        dispatch.cameraFar = inputs.cameraFar;
        dispatch.cameraFovAngleVertical = inputs.verticalFov;
        // Fox Engine's units are metres.
        dispatch.viewSpaceToMetersFactor = 1;
        fidelityfx::check(s.api->dispatch(&s.context, &dispatch.header), "Upscaling with FSR");
    });

    return created;
}

void stop() noexcept
{
    if (!state) return;

    try
    {
        destroyContext(*state);
    }
    catch (const std::exception& error)
    {
        log(error.what());
    }

    state.reset();
    log("FSR stopped.");
}

std::string name()
{
    return state && !state->version.empty() ? "FSR " + state->version : "FSR";
}
}
