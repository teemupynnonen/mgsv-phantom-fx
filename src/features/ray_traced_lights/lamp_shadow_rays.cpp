#include "features/ray_traced_lights/lamp_shadow_rays.hpp"

#include "log.hpp"
#include "passes/ray_tracing/gi_scene.hpp"
#include "passes/ray_tracing/trace.hpp"
#include "performance.hpp"

#include <Windows.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace phantom_fx::lamp_shadow_rays
{
namespace
{
using namespace phantom_fx::gi;
using gi_d3d12::ComPtr;
using gi_d3d12::groups;
using gi_d3d12::Kernel;
using gi_renderer::Frame;
using gi_renderer::LocalLight;
using gi_scene::FramesInFlight;
using kick_traces::KickTraces;
using scene_contents::instancedAt;
using scene_contents::InstancedModel;
using scene_contents::SceneContents;
using shared_textures::SharedOutput;
using shared_textures::SharedRow;
using trace::TraceFrame;

constexpr SharedRow ShareRow{DXGI_FORMAT_R16_FLOAT, binding::LocalLightShadow, "Opening the lights' shadow"};

// The lamp probe as text, and a key that changes when which lights reach the centre pixel, or what
// blocks them, changes.
std::pair<std::string, std::string> describeProbe(const SceneContents& contents, std::span<const InstancedModel> instanced, std::span<const LocalLight> lights,
    const GILightProbe& probe)
{
    if (probe.receiver.w <= 0) return {"lamps: the centre pixel is the sky", "sky"};

    const auto& p = probe.receiver;
    std::string text = std::format("lamps at the centre pixel, its surface {:.1f} m away at ({:.1f} {:.1f} {:.1f}): {} of the frame's {} lights light "
        "it; the rays to their centres let {:.0f}% of their light through, the filtered share they are drawn with {:.2f}\n",
        p.w, p.x, p.y, p.z, static_cast<int>(probe.share.w), lights.size(), probe.share.x * 100, probe.share.y);
    const auto& start = probe.start;
    const float offset = std::sqrt(start.x * start.x + start.y * start.y + start.z * start.z) * 1000.0f;
    text += start.w > 0.5f ? std::format("the rays leave the scene's own surface, {:.1f} mm from the depth's point: {}\n", offset,
                                 contents.describeHit(instanced, probe.startHit))
                           : std::string("the rays leave the depth's point: the scene has no surface there\n");
    text += std::format("the camera's ray (the scene view's): {}\n", contents.describeHit(instanced, probe.view));

    // A character the rays leave, or the strongest light's blocker: every mesh of it.
    const auto characterOf = [&](const uint4& hit) {
        return hit.x && (hit.z & GI_INSTANCE_CHARACTER) ? instancedAt(instanced, hit.x - 1) : nullptr;
    };
    const auto* character = characterOf(probe.startHit);
    if (!character) character = characterOf(probe.hits[0]);
    if (character) text += std::format("the character's meshes:{}\n", contents.charactersAt(character->position));

    std::string key;
    for (std::uint32_t i = 0; i < GI_PROBE_LIGHTS; ++i)
    {
        const auto& entry = probe.lights[i];
        if (entry.x < 1) break;

        const auto index = static_cast<std::size_t>(entry.x) - 1;
        std::string what = "light " + std::to_string(index);
        if (index < lights.size())
        {
            const auto& light = lights[index];
            const bool spot = light.cosOuter >= -1;
            what += std::format(" ({} at ({:.1f} {:.1f} {:.1f}){}, reach {:.1f} m{})", spot ? "spot" : "point", light.position[0], light.position[1],
                light.position[2], spot ? std::format(" towards ({:.2f} {:.2f} {:.2f})", light.axis[0], light.axis[1], light.axis[2]) : "", light.range,
                light.shadowStart > 0 ? std::format(", the game's shadow map from {:.2f} m", light.shadowStart) : "");
        }

        // The ray met something (w: how far along) that blocked it, or hair that let some of the light through.
        const float through = probe.through[i].x;
        const auto outcome = entry.w < 0 ? std::string("reaches it") : through <= 0 ? std::string("blocked") : std::format("{:.0f}% gets past hair", through * 100);
        text += std::format("{}: {:.0f}% of their light, {:.1f} m away, {}\n", what, probe.share.z > 0 ? entry.y / probe.share.z * 100 : 0.0f, entry.z, outcome);
        // Each hit on the way to the light, and how far short of it.
        for (std::uint32_t h = 0; h < GI_PROBE_LIGHT_HITS; ++h)
        {
            const auto& hit = probe.hits[i * GI_PROBE_LIGHT_HITS + h];
            if (!hit.x) break;

            text += std::format("  {:.2f} m short of the light: {}\n", entry.z - std::bit_cast<float>(hit.w), contents.describeHit(instanced, hit));
        }

        key += std::format("{}:{}:{}|", index, outcome, probe.hits[i * GI_PROBE_LIGHT_HITS].x);
    }

    return {text, key};
}

class LampShadowRays final : public trace::Trace
{
public:
    bool runsIn(const KickTraces& traces) const override { return traces.tracesLampShadows; }

    void gather(const trace::Kick& kick) override
    {
        output.clearWritten();
        gathered = runsIn(kick.traces) ? output.ensure(kick.device, kick.width, kick.height) : nullptr;
    }

    void prepare(const TraceFrame& frame) override
    {
        readProbe(frame);
        if (isProbedIn(frame.frame)) makeProbe(frame.device);
        else
        {
            std::lock_guard lock(probeMutex);
            probeText.clear();
        }

        if (!runsIn(frame.frame.traces) || (memoryWidth == frame.frame.width && memoryHeight == frame.frame.height)) return;

        frame.waitForAll();
        const auto texture = [&](const char* what) {
            return gi_d3d12::createTexture(frame.device, frame.frame.width, frame.frame.height, DXGI_FORMAT_R16G16_FLOAT, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, what);
        };
        rays = texture("Creating the lights' shadow rays");
        firstFilter = texture("Creating the lights' shadow filter");
        memoryWidth = frame.frame.width;
        memoryHeight = frame.frame.height;
    }

    void bind(const TraceFrame& frame, trace::TraceBindings& bindings) override
    {
        bindings.shared.share(ShareRow, gathered, opened, &output);
        bindings.descriptors.outputTexture(binding::LocalShadowRays, rays.Get(), DXGI_FORMAT_R16G16_FLOAT);
        bindings.descriptors.outputTexture(binding::LocalShadowFirst, firstFilter.Get(), DXGI_FORMAT_R16G16_FLOAT);
        if (isProbedIn(frame.frame)) bindings.descriptors.outputBuffer(binding::LightProbe, probe.Get(), sizeof(GILightProbe), 1);
    }

    void record(const TraceFrame& frame, gi_d3d12::Recorder& recorder) override
    {
        const auto columns = groups(frame.frame.width, 8), rows = groups(frame.frame.height, 8);
        recorder.mark("shadow rays for lights", performance::Pass::LightShadowRays);
        recorder.dispatch(Kernel::TraceLocalLightShadows, columns, rows);
        recorder.barrier();
        // For Ray Reconstruction, which denoises itself, the rays wrote the output.
        if (!frame.frame.traces.isForReconstruction)
        {
            recorder.mark("shadow filter for lights", performance::Pass::LightShadowFilter);
            for (std::uint32_t pass = 0; pass < 3; ++pass)
            {
                recorder.dispatch(Kernel::FilterLocalLightShadows, columns, rows, {pass, 1u << pass});
                recorder.barrier();
            }
        }

        if (!frame.frame.probeLights) return;

        recorder.dispatch(Kernel::ProbeLocalLights, 1, 1);
        recorder.barrier();
        recorder.transition(probe.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        recorder.commands().CopyBufferRegion(probeReadback[frame.slot].Get(), 0, probe.Get(), 0, sizeof(GILightProbe));
        recorder.transition(probe.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        isProbeWritten[frame.slot] = true;
        probedLights[frame.slot] = frame.frame.localLights;
    }

    void release() noexcept override
    {
        opened = {};
        rays.Reset();
        firstFilter.Reset();
        memoryWidth = memoryHeight = 0;
        probe.Reset();
        probeReadback = {};
        isProbeWritten = {};
        probedLights = {};
    }

    void detach() noexcept override
    {
        gathered = nullptr;
        output.reset();
    }

    ID3D11ShaderResourceView* writtenShare(UINT width, UINT height) const noexcept { return output.writtenView(width, height); }

    std::string probeForOverlay()
    {
        std::lock_guard lock(probeMutex);

        return probeText;
    }

private:
    bool isProbedIn(const Frame& frame) const { return runsIn(frame.traces) && frame.probeLights; }

    void makeProbe(ID3D12Device* device)
    {
        if (probe) return;

        probe = gi_d3d12::createStorage(device, sizeof(GILightProbe), "Creating the lamp probe");
        for (auto& readback : probeReadback)
            readback = gi_d3d12::createBuffer(device, sizeof(GILightProbe), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
                "Creating the lamp probe's readback");
    }

    // The probe the slot's last frame wrote, once the GPU is past it: into the overlay's text, and the log when
    // what blocks its rays changed (at most five times a second).
    void readProbe(const TraceFrame& frame)
    {
        if (!isProbeWritten[frame.slot]) return;

        isProbeWritten[frame.slot] = false;
        void* data{};
        if (FAILED(probeReadback[frame.slot]->Map(0, nullptr, &data))) return;

        GILightProbe read{};
        std::memcpy(&read, data, sizeof(read));
        const D3D12_RANGE none{};
        probeReadback[frame.slot]->Unmap(0, &none);

        auto [text, key] = describeProbe(frame.contents, frame.instanced, probedLights[frame.slot], read);
        const auto now = GetTickCount64();
        std::lock_guard lock(probeMutex);
        probeText = text;
        if constexpr (DiagnosticLog)
        {
            if (key != lastProbeKey && now - lastProbeLog >= 200)
            {
                lastProbeKey = key;
                lastProbeLog = now;
                log("Lamp probe:\n" + text);
            }
        }
    }

    SharedOutput output{DXGI_FORMAT_R16_FLOAT, "Creating the lights' shadow"};
    ID3D11Texture2D* gathered{};
    OpenedTexture opened;

    // Before and between the filter's passes (TraceLocalLightShadows, FilterLocalLightShadows), at the render size.
    ComPtr<ID3D12Resource> rays, firstFilter;
    UINT memoryWidth{}, memoryHeight{};

    // Diagnostics: the probe, read back when the frame's slot comes round again, with each slot's frame's lights, which
    // it lists by index.
    ComPtr<ID3D12Resource> probe;
    std::array<ComPtr<ID3D12Resource>, FramesInFlight> probeReadback;
    std::array<bool, FramesInFlight> isProbeWritten{};
    std::array<std::vector<LocalLight>, FramesInFlight> probedLights;

    // For the overlay, on another thread, and the log.
    std::mutex probeMutex;
    std::string probeText, lastProbeKey;
    std::uint64_t lastProbeLog{};
};

LampShadowRays lampShadowRays;
}

trace::Trace& trace() noexcept
{
    return lampShadowRays;
}

ID3D11ShaderResourceView* share(UINT width, UINT height) noexcept
{
    return lampShadowRays.writtenShare(width, height);
}

std::string probe()
{
    return lampShadowRays.probeForOverlay();
}
}
