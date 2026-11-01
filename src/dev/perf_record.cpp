#include "dev/perf_record.hpp"

#include "clock.hpp"
#include "core.hpp"
#include "dev/automation.hpp"
#include "dev/game_lua.hpp"
#include "dev/held_camera.hpp"
#include "log.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"
#include "weather.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <format>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <utility>
#include <string_view>
#include <vector>

namespace phantom_fx::perf_record
{
namespace
{
using Json = nlohmann::ordered_json;
using performance::Pass;

constexpr LONGLONG RowMilliseconds = 1000;
constexpr std::uint64_t PlaceMilliseconds = 1000;
// Its being there is the switch: a setting of its own would put a tool's line in every player's PhantomFX.ini.
constexpr std::string_view RecordingMarker = "recording";

// In performance::Pass's order: a pass added there fails to compile here until it is named.
constexpr std::array<const char*, static_cast<std::size_t>(Pass::Count)> PassNames{
    "UpscalerInputs", "Upscaler", "UpscalerOutput",
    "SunTrace", "SunLight", "LampTraces", "LampShadows", "LampSoftLight",
    "OcclusionPrefilter", "OcclusionMarch", "OcclusionDenoise", "OcclusionApply",
    "SsgiGather", "SsgiAccumulate", "SsgiFilter", "SsgiResolve",
    "FogPass", "SkyTables", "FogShadow", "FogScattering", "FogIntegration", "Sky",
    "SeaWaves", "SeaSurface", "WaterCaustics",
    "GiInputs", "GiProbeLight", "GiCombine", "GiWait", "LocalLights", "PathTracedLight",
    "RayTracedScene", "SunShadowRays", "SunShadowDenoiser", "LightShadowRays", "LightShadowFilter", "GiRays", "GiDenoiser", "Reflections", "LampLight",
    "HdrEncode", "HdrDecode", "HdrGrade"};
static_assert(PassNames.back() != nullptr, "A pass has no name");

constexpr std::pair<std::uint32_t, const char*> CounterNames[]{{GI_COUNTER_HITS, "hits"}, {GI_COUNTER_NEAR_HITS, "nearHits"}, {GI_COUNTER_BOUNCE_HITS, "bounceHits"},
    {GI_COUNTER_VISIBILITY0, "intoCache"}, {GI_COUNTER_VISIBILITY1, "multibounce"}, {GI_COUNTER_MULTIBOUNCE_AWAY, "multibounceAway"}, {GI_COUNTER_LIGHTS, "cacheHitsLit"},
    {GI_COUNTER_COPIES, "copiesSkipped"}, {GI_COUNTER_UPDATE_TILES, "cacheTiles"}, {GI_COUNTER_FOLIAGE, "foliagePixels"}};

// What frames cost, added up.
struct Tally
{
    std::vector<float> frameTimes, gpuFrames;
    performance::Milliseconds passes{};
    performance::PhaseTimes phases;
    std::uint32_t phaseFrames{};
    std::array<std::uint64_t, GI_COUNTER_COUNT> counters{};
    std::uint32_t counterFrames{};
    std::uint64_t tracedLights{}, lightsOverCap{};
    std::uint32_t lightFrames{};
    Kick lastKick;
    std::uint64_t builds{}, builtTriangles{}; // Of all its kicks: most build nothing
};

struct Capture
{
    std::filesystem::path file;
    std::uint32_t frames{};
    Tally tally;
};

// Where the game is, from the game thread.
struct Place
{
    std::string mission = "?", location = "?";
    float clock{};
    int weather = -1;
    bool isFrozen{};
    std::optional<held_camera::Pose> camera;
};

std::filesystem::path folder; // Empty while off
std::atomic<bool> isRecordingOn{};

std::mutex recordMutex;
Tally row;
std::optional<Capture> running;
Place place;
LONGLONG rowStart{}, lastPresent{}, firstPresent{};

// From the presenting thread alone.
std::ofstream file;
std::optional<Settings> writtenSettings;

std::atomic<std::uint64_t> finished{};
std::mutex lastMutex;
std::string lastLine;

// Call with recordMutex held. Each tally that costs go into now.
template<class Add> void addToTallies(Add&& add)
{
    if (isRecordingOn) add(row);
    if (running) add(running->tally);
}

// The game's threads call in here, and nothing may get back to them.
template<class Work> void guarded(Work&& work) noexcept
{
    try
    {
        work();
    }
    catch (const std::exception& error)
    {
        log(std::string("Costs on file: ") + error.what());
    }
}

double rounded(double value)
{
    return std::round(value * 1000.0) / 1000.0;
}

// The value `share` of the way up `values`, which it sorts.
float percentile(std::vector<float>& values, float share)
{
    if (values.empty()) return 0.0f;

    const auto nth = values.begin() + static_cast<std::ptrdiff_t>(share * static_cast<float>(values.size() - 1));
    std::nth_element(values.begin(), nth, values.end());

    return *nth;
}

Json spread(std::vector<float>& values)
{
    Json result = Json::object();
    if (values.empty()) return result;

    double sum{};
    for (const auto value : values) sum += value;
    result["mean"] = rounded(sum / static_cast<double>(values.size()));
    result["p50"] = rounded(percentile(values, 0.5f));
    result["p95"] = rounded(percentile(values, 0.95f));
    result["p99"] = rounded(percentile(values, 0.99f));
    result["max"] = rounded(*std::ranges::max_element(values));

    return result;
}

Json placeJson(const Place& where)
{
    Json result = Json::object();
    result["mission"] = where.mission;
    result["location"] = where.location;
    result["clock"] = rounded(where.clock);
    result["weather"] = where.weather;
    result["frozen"] = where.isFrozen;

    if (where.camera)
        result["camera"] = {rounded(where.camera->position[0]), rounded(where.camera->position[1]), rounded(where.camera->position[2]), rounded(where.camera->yaw),
            rounded(where.camera->pitch)};

    return result;
}

// Sorts the tally's times: a tally is written once.
Json toJson(Tally& tally, const Place& where)
{
    Json result = placeJson(where);
    result["frames"] = tally.frameTimes.size();
    result["render"] = {tally.lastKick.width, tally.lastKick.height};
    result["frame"] = spread(tally.frameTimes);
    result["gpu"] = spread(tally.gpuFrames);

    const auto gpuFrames = std::max<std::size_t>(tally.gpuFrames.size(), 1);
    Json passes = Json::object();
    for (std::size_t i = 0; i < tally.passes.size(); ++i)
        if (tally.passes[i] > 0.0f) passes[PassNames[i]] = rounded(tally.passes[i] / static_cast<double>(gpuFrames));

    result["passes"] = std::move(passes);

    Json phases = Json::object();
    const double phaseFrames = std::max(tally.phaseFrames, 1u);
    for (const auto& [name, milliseconds] : tally.phases) phases[name] = rounded(milliseconds / phaseFrames);
    result["phases"] = std::move(phases);

    const double lightFrames = std::max(tally.lightFrames, 1u);
    result["lights"] = {{"traced", rounded(static_cast<double>(tally.tracedLights) / lightFrames)}, {"overCap", rounded(static_cast<double>(tally.lightsOverCap) / lightFrames)}};
    result["scene"] = {{"instances", tally.lastKick.instances}, {"triangles", tally.lastKick.triangles}, {"skins", tally.lastKick.skins}, {"builds", tally.builds},
        {"builtTriangles", tally.builtTriangles}};

    Json counters = Json::object();
    const auto counterFrames = std::max<std::uint64_t>(tally.counterFrames, 1);
    for (const auto& [counter, name] : CounterNames) counters[name] = tally.counters[counter] / counterFrames;
    result["counters"] = std::move(counters);

    return result;
}

std::string timeOfDay()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);

