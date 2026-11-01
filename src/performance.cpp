#include "core.hpp"
#include "features/upscaling/upscaler.hpp"
#include "performance.hpp"
#include "clock.hpp"
#include "command_list_notes.hpp"
#include "dev/perf_record.hpp"
#include "dev_tools.hpp"
#include "log.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <utility>

namespace phantom_fx::performance
{
namespace
{
using Microsoft::WRL::ComPtr;

// A frame on the immediate context: the disjoint query around it, timestamps at its first command
// list and at its present, and the passes its command lists ran; with the passes Direct3D 12 timed.
struct Frame
{
    ComPtr<ID3D11Query> disjoint, start, end;
    std::vector<TimedRun> runs;
    Milliseconds direct3D12{};
    bool started{}; // A command list executed, so `start` was ended
    bool pending{}; // Closed and not read yet
};

// What a measurement step leaves out, with the steps before it.
struct Setting
{
    std::string removed;
    LeftOut leftOut;
};

// The runs a deferred context recorded since its last FinishCommandList, then those of its command list.
struct RecordedRuns
{
    std::vector<TimedRun> runs;

    void merge(RecordedRuns later)
    {
        runs.insert(runs.end(), std::make_move_iterator(later.runs.begin()), std::make_move_iterator(later.runs.end()));
    }
};

// Frames the GPU may still be working on, and the one being recorded.
constexpr std::size_t FrameCount = 5;
constexpr float AverageMilliseconds = 500;
constexpr float SettleSeconds = 1.5f, TimedSeconds = 2.5f;

// ---- The timer: taken from any thread, and nothing else is locked while it is held ----
std::mutex timerMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> immediate;
std::array<Frame, FrameCount> frames;
std::size_t current{};
bool open{}; // frames[current] takes the runs of the command lists executed now
std::vector<TimedRun> spare;

CommandListNotes<RecordedRuns> recordedRuns;
// Diagnostics: contexts' or lists' runs never handed over (CommandListNotes::present) since the last log.
std::uint64_t expiredRuns{}, expiredLoggedAt{};

// Read on every recording thread.
std::atomic<bool> timing{};
std::atomic<std::uint64_t> watchedAt{};
std::atomic<bool> overlay{}, measurementRequested{};

// ---- On the presenting thread ----
std::filesystem::path reportPath;
Costs costs;
bool averaging{}; // `costs` has a GPU sample since the timing began
LONGLONG lastPresent{}, lastSample{};

struct
{
    std::vector<Setting> steps;
    std::size_t step{};
    LONGLONG stepStart{}, timingStart{};
    double gpuSum{};
    unsigned gpuFrames{};
    std::vector<Measurement::Step> results, finished;
    std::uint64_t finishedAt{};
    bool running{};
} measurement;

std::mutex statusMutex;
Status published;

// Call with timerMutex held. Null when the device refuses.
ComPtr<ID3D11Query> query(D3D11_QUERY type)
{
    ComPtr<ID3D11Query> result;
    const D3D11_QUERY_DESC description{type, 0};
    if (device && FAILED(device->CreateQuery(&description, result.GetAddressOf()))) result.Reset();

    return result;
}

// Call with timerMutex held.
void giveBack(std::vector<TimedRun>& runs)
{
    spare.insert(spare.end(), std::make_move_iterator(runs.begin()), std::make_move_iterator(runs.end()));
    runs.clear();
}

std::optional<TimedRun> acquire(Pass pass)
{
    if (!timing.load(std::memory_order_relaxed)) return std::nullopt;

    std::lock_guard lock(timerMutex);
    if (!device) return std::nullopt;

    TimedRun run;
    if (!spare.empty())
    {
        run = std::move(spare.back());
        spare.pop_back();
    }
    else run = {pass, query(D3D11_QUERY_TIMESTAMP), query(D3D11_QUERY_TIMESTAMP)};

    if (!run.start || !run.end) return std::nullopt;

    run.pass = pass;

    return run;
}

// A run the immediate context recorded counts for the frame now executing; a deferred context's
// waits for its command list.
void finish(ID3D11DeviceContext* context, TimedRun run)
{
    {
        std::lock_guard lock(timerMutex);
        if (context == immediate.Get())
        {
            if (open) frames[current].runs.push_back(std::move(run));
            else spare.push_back(std::move(run));

            return;
        }
    }

    recordedRuns.note(reinterpret_cast<std::uint64_t>(context), [&](RecordedRuns& contextRuns) { contextRuns.runs.push_back(std::move(run)); });
}

// Call with timerMutex held.
std::optional<float> millisecondsBetween(ID3D11Query* start, ID3D11Query* end, UINT64 frequency)
{
    UINT64 from{}, to{};
    if (immediate->GetData(start, &from, sizeof(from), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
        || immediate->GetData(end, &to, sizeof(to), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || to < from)
        return std::nullopt;

    return static_cast<float>(static_cast<double>(to - from) * 1000.0 / static_cast<double>(frequency));
}

// Call with timerMutex held. Nullopt while the GPU has not finished the frame, or when its clock
// changed speed during it (a power state change), which makes its times meaningless.
std::optional<FrameCost> read(Frame& frame)
{
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    if (immediate->GetData(frame.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return std::nullopt;

    frame.pending = false;
    if (disjoint.Disjoint || !disjoint.Frequency)
    {
        giveBack(frame.runs);
        return std::nullopt;
    }

    FrameCost sample{.passes = frame.direct3D12};
    if (frame.started) sample.gpuFrame = millisecondsBetween(frame.start.Get(), frame.end.Get(), disjoint.Frequency).value_or(0.0f);
    for (const auto& run : frame.runs)
        if (const auto time = millisecondsBetween(run.start.Get(), run.end.Get(), disjoint.Frequency))
            sample.passes[static_cast<std::size_t>(run.pass)] += *time;

    giveBack(frame.runs);

    return sample;
}

// Closes the frame being recorded, then returns the newest frame the GPU has finished since the last
// call, if any. Oldest first; the frame about to be recorded into again is given up if the GPU has
// not finished it.
std::optional<FrameCost> closeAndRead()
{
    std::lock_guard lock(timerMutex);
    if (!immediate) return std::nullopt;

    if (std::exchange(open, false))
    {
        auto& frame = frames[current];
        if (frame.started) immediate->End(frame.end.Get());
        immediate->End(frame.disjoint.Get());
        frame.pending = true;
        current = (current + 1) % FrameCount;
    }

    std::optional<FrameCost> newest;
    for (std::size_t i = 0; i < FrameCount; ++i)
    {
        auto& candidate = frames[(current + i) % FrameCount];
        if (!candidate.pending) continue;

        if (auto sample = read(candidate)) newest = sample;
        else if (candidate.pending && &candidate == &frames[current])
        {
            candidate.pending = false;
            giveBack(candidate.runs);
        }
    }

    return newest;
}

// Starts timing the next frame, or stops when no one looks.
void openFrame(bool wanted)
{
    std::lock_guard lock(timerMutex);
    auto& frame = frames[current];
    if (!wanted || !immediate || frame.pending)
    {
        timing = false;
        return;
    }

    if (!frame.disjoint)
    {
        frame.disjoint = query(D3D11_QUERY_TIMESTAMP_DISJOINT);
        frame.start = query(D3D11_QUERY_TIMESTAMP);
        frame.end = query(D3D11_QUERY_TIMESTAMP);
    }

    if (!frame.disjoint || !frame.start || !frame.end)
    {
        timing = false;
        return;
    }

    immediate->Begin(frame.disjoint.Get());
    frame.direct3D12 = {};
    frame.started = false;
    open = true;
    timing = true;
}

// Adds the frame to the averages, which follow about the last half second.
void average(LONGLONG now, const std::optional<FrameCost>& sample)
{
    if (lastPresent)
    {
        const float frame = static_cast<float>(clock::millisecondsOf(now - lastPresent));
        costs.frame += (frame - costs.frame) * (1 - std::exp(-frame / AverageMilliseconds));
    }

    lastPresent = now;
    if (!sample) return;

    // The first sample after the timing began replaces whatever was left from before.
    const float weight = averaging ? 1 - std::exp(-static_cast<float>(clock::millisecondsOf(now - lastSample)) / AverageMilliseconds) : 1;
    averaging = true;
    lastSample = now;
    costs.gpuFrame += (sample->gpuFrame - costs.gpuFrame) * weight;
    for (std::size_t i = 0; i < costs.passes.size(); ++i) costs.passes[i] += (sample->passes[i] - costs.passes[i]) * weight;
}

// The steps from what is on now: each leaves out one more effect, the lighting effects in Effect's order
// (effects.hpp), the upscaler last, as the others run at its render size. HDR takes a restart, so it is
// not measured. Returns what the first step leaves out: nothing.
std::optional<LeftOut> startMeasurement(LONGLONG now)
{
    auto& m = measurement;
    m.steps.clear();
    m.results.clear();
    m.step = 0;
    m.timingStart = 0;

    const auto settings = core::currentSettings();
    const auto plan = core::effectPlan(settings);
    LeftOut leftOut;
    m.steps.push_back({"", leftOut});
    const auto leave = [&](bool& flag, std::string removed)
    {
        flag = true;
        m.steps.push_back({std::move(removed), leftOut});
    };
    // The global illumination in parts: its light passes, then the rest (the scene's build, the wait
    // for Direct3D 12 and its passes in the game's frame).
    for (const auto effect : AllEffects)
    {
        if (!plan.runs(effect)) continue;

        const bool global = effect == Effect::GlobalIllumination;
        if (global) leave(leftOut.globalLight, "Global illumination's light passes");
        leave(leftOut.effects[effect], global ? "Global illumination (the rest)" : std::string(featureOf(effect).name));
    }

    const bool upscaling = settings.upscaling.mode != upscaler::Mode::Off;
    if (upscaling && settings.upscaling.sharpness > 0) leave(leftOut.sharpening, "Sharpening");
    if (upscaling) leave(leftOut.upscaler, std::string(upscaler::label(settings.upscaling.upscaler)) + " (FXAA instead)");

    if (m.steps.size() < 2)
    {
        log("Nothing to measure: no effect is on.");
        return std::nullopt;
    }

    m.running = true;
    m.stepStart = now;
    log("Measuring the real cost: " + std::to_string(m.steps.size()) + " steps.");

    return m.steps.front().leftOut;
}

void finishMeasurement()
{
    auto& m = measurement;
    m.running = false;
    m.finished = m.results;
    m.finishedAt = GetTickCount64();

    auto line = std::format("Real cost (GPU frame without each, ms): everything on {:.2f}", m.results.front().gpuFrame);
    for (std::size_t i = 1; i < m.results.size(); ++i)
        line += std::format("; {} {:.2f} ({:+.2f})", m.results[i].removed, m.results[i].gpuFrame, m.results[i - 1].gpuFrame - m.results[i].gpuFrame);

    log(line + std::format("; total {:.2f}.", m.results.front().gpuFrame - m.results.back().gpuFrame));
}

// One present into the measurement: each step settles, then its GPU frames are timed. Returns what
// to leave out from now on, when that changes.
std::optional<LeftOut> advanceMeasurement(LONGLONG now, const std::optional<FrameCost>& sample)
{
    auto& m = measurement;
    if (measurementRequested.exchange(false) && !m.running) return startMeasurement(now);
    if (!m.running || clock::millisecondsOf(now - m.stepStart) < SettleSeconds * 1000) return std::nullopt;

    if (!m.timingStart)
    {
        m.timingStart = now;
        m.gpuSum = 0;
        m.gpuFrames = 0;
        return std::nullopt;
    }

    if (sample && sample->gpuFrame > 0)
    {
        m.gpuSum += sample->gpuFrame;
        ++m.gpuFrames;
    }

    if (clock::millisecondsOf(now - m.timingStart) < TimedSeconds * 1000) return std::nullopt;

    m.results.push_back({m.steps[m.step].removed, m.gpuFrames ? static_cast<float>(m.gpuSum / m.gpuFrames) : 0.0f});
    m.timingStart = 0;
    m.stepStart = now;
    if (++m.step < m.steps.size()) return m.steps[m.step].leftOut;

    finishMeasurement();

    return LeftOut{};
}

void publish()
{
    std::lock_guard lock(statusMutex);
    published.overlay = overlay;
    published.costs = costs;
    auto& m = published.measurement;
    m.running = measurement.running;
    m.step = static_cast<unsigned>(measurement.step);
    m.stepCount = static_cast<unsigned>(measurement.steps.size());
    if (m.finishedAt == measurement.finishedAt) return;

    m.steps = measurement.finished;
    m.finishedAt = measurement.finishedAt;
}

// While the costs show over the game or are measured, PhantomFX-performance.txt holds them, rewritten
// every second, and once more after, so it keeps a finished measurement's result. Not otherwise:
// opening a file while presenting can hitch the game.
void writeReport()
{
    static std::uint64_t written{};
    static bool asked{};
    const auto now = GetTickCount64();
    if (now - written < 1000) return;

    written = now;
    const auto askedBefore = std::exchange(asked, overlay || measurement.running);
    if ((!asked && !askedBefore) || reportPath.empty()) return;

    std::ofstream(reportPath, std::ios::trunc) << report(status());
}
}

void attach(ID3D11Device* gameDevice, ID3D11DeviceContext* gameImmediate, const std::filesystem::path& report) noexcept
{
    std::lock_guard lock(timerMutex);
    device = gameDevice;
    immediate = gameImmediate;
    reportPath = report;
}

void detach() noexcept
{
    timing = false;
    recordedRuns.clear();
    if (measurement.running)
    {
        measurement.running = false;
        core::leaveOut({});
    }

    std::lock_guard lock(timerMutex);
    frames = {};
    current = 0;
    open = false;
    spare.clear();
    immediate.Reset();
    device.Reset();
    averaging = false;
}

Timed::Timed(ID3D11DeviceContext* context, Pass pass) noexcept : context(context)
{
    next(pass);
}

void Timed::next(Pass pass) noexcept
{
    end();
    try
    {
        run = acquire(pass);
    }
    catch (...)
    {
        run.reset();
    }

    if (run) context->End(run->start.Get());
}

void Timed::end() noexcept
{
    if (!run) return;

    context->End(run->end.Get());
    try
    {
        finish(context, std::move(*run));
    }
    catch (...)
    {
    }

    run.reset();
}

void recorded(std::uint64_t into, std::uint64_t from) noexcept
{
    recordedRuns.recorded(into, from);
}

void forget(std::uint64_t contextOrList) noexcept
{
    // A list gone before it ran gives its queries back.
    auto listRuns = recordedRuns.take(contextOrList);
    recordedRuns.forget(contextOrList);
    if (!listRuns) return;

    try
    {
        std::lock_guard lock(timerMutex);
        giveBack(listRuns->runs);
    }
    catch (...)
    {
    }
}

void timedOnDirect3D12(const Milliseconds& passes) noexcept
{
    if (!timing.load(std::memory_order_relaxed)) return;

    try
    {
        std::lock_guard lock(timerMutex);
        if (!open) return;

        auto& frame = frames[current].direct3D12;
        for (std::size_t i = 0; i < passes.size(); ++i) frame[i] += passes[i];
    }
    catch (...)
    {
    }
}

void addPhase(PhaseTimes& phases, const char* name, double milliseconds)
{
    const auto found = std::ranges::find_if(phases, [&](const auto& phase) { return std::strcmp(phase.first, name) == 0; });
    if (found != phases.end()) found->second += milliseconds;
    else phases.emplace_back(name, milliseconds);
}

void executing(std::uint64_t list) noexcept
{
    try
    {
        auto listRuns = recordedRuns.take(list);

        std::lock_guard lock(timerMutex);
        if (open && !frames[current].started)
        {
            immediate->End(frames[current].start.Get());
            frames[current].started = true;
        }

        if (!listRuns) return;

        auto& runs = frames[current].runs;
        if (open) runs.insert(runs.end(), std::make_move_iterator(listRuns->runs.begin()), std::make_move_iterator(listRuns->runs.end()));
        else giveBack(listRuns->runs);
    }
    catch (...)
    {
    }
}

void present() noexcept
{
    expiredRuns += recordedRuns.present();
    if constexpr (DiagnosticLog)
    {
        const auto now = GetTickCount64();
        if (expiredRuns && now - expiredLoggedAt >= 10'000)
        {
            log("Costs: " + std::to_string(expiredRuns) + " contexts' or lists' timed runs expired.");
            expiredRuns = 0;
            expiredLoggedAt = now;
        }
    }

    try
    {
        const auto now = clock::ticksNow();
        const auto sample = closeAndRead();
        average(now, sample);
        if constexpr (DevTools) perf_record::onPresent(sample);
        // Not under a lock: the effects' settings take theirs.
        if (const auto leftOut = advanceMeasurement(now, sample)) core::leaveOut(*leftOut);

        const bool wanted = overlay || measurement.running || GetTickCount64() - watchedAt.load() < 1000;
        if (!wanted) averaging = false;
        openFrame(wanted);
        publish();
        writeReport();
    }
    catch (const std::exception& error)
    {
        log(std::string("Costs: ") + error.what());
    }
}

Status status()
{
    std::lock_guard lock(statusMutex);
    return published;
}

void watch() noexcept
{
    watchedAt = GetTickCount64();
}

void showOverlay(bool show) noexcept
{
    overlay = show;
}

void measure() noexcept
{
    measurementRequested = true;
}
}
