#include "dev/automation.hpp"

#include "core.hpp"
#include "dev/inspect_trace.hpp"
#include "features/photo_mode/photo_mode.hpp"
#include "fxui/fxui.hpp"
#include "game_loop.hpp"
#include "dev/game_lua.hpp"
#include "log.hpp"
#include "passes/ray_tracing/engine_scene.hpp"
#include "passes/ray_tracing/gi_renderer.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "passes/sun_light_pass.hpp"
#include "resource_dump.hpp"
#include "dev/save_redirect.hpp"
#include "dev/travel.hpp"
#include "weather.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <exception>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace phantom_fx::automation
{
namespace
{
// A directory scan every frame start would find nothing most of the time.
constexpr std::uint32_t PollFrames = 6;
// Presents a line waits for the render thread before it gives up: with frame generation several a game frame.
constexpr std::uint64_t InspectionFrames = 3600, SceneFrames = 3600;
// Presents the scene's work to do stays the same before it counts as made: a model that never gets room
// would otherwise hold a case up for SceneFrames.
constexpr std::uint64_t SettledFrames = 60;
// Presents after a probe's pixel changes before its text has that pixel: a readback's frames in flight.
constexpr std::string_view ProbeWait = "wait 8";

// What the game is at, for a case to be found again (tools/automation/fx.py replay). Each value alone, as
// any may fail in a loading screen. A story mission or a cutscene is found again only as a place on its map.
constexpr std::string_view StateScript = R"(
local function value(read)
  local ok, result = pcall(read)
  return ok and tostring(result) or "?"
end
return "mission=" .. value(function() return vars.missionCode end)
  .. "\nplayer=" .. value(function() return vars.playerPosX end) .. " " .. value(function() return vars.playerPosY end) .. " " .. value(function() return vars.playerPosZ end)
  .. "\nlocation=" .. value(function() return TppLocation.GetLocationName() end)
  .. "\ncutscene=" .. value(function() return DemoDaemon.IsDemoPlaying() end)
)";

std::filesystem::path folder; // Empty while off
std::atomic<std::uint64_t> presents{};
std::atomic<bool> caseAsked{};

// A job the render thread does at its next present, and what it reports.
using PresentJob = std::function<std::string(ID3D11DeviceContext*, ID3D11Resource* backBuffer)>;
std::mutex jobMutex;
std::optional<PresentJob> job;
std::optional<std::string> jobResult;

// The command file running, on the game thread only.
struct Script
{
    std::filesystem::path result;
    std::deque<std::string> lines;
    std::string output;
    std::optional<std::uint64_t> waitUntil; // A present count
    bool isWaitingForJob{};
    std::optional<std::uint64_t> inspectionsBefore;
    std::uint64_t inspectionDeadline{};
    // Waiting for the scene: the give-up present, the work it had to do, and the present that last changed.
    std::optional<std::uint64_t> sceneDeadline;
    std::size_t sceneWork{};
    std::uint64_t sceneBusyAt{};
};
std::optional<Script> script;
std::uint32_t framesSincePoll{};
bool probeTurnedOnView{};

std::optional<std::filesystem::path> nextCommandFile()
{
    std::error_code error;
    std::optional<std::filesystem::path> first;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error))
        if (entry.path().extension() == ".cmd" && (!first || entry.path() < *first)) first = entry.path();

    return first;
}

Script load(const std::filesystem::path& file)
{
    Script loaded{.result = std::filesystem::path(file).replace_extension(".result")};
    std::ifstream input(file);
    for (std::string line; std::getline(input, line);)
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) loaded.lines.push_back(std::move(line));
    }
    input.close();

    std::error_code error;
    std::filesystem::remove(file, error);

    return loaded;
}

// Written under another name first, so a reader never sees half of it. The key's script has no reader.
void finish(const Script& finished)
{
    if (finished.result.empty()) return;

    auto partial = finished.result;
    partial += ".partial";
    std::ofstream(partial, std::ios::binary) << finished.output;
    std::error_code error;
    std::filesystem::rename(partial, finished.result, error);
}

