#include "dev/inspect_trace.hpp"

#include "dev/inspect_shared.h"
#include "log.hpp"
#include "passes/ray_tracing/trace.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace phantom_fx::inspect_trace
{
namespace
{
using namespace phantom_fx::gi;
using gi_d3d12::ComPtr;
using gi_d3d12::groups;
using gi_d3d12::Kernel;
using kick_traces::KickTraces;
using trace::TraceFrame;

// What a record starts with, for tools/automation/case.py.
struct Header
{
    char magic[4]{'F', 'X', 'I', '1'};
    std::uint32_t width{}, height{}, frame{};
    float viewProjectionInverse[16]{};
    float eye[4]{};
    float sunDirection[4]{};
};

class InspectTrace final : public trace::Trace
{
public:
    bool runsIn(const KickTraces&) const override { return isRecording; }

    bool listsInstances(const gi_renderer::Frame&) const override { return isRecording; }

    void gather(const trace::Kick&) override {}

    void prepare(const TraceFrame& frame) override
    {
        readRecord(frame);
        isRecording = false;
        // One at a time: a frame's record is written as its slot comes round.
        if (recordedSlot) return;

        std::lock_guard lock(requestMutex);
        if (requested.empty()) return;

        file = std::exchange(requested, {});
        const std::uint64_t bytes = std::uint64_t{frame.frame.width} * frame.frame.height * sizeof(GIInspectPixel);
        pixels = gi_d3d12::createStorage(frame.device, bytes, "Creating a debug case's record");
        readback = gi_d3d12::createBuffer(frame.device, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
            "Creating a debug case's readback");
        isRecording = true;
    }

    void bind(const TraceFrame& frame, trace::TraceBindings& bindings) override
    {
        bindings.descriptors.outputBuffer(binding::Inspect, pixels.Get(), sizeof(GIInspectPixel), std::uint64_t{frame.frame.width} * frame.frame.height);
        constants = bindings.constants;
    }

    void record(const TraceFrame& frame, gi_d3d12::Recorder& recorder) override
    {
        const auto width = frame.frame.width, height = frame.frame.height;
        recorder.mark("a debug case's record");
        recorder.dispatch(Kernel::InspectPixels, groups(width, 8), groups(height, 8));
        recorder.barrier();
        recorder.transition(pixels.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        recorder.commands().CopyBufferRegion(readback.Get(), 0, pixels.Get(), 0, std::uint64_t{width} * height * sizeof(GIInspectPixel));
        recordedSlot = frame.slot;
        size = {width, height, frame.frame.frameIndex};
    }

    void release() noexcept override
    {
        pixels.Reset();
        readback.Reset();
        recordedSlot.reset();
        isRecording = false;
    }

    void detach() noexcept override {}

    void ask(const std::filesystem::path& to)
    {
        std::lock_guard lock(requestMutex);
        requested = to;
    }

    std::uint64_t finishedRecords() const noexcept { return finishedCount; }

private:
    // The record the slot's last frame wrote, once the GPU is past it: every pixel, then a line for each mesh a
    // hit names, as the probes name them.
    void readRecord(const TraceFrame& frame)
    {
        if (recordedSlot != frame.slot) return;

        recordedSlot.reset();
        const auto [width, height, frameIndex] = size;
        const std::uint64_t bytes = std::uint64_t{width} * height * sizeof(GIInspectPixel);
        void* data{};
        const D3D12_RANGE whole{0, static_cast<SIZE_T>(bytes)};
        if (SUCCEEDED(readback->Map(0, &whole, &data)))
        {
            Header header;
            header.width = width;
            header.height = height;
            header.frame = frameIndex;
            static_assert(sizeof(header.viewProjectionInverse) == sizeof(constants.viewProjectionInverse));
            std::memcpy(header.viewProjectionInverse, &constants.viewProjectionInverse, sizeof(header.viewProjectionInverse));
            std::memcpy(header.eye, &constants.eye, sizeof(header.eye));
            std::memcpy(header.sunDirection, &constants.sunDirection, sizeof(header.sunDirection));

            std::ofstream record(file.replace_extension(".bin"), std::ios::binary | std::ios::trunc);
            record.write(reinterpret_cast<const char*>(&header), sizeof(header));
            record.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
            record.close();
            if (!record) log("Writing " + file.string() + " failed.");

            std::map<std::pair<std::uint32_t, std::uint32_t>, std::string> names;
            const auto note = [&](std::uint32_t hit, std::uint32_t kind) {
                if (!hit || names.contains({hit, kind})) return;

                const auto instance = (hit & ((1u << GI_HIT_INSTANCE_BITS) - 1)) - 1;
                names[{hit, kind}] = frame.contents.describeMesh(frame.instanced, instance, hit >> GI_HIT_INSTANCE_BITS, kind);
            };

            const std::span records(static_cast<const GIInspectPixel*>(data), std::size_t{width} * height);
            for (const auto& pixel : records)
            {
                note(pixel.viewHit, pixel.kinds & GI_INSTANCE_KINDS);
                note(pixel.surfaceHit, (pixel.kinds >> 8) & GI_INSTANCE_KINDS);
                note(pixel.sunHit, (pixel.kinds >> 16) & GI_INSTANCE_KINDS);
            }

            const D3D12_RANGE nothing{};
            readback->Unmap(0, &nothing);

            std::ofstream table(file.replace_extension(".tsv"), std::ios::trunc);
            for (const auto& [key, name] : names) table << key.first << '\t' << key.second << '\t' << name << '\n';
            table.close();
            if (!table) log("Writing " + file.string() + " failed.");
        }
        else log("A debug case's record could not be read back.");

        pixels.Reset();
        readback.Reset();
        finishedCount.fetch_add(1);
    }

    // Asked on the thread that runs the commands.
    std::mutex requestMutex;
    std::filesystem::path requested;

    // The frame's record, made for it and let go once written: the slot of the frame it is in, and where it goes.
    bool isRecording{};
    ComPtr<ID3D12Resource> pixels, readback;
    std::optional<std::uint32_t> recordedSlot;
    std::filesystem::path file;
    std::array<std::uint32_t, 3> size{}; // Width, height, the frame's index
    GIFrameConstants constants{};

    std::atomic<std::uint64_t> finishedCount{};
};

InspectTrace inspectTrace;
}

trace::Trace& trace() noexcept
{
    return inspectTrace;
}

void request(const std::filesystem::path& file)
{
    inspectTrace.ask(file);
}

std::uint64_t finished() noexcept
{
    return inspectTrace.finishedRecords();
}
}