    return std::format("{:02}:{:02}:{:02}", now.wHour, now.wMinute, now.wSecond);
}

// The file is opened once: opening one while presenting can hitch the game.
bool writeLine(const Json& line)
{
    static bool wasFailing{};
    if (!file.is_open()) file.open(folder / (automation::timeNow() + ".jsonl"), std::ios::app);

    file << line.dump() << '\n';
    file.flush();
    if (file)
    {
        wasFailing = false;
        return true;
    }

    // Once for a stretch of failures, and the next row tries again.
    if (!std::exchange(wasFailing, true)) log("Costs on file: a row could not be written to " + folder.string() + ".");

    file.clear();

    return false;
}

// The settings in force, as PhantomFX.ini's lines, when they change: what a stretch of rows ran with. Not from
// that file, which is half a second behind a change.
void writeSettings()
{
    const auto settings = core::currentSettings();
    if (writtenSettings == settings) return;

    std::istringstream ini(iniText(settings));
    Json lines = Json::object();
    for (std::string line; std::getline(ini, line);)
        if (const auto separator = line.find('='); separator != line.npos) lines[line.substr(0, separator)] = line.substr(separator + 1);

    if (writeLine({{"time", timeOfDay()}, {"settings", std::move(lines)}})) writtenSettings = settings;
}