std::string_view trimmed(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t");
    if (first == text.npos) return {};

    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// What the scene has yet to make.
std::size_t sceneWork()
{
    return engine_scene::waitingFiles() + gi_renderer::waitingWork();
}

std::string text(const photo_mode::Pose& pose)
{
    return std::format("{} {} {} {} {}", pose.position[0], pose.position[1], pose.position[2], pose.yaw, pose.pitch);
}

std::optional<photo_mode::Pose> readPose(std::istream& numbers)
{
    photo_mode::Pose pose;
    if (numbers >> pose.position[0] >> pose.position[1] >> pose.position[2] >> pose.yaw >> pose.pitch) return pose;

    return std::nullopt;
}

std::string timeNow()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);

    return std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
}

std::string status()
{
    const auto camera = photo_mode::lastCamera();
    auto line = std::format("frame {}, presented {}, {}, ", game_loop::currentFrame(), presents.load(), photo_mode::isHeld() ? "frozen" : "running");
    line += camera ? "camera " + text(*camera) : std::string("no camera yet");

    return line + "\n" + ray_tracing::status();
}

// A case: the frame as traced with what the game itself says, as the game draws it, then the scene with
// everything it leaves out, to name what the game drew and it lacks. The game stays frozen for more
// questions. tools/automation/case.py reads it.
std::deque<std::string> caseLines(const std::string& name)
{
    const auto path = "cases/" + name;
    return {"game_shadow 1", "freeze", "wait 30", "state " + name, "inspect " + path + "/scene", "image " + path + "/traced", "textures " + path, "game_shadow 0",
        "enhancements 0", "wait 45", "image " + path + "/vanilla", "enhancements 1", "reveal all", "inspect " + path + "/revealed", "reveal off", "case_done " + name};
}

// What the game is at into the case's folder.
void writeState(const std::string& name)
{
    const auto caseFolder = folder / "cases" / name;
    std::error_code error;
    std::filesystem::create_directories(caseFolder, error);

    std::ofstream state(caseFolder / "state.txt", std::ios::trunc);
    const auto camera = photo_mode::lastCamera();
    state << "name=" << name << "\nframe=" << game_loop::currentFrame() << "\ncamera=" << (camera ? text(*camera) : std::string("?")) << "\n";
    if (const auto outcome = game_lua::run(StateScript, "=automation")) state << outcome->text << "\n";

    const auto sky = weather::state();
    state << "clock=" << sky.clock << "\nweather=" << (sky.requested ? static_cast<int>(*sky.requested) : -1) << " " << sky.fogDensity << "\n";
    state << "scene=" << ray_tracing::status() << "\n";
    std::filesystem::copy_file(folder.parent_path() / "PhantomFX.ini", caseFolder / "PhantomFX.ini", std::filesystem::copy_options::overwrite_existing, error);
}

void askJob(Script& running, PresentJob asked)
{
    std::scoped_lock lock(jobMutex);
    job = std::move(asked);
    jobResult.reset();
    running.isWaitingForJob = true;
}

void setEnhancements(bool isOn)
{
    if (core::enhancementsOn() != isOn) core::setEnhancements(isOn);
}

void reveal(std::string_view what)
{
    engine_scene::Reveal revealed;
    if (what == "all") revealed.everything = true;
    else if (what != "off")
    {
        std::istringstream hashes{std::string(what)};
        hashes >> std::hex;
        for (std::uint64_t hash{}; hashes >> hash;) revealed.hashes.push_back(hash);
    }

    engine_scene::setReveal(std::move(revealed));
}

void setDiagnostic(Script& running, const std::string& rest)
{
    std::istringstream arguments(rest);
    std::string name;
    int value{};
    auto diagnostics = core::currentDiagnostics();
    const std::pair<std::string_view, bool*> flags[]{{"show_scene", &diagnostics.showTracedScene}, {"two_sided_shadows", &diagnostics.twoSidedShadows},
        {"no_jitter", &diagnostics.noJitter}};
    const bool isRead = static_cast<bool>(arguments >> name >> value);
    const auto* const flag = std::ranges::find(flags, name, &std::pair<std::string_view, bool*>::first);
    if (isRead && flag != std::end(flags)) *flag->second = value != 0;
    else if (isRead && name == "debug_view") diagnostics.debugView = static_cast<DebugView>(value);
    else running.output += "error: diagnostic debug_view|show_scene|two_sided_shadows|no_jitter <value>\n";

    core::applyDiagnostics(diagnostics);
}

void showDebugView(DebugView view)
{
    auto diagnostics = core::currentDiagnostics();
    diagnostics.debugView = view;
    core::applyDiagnostics(diagnostics);
}

void run(Script& running, const std::string& line)
{
    std::istringstream words(line);
    std::string command;
    words >> command;
    std::string rest;
    std::getline(words >> std::ws, rest);
    const auto invert = [&](std::string_view off) { return rest != off; };

    if (command == "case")
    {
        const auto lines = caseLines(rest.empty() ? timeNow() : rest);
        running.lines.insert(running.lines.begin(), lines.begin(), lines.end());
    }
    else if (command == "freeze")
    {
        if (const auto camera = photo_mode::lastCamera()) photo_mode::hold(*camera);
        else running.output += "error: no camera yet\n";
    }
    else if (command == "release")
    {
        photo_mode::release();
        ray_tracing::clearProbe();
        if (std::exchange(probeTurnedOnView, false)) showDebugView(DebugView::FinalImage);
    }
    else if (command == "camera")
    {
        std::istringstream numbers(rest);
        if (const auto pose = readPose(numbers)) photo_mode::hold(*pose);
        else running.output += "error: camera <x> <y> <z> <yaw> <pitch>\n";
    }
    else if (command == "move")
    {
        std::istringstream numbers(rest);
        const auto pose = readPose(numbers);
        int frames{};
        if (pose && numbers >> frames) photo_mode::move(*pose, frames);
        else running.output += "error: move <x> <y> <z> <yaw> <pitch> <frames>\n";
    }
    else if (command == "wait")
    {
        std::uint64_t frames{};
        if (std::istringstream(rest) >> frames) running.waitUntil = presents + frames;
        else running.output += "error: wait <frames>\n";
    }
    else if (command == "wait_scene")
    {
        running.sceneDeadline = presents + SceneFrames;
        running.sceneWork = sceneWork();
        running.sceneBusyAt = presents;
    }
    else if (command == "probe")
    {
        std::uint32_t x{}, y{};
        if (!(std::istringstream(rest) >> x >> y))
        {
            running.output += "error: probe <x> <y>\n";
            return;
        }

        ray_tracing::probeAt(x, y);
        if (core::currentDiagnostics().debugView != DebugView::ShadowRays)
        {
            showDebugView(DebugView::ShadowRays);
            probeTurnedOnView = true;
        }

        running.lines.push_front("probe_text");
        running.lines.emplace_front(ProbeWait);
    }
    else if (command == "probe_text") running.output += ray_tracing::shadowProbe() + "\n";
    else if (command == "inspect" && !rest.empty())
    {
        running.inspectionsBefore = inspect_trace::finished();
        running.inspectionDeadline = presents + InspectionFrames;
        inspect_trace::request(folder / rest);
    }
    else if (command == "image" && !rest.empty())
    {
        askJob(running, [file = folder / (rest + ".dds"), rest](ID3D11DeviceContext* context, ID3D11Resource* backBuffer) {
            return saveTexture(context, backBuffer, DXGI_FORMAT_UNKNOWN, file) ? "saved " + rest + ".dds\n" : "error: the back buffer could not be saved\n";
        });
    }
    else if (command == "textures" && !rest.empty())
    {
        askJob(running, [path = folder / rest, rest](ID3D11DeviceContext* context, ID3D11Resource*) {
            ray_tracing::saveTextures(context, path);
            sun_light_pass::saveGameShadow(context, path / "game-shadow.dds");
            return "saved the textures into " + rest + "\n";
        });
    }
    else if (command == "game_shadow") sun_light_pass::keepGameShadow(invert("0"));
    else if (command == "enhancements") setEnhancements(invert("0"));
    else if (command == "reveal")
    {
        // The scene reads its models again with what it takes: restarted by the enhancements switch.
        for (const auto& later : {std::string("wait_scene"), std::string("enhancements 1"), "reveal_now " + rest, std::string("wait 5"), std::string("enhancements 0")})
            running.lines.push_front(later);
    }
    else if (command == "reveal_now") reveal(rest);
    else if (command == "setting")
    {
        const auto separator = rest.find('=');
        const auto settings = core::currentSettings();
        const auto changed = separator == rest.npos ? settings : withLine(settings, trimmed(std::string_view(rest).substr(0, separator)),
                                                                     trimmed(std::string_view(rest).substr(separator + 1)));
        core::applySettings(changed);
    }
    else if (command == "diagnostic") setDiagnostic(running, rest);
    else if (command == "lua")
    {
        const auto outcome = game_lua::run(rest, "=automation", true);
        if (!outcome) running.output += "error: the game's scripts are not running yet\n";
        else running.output += (outcome->ran ? "" : "error: ") + outcome->text + "\n";
    }
    else if (command == "travel")
    {
        constexpr std::pair<std::string_view, travel::Map> Maps[]{
            {"afghanistan", travel::Map::Afghanistan}, {"africa", travel::Map::Africa}, {"motherbase", travel::Map::MotherBase}};
        std::istringstream arguments(rest);
        std::string name;
        arguments >> name;
        std::array<float, 3> place{};
        const bool hasPlace = static_cast<bool>(arguments >> place[0] >> place[1] >> place[2]);
        const auto* const map = std::ranges::find(Maps, name, &std::pair<std::string_view, travel::Map>::first);
        if (map != std::end(Maps)) travel::go(map->second, hasPlace ? std::optional(place) : std::nullopt);
        else running.output += "error: travel afghanistan|africa|motherbase [<x> <y> <z>]\n";
    }
    else if (command == "clock")
    {
        float seconds{};
        if (std::istringstream(rest) >> seconds)
        {
            weather::setClock(seconds);
            weather::stopClock(true);
        }
        else running.output += "error: clock <seconds>\n";
    }
    else if (command == "weather" && rest == "off") weather::release();
    else if (command == "weather")
    {
        std::istringstream arguments(rest);
        int type{};
        float fog{};
        if (arguments >> type && type >= 0 && type < static_cast<int>(weather::Type::Count))
        {
            arguments >> fog;
            weather::force(static_cast<weather::Type>(type), fog);
        }
        else running.output += "error: weather <type> [<fog>]\n";
    }
    else if (command == "state") writeState(rest);
    else if (command == "case_done")
    {
        std::ofstream(folder / "cases" / rest / "log.txt", std::ios::binary | std::ios::trunc) << running.output;
        log("Debug case " + rest + " saved.");
        fxui::toast(fxui::Tone::Ok, "Debug case saved", "The game stays frozen: F9 lets it go.");
    }
    else if (command == "status") running.output += status() + "\n";
    else running.output += "error: unknown command\n";
}