void finishRow(Tally&& finishedRow, const Place& where, double seconds)
{
    writeSettings();
    Json line = {{"time", timeOfDay()}, {"seconds", std::floor(seconds)}};
    line.update(toJson(finishedRow, where));
    writeLine(line);
}

// Whatever comes of it counts as finished, so a command waiting for it hears why.
void finishCapture(Capture&& capture, const Place& where)
{
    std::string outcome;
    try
    {
        auto json = toJson(capture.tally, where);
        const auto gpu = json["gpu"].value("mean", 0.0), frame = json["frame"].value("mean", 0.0);
        std::error_code error;
        std::filesystem::create_directories(capture.file.parent_path(), error);
        auto path = capture.file;
        path += ".json";
        std::ofstream output(path, std::ios::trunc);
        output << json.dump(1);
        output.close();
        outcome = output ? std::format("{} frames, frame {:.2f} ms, GPU {:.2f} ms", json["frames"].get<std::size_t>(), frame, gpu)
                         : "error: " + path.string() + " could not be written";
    }
    catch (const std::exception& error)
    {
        outcome = std::string("error: ") + error.what();
    }

    {
        std::lock_guard lock(lastMutex);
        lastLine = std::move(outcome);
    }

    ++finished;
}
}

void install(const std::filesystem::path& into) noexcept
{
    std::error_code error;
    std::filesystem::create_directories(into, error);
    if (error) return;

    folder = into;
    isRecordingOn = std::filesystem::exists(folder / RecordingMarker, error);
}

bool isRecording() noexcept
{
    return isRecordingOn;
}

void setRecording(bool isOn) noexcept
{
    if (folder.empty()) return;

    guarded([&] {
        // Left as it was if the marker cannot follow: the switch would come back otherwise at the next start.
        std::error_code error;
        const bool isKept = isOn ? static_cast<bool>(std::ofstream(folder / RecordingMarker)) : std::filesystem::remove(folder / RecordingMarker, error) || !error;
        if (!isKept)
        {
            log("Costs on file: the recording switch could not be kept in " + folder.string() + ".");
            return;
        }

        std::lock_guard lock(recordMutex);
        row = {};
        rowStart = 0;
        isRecordingOn = isOn;
    });
}

bool isKeepingCosts() noexcept
{
    std::lock_guard lock(recordMutex);
    return isRecordingOn || running;
}

void onFrameStart() noexcept
{
    if (folder.empty()) return;

    guarded([] {
        // Not while nothing keeps it: this runs Lua in the game's script state.
        static std::uint64_t readAt{};
        const auto now = GetTickCount64();
        if (now - readAt < PlaceMilliseconds || !isKeepingCosts()) return;

        readAt = now;
        Place read;
        if (const auto outcome = game_lua::run(game_lua::StateScript, "=perf"); outcome && outcome->ran)
        {
            std::istringstream lines(outcome->text);
            for (std::string line; std::getline(lines, line);)
            {
                const auto separator = line.find('=');
                const auto name = line.substr(0, separator);
                if (name == "mission") read.mission = line.substr(separator + 1);
                else if (name == "location") read.location = line.substr(separator + 1);
            }
        }

        const auto sky = weather::state();
        read.clock = sky.clock;
        read.weather = sky.requested ? static_cast<int>(*sky.requested) : -1;
        read.isFrozen = held_camera::isHeld();
        read.camera = held_camera::lastCamera();

        std::lock_guard lock(recordMutex);
        place = std::move(read);
    });
}