// Whether the line that last ran still has the render thread working for it; a line it gave up on says so.
bool isWaiting(Script& running)
{
    if (running.waitUntil && presents < *running.waitUntil) return true;

    running.waitUntil.reset();
    if (running.isWaitingForJob)
    {
        std::scoped_lock lock(jobMutex);
        if (!jobResult) return true;

        running.output += *jobResult;
        running.isWaitingForJob = false;
    }

    if (running.inspectionsBefore)
    {
        if (inspect_trace::finished() != *running.inspectionsBefore) running.inspectionsBefore.reset();
        else if (presents < running.inspectionDeadline) return true;
        else
        {
            running.output += "error: no record came: is the ray-traced scene running?\n";
            running.inspectionsBefore.reset();
        }
    }

    if (running.sceneDeadline)
    {
        if (const auto work = sceneWork(); work != running.sceneWork)
        {
            running.sceneWork = work;
            running.sceneBusyAt = presents;
        }

        if (presents >= *running.sceneDeadline) running.output += "error: the scene is still changing\n";
        else if (presents < running.sceneBusyAt + SettledFrames) return true;
        else if (running.sceneWork) running.output += std::format("the scene left {} things unmade\n", running.sceneWork);

        running.sceneDeadline.reset();
    }

    return false;
}

// Runs lines until one has to wait for frames or finishes the file.
void step(Script& running)
{
    while (!isWaiting(running))
    {
        if (running.lines.empty())
        {
            finish(running);
            script.reset();
            return;
        }

        const auto line = std::move(running.lines.front());
        running.lines.pop_front();
        running.output += "> " + line + "\n";
        run(running, line);
    }
}

// F9: a case, or the game let go after one.
void onCaseKey()
{
    if (script)
    {
        fxui::toast(fxui::Tone::Warn, "A debug case is running");
        return;
    }

    Script lines;
    if (photo_mode::isHeld())
    {
        lines.lines = {"release"};
        fxui::toast(fxui::Tone::Ok, "The game is let go");
    }
    else
    {
        lines.lines = {"case"};
        fxui::toast(fxui::Tone::Ok, "Debug case", "Hold on while it records: the picture changes a few times.");
    }

    script = std::move(lines);
}
}

void install(const std::filesystem::path& addonDirectory) noexcept
{
    std::error_code error;
    const auto candidate = addonDirectory / "PhantomFX-automation";

    // The automation copy must not change the player's saves. Left running without its own it would, even with
    // no commands, so it quits. Without a `saves` folder this is the player's game.
    const auto saves = candidate / "saves", userData = candidate / "local";
    if (std::filesystem::is_directory(saves, error) && !(std::filesystem::is_directory(userData, error) && save_redirect::install(saves, userData)))
    {
        log("The automation copy quits: it needs " + saves.string() + " with the saves to start from and " + userData.string() + ", and the game's files redirected there.");
        TerminateProcess(GetCurrentProcess(), 1);
    }

    std::filesystem::create_directories(candidate / "cases", error);
    if (error) return;

    folder = candidate;
    log("Automation reads command files from " + folder.string() + ".");
}

void caseKey() noexcept
{
    if (!folder.empty()) caseAsked = true;
}

void onFrameStart() noexcept
{
    if (folder.empty()) return;

    try
    {
        if (caseAsked.exchange(false)) onCaseKey();

        if (!script)
        {
            if (++framesSincePoll < PollFrames) return;

            framesSincePoll = 0;
            if (const auto file = nextCommandFile()) script = load(*file);
            if (!script) return;
        }

        step(*script);
    }
    catch (const std::exception& error)
    {
        log(std::string("Automation: ") + error.what());
        if (script)
        {
            script->output += std::string("error: ") + error.what() + "\n";
            finish(*script);
        }

        script.reset();
    }
}

void onPresent(ID3D11DeviceContext* context, ID3D11Resource* backBuffer) noexcept
{
    if (folder.empty()) return;

    ++presents;

    std::scoped_lock lock(jobMutex);
    if (!job) return;

    try
    {
        jobResult = (*job)(context, backBuffer);
    }
    catch (const std::exception& error)
    {
        jobResult = std::string("error: ") + error.what() + "\n";
    }

    job.reset();
}
}