void onPresent(const std::optional<performance::FrameCost>& cost) noexcept
{
    if (folder.empty()) return;

    guarded([&] {
        const auto now = clock::ticksNow();
        std::optional<Tally> finishedRow;
        std::optional<Capture> finishedCapture;
        Place where;
        double seconds{};
        {
            std::lock_guard lock(recordMutex);
            const bool isOn = isRecordingOn;

            // Timed for as long as anything is kept, as while someone looks at the costs.
            if (isOn || running) performance::watch();

            addToTallies([&](Tally& tally) {
                if (lastPresent) tally.frameTimes.push_back(static_cast<float>(clock::millisecondsOf(now - lastPresent)));
                if (!cost || cost->gpuFrame <= 0.0f) return;

                tally.gpuFrames.push_back(cost->gpuFrame);
                for (std::size_t i = 0; i < tally.passes.size(); ++i) tally.passes[i] += cost->passes[i];
            });

            // By the GPU's frames: with no one looking, its timing starts with the capture and comes a few frames late.
            if (running && running->tally.gpuFrames.size() >= running->frames)
            {
                finishedCapture = std::move(running);
                running.reset();
            }

            lastPresent = now;
            if (!firstPresent) firstPresent = now;
            if (!rowStart) rowStart = now;
            if (isOn && clock::millisecondsOf(now - rowStart) >= RowMilliseconds)
            {
                finishedRow = std::move(row);
                row = {};
                rowStart = now;
                seconds = clock::millisecondsOf(now - firstPresent) / 1000.0;
            }

            where = place;
        }

        if (finishedCapture) finishCapture(std::move(*finishedCapture), where);
        if (finishedRow && !finishedRow->frameTimes.empty()) finishRow(std::move(*finishedRow), where, seconds);
    });
}

void onLights(std::size_t traced, std::size_t overCap) noexcept
{
    if (folder.empty()) return;

    std::lock_guard lock(recordMutex);
    addToTallies([&](Tally& tally) {
        tally.tracedLights += traced;
        tally.lightsOverCap += overCap;
        ++tally.lightFrames;
    });
}

void onKick(const Kick& kick) noexcept
{
    if (folder.empty()) return;

    std::lock_guard lock(recordMutex);
    addToTallies([&](Tally& tally) {
        tally.lastKick = kick;
        tally.builds += kick.builds;
        tally.builtTriangles += kick.builtTriangles;
    });
}

void onPhases(const performance::PhaseTimes& phases) noexcept
{
    if (folder.empty()) return;

    guarded([&] {
        std::lock_guard lock(recordMutex);
        addToTallies([&](Tally& tally) {
            ++tally.phaseFrames;
            for (const auto& [name, milliseconds] : phases) performance::addPhase(tally.phases, name, milliseconds);
        });
    });
}

void onCounters(std::span<const std::uint32_t> counters) noexcept
{
    if (folder.empty() || counters.size() < GI_COUNTER_COUNT) return;

    std::lock_guard lock(recordMutex);
    addToTallies([&](Tally& tally) {
        for (std::size_t i = 0; i < tally.counters.size(); ++i) tally.counters[i] += counters[i];
        ++tally.counterFrames;
    });
}

void capture(const std::filesystem::path& into, std::uint32_t frames) noexcept
{
    if (folder.empty()) return;

    guarded([&] {
        std::lock_guard lock(recordMutex);
        running = Capture{.file = into, .frames = std::max(frames, 1u), .tally = {}};
    });
}

void cancelCapture() noexcept
{
    std::lock_guard lock(recordMutex);
    running.reset();
}

std::uint64_t capturesFinished() noexcept
{
    return finished;
}

std::string lastCapture()
{
    std::lock_guard lock(lastMutex);
    return lastLine;
}
}
