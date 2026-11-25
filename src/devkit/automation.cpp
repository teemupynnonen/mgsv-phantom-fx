#include "devkit/automation.hpp"

#include "devkit/commands.hpp"
#include "devkit/cutscenes.hpp"
#include "devkit/extension.hpp"
#include "devkit/frame_log.hpp"
#include "devkit/game_lua.hpp"
#include "devkit/gamepad.hpp"
#include "devkit/host.hpp"
#include "devkit/keys.hpp"
#include "devkit/log.hpp"
#include "devkit/save_redirect.hpp"
#include "devkit/travel.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace devkit::automation
{
namespace
{
using Script = commands::Script;

// Frames between looks at where the game is while its startup logos would show.
constexpr std::uint32_t LogoPollFrames = 30;
// The game shows konamiLogo, kjpLogo and foxLogo six seconds each in this sequence (init_sequence.lua,
// Seq_Demo_ShowKonamiAndFoxLogo). Deleted and the sequence left as Infinite Heaven's skipLogos does, the
// pre-title comes at once.
constexpr std::string_view LogoSkipScript = R"(
if not TppSequence then return "" end
local dropped = 0
for _, name in ipairs({ "konamiLogo", "kjpLogo", "foxLogo" }) do
  local splash = SplashScreen and SplashScreen.GetSplashScreenWithName(name)
  if splash then SplashScreen.Delete(splash); dropped = dropped + 1 end
end
local sequence = TppSequence.GetCurrentSequenceName()
if sequence == "Seq_Demo_ShowKonamiAndFoxLogo" then TppUiCommand.StartPreTitleSequence() end
return sequence .. (dropped > 0 and (" dropped " .. dropped) or "")
)";
// The most a present job can say.
constexpr std::size_t JobTextBytes = 4096;
// Presents `frame` waits for the render thread to record a frame: with frame generation several a game frame.
constexpr std::uint64_t FrameWait = 3600;

std::atomic<bool> caseAsked{};
std::uint32_t framesSinceLogos{};
bool logosPassed{}; // A copy that holds `skip-logos`, once the game is past its startup logos

// What the add-ons plugged in (extension.hpp).
struct Command
{
    DevKitCommandFn fn;
    void* user;
};
struct Text
{
    DevKitTextFn fn;
    void* user;
};
struct Wait
{
    DevKitWaitFn fn;
    void* user;
    std::uint64_t deadline;
};
std::map<std::string, Command, std::less<>> commandTable;
std::vector<Text> stateTexts, statusTexts;
DevKitKeyFn caseKeyFn{};
void* caseKeyUser{};
// The waits a line leaves: the lines after it run once these are done.
std::vector<Wait> waits;

void resetWaits()
{
    waits.clear();
}

// The text a text function gives, whatever its length; nothing when it has no answer.
std::optional<std::string> fetch(const Text& source, const char* caseFolder)
{
    std::string text(512, '\0');
    auto length = source.fn(caseFolder, text.data(), text.size(), source.user);
    if (length == DevKitNone) return std::nullopt;

    if (length >= text.size())
    {
        text.assign(length + 1, '\0');
        length = source.fn(caseFolder, text.data(), text.size(), source.user);
        if (length == DevKitNone) return std::nullopt;
    }

    text.resize(std::min(length, text.size() - 1));

    return text;
}

// What the add-ons' texts say, one after another; each ends its line.
std::string joined(const std::vector<Text>& sources, const char* caseFolder)
{
    std::string all;
    for (const auto& source : sources)
    {
        const auto text = fetch(source, caseFolder);
        if (!text || text->empty()) continue;

        all += *text;
        if (all.back() != '\n') all += '\n';
    }

    return all;
}

std::string utf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();

    return std::string(text.begin(), text.end());
}

// What the game is at: its Lua's state, then what the add-ons say.
std::string stateText(const std::string& name, const char* caseFolder)
{
    std::string state;
    if (!name.empty()) state += "name=" + name + "\n";

    if (const auto outcome = game_lua::run(game_lua::StateScript, "=automation")) state += outcome->text + "\n";

    return state + joined(stateTexts, caseFolder);
}

// A case's folder: one word under cases/, never a path of the caller's own.
std::optional<std::filesystem::path> caseFolder(const std::string& name)
{
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos) return std::nullopt;

    return commands::folder() / "cases" / name;
}

// What the game is at into the case's folder; false when it could not be written.
bool writeState(const std::string& name)
{
    const auto folder = caseFolder(name);
    if (!folder) return false;

    std::error_code error;
    std::filesystem::create_directories(*folder, error);
    const auto folderText = utf8(*folder);
    std::ofstream file(*folder / "state.txt", std::ios::trunc);
    file << stateText(name, folderText.c_str());
    file.close();

    return !file.fail();
}

// The frame log's recording: the lines after `frame` wait for it.
Script& scriptOf(DevKitScript* script);
std::uint64_t recordingRequest{};

int pollRecording(DevKitScript* script, int timedOut, void*)
{
    auto& output = scriptOf(script).output;
    if (frame_log::completedRequest() == recordingRequest)
    {
        const auto name = frame_log::lastRecording();
        output += name.empty() ? "error: the frame's files could not be written; see DevKit/frame-log\n"
                               : "recorded " + name + " in frame-log\n";
        return 0;
    }

    if (!timedOut) return 1;

    output += "error: no frame was recorded\n";

    return 0;
}

std::string statusText()
{
    auto text = joined(statusTexts, nullptr);

    return text.empty() ? std::format("presented {}\n", commands::presents()) : text;
}

std::string listing()
{
    std::string text = "built in: lua travel cutscene key pad state peek wait status case_done help frame shaders shader\n";
    if (commandTable.empty()) return text + "added by add-ons: nothing\n";

    text += "added by add-ons:";
    for (const auto& [name, command] : commandTable) text += " " + name;

    return text + "\n";
}

void run(Script& running, const std::string& line)
{
    std::istringstream words(line);
    std::string command;
    words >> command;
    std::string rest;
    std::getline(words >> std::ws, rest);

    if (command.empty() || command == "wait") return;
    if (command == "lua")
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
    else if (command == "key")
    {
        // The screens that wait for one: the loading screens press space, sometimes more than once.
        std::istringstream arguments(rest);
        std::string name;
        int times{1};
        arguments >> name >> times;
        if (!keys::keyOf(name)) running.output += "error: key space|enter|escape|tab|up|down|left|right|e|f|q|0x<code> [times]\n";
        else
        {
            keys::press(name, std::max(times, 1));
            running.output += "pressed " + name + (times > 1 ? " " + std::to_string(times) + " times" : "") + "\n";
        }
    }
    else if (command == "pad")
    {
        // A button of a gamepad the game reads, without the keyboard or its focus. The screen that waits for a key
        // while the loop is stopped is asked through DevKit/pad instead (fx.py press_pad).
        std::istringstream arguments(rest);
        std::string name;
        int polls{6};
        arguments >> name >> polls;
        const auto button = gamepad::buttonOf(name);
        if (!button) running.output += "error: pad a|b|x|y|start|back|lb|rb|up|down|left|right|0x<mask> [polls]\n";
        else
        {
            gamepad::press(*button, std::max(polls, 1));
            running.output += "pressed pad " + name + "\n";
        }
    }
    else if (command == "cutscene")
    {
        std::size_t index{};
        if (std::istringstream(rest) >> index && index < cutscenes::labels().size())
        {
            cutscenes::play(index);
            running.output += "cutscene requested\n";
        }
        else running.output += "error: cutscene <index of the list>\n";
    }
    else if (command == "state")
    {
        if (rest.empty()) running.output += stateText("", nullptr);
        else if (!writeState(rest)) running.output += "error: the state could not be written to cases/" + rest + "\n";
    }
    else if (command == "status") running.output += statusText();
    else if (command == "help") running.output += listing();
    else if (command == "peek" && !rest.empty())
    {
        // `game+` takes an offset from the executable's base.
        std::string where;
        std::size_t length = 256;
        std::istringstream arguments(rest);
        arguments >> where >> std::dec >> length;
        std::uintptr_t address{};
        const bool isRva = where.starts_with("game+");
        const std::string_view digits = std::string_view(where).substr(isRva ? 5 : 0);
        const auto [stop, error] = std::from_chars(digits.data(), digits.data() + digits.size(), address, 16);
        if (error != std::errc{} || length == 0 || length > 4096) running.output += "error: peek <hex address or game+hex> [bytes, at most 4096]\n";
        else
        {
            if (isRva) address += reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            std::array<std::uint8_t, 16> row{};
            for (std::size_t at = 0; at < length; at += 16)
            {
                const std::size_t bytes = std::min<std::size_t>(row.size(), length - at);
                running.output += std::format("{:#014x}", address + at);
                SIZE_T read{};
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address + at), row.data(), bytes, &read) || read != bytes) running.output += " unreadable";
                else
                    for (std::size_t i = 0; i < bytes; ++i) running.output += std::format(" {:02x}", row[i]);
                running.output += '\n';
            }
        }
    }
    else if (command == "frame")
    {
        if (!frame_log::installed())
        {
            running.output += "error: the frame log is off: make the DevKit/frame-log folder beside the add-ons and start the game again\n";
            return;
        }

        recordingRequest = frame_log::request();
        if (!recordingRequest)
        {
            running.output += "error: a frame is already being recorded; run this after it\n";
            return;
        }
        waits.push_back({&pollRecording, nullptr, commands::presents() + FrameWait});
    }
    else if (command == "shaders")
    {
        // The most it lists: a search, not the whole game.
        constexpr std::size_t Most = 100;
        const auto found = frame_log::shaders(rest);
        for (std::size_t i = 0; i < found.size() && i < Most; ++i)
            running.output += std::format("{} {} {} {} bytes\n", found[i].stage, found[i].hash, found[i].name.empty() ? "-" : found[i].name, found[i].size);

        running.output += std::format("{} of {} shaders{}\n", found.size(), frame_log::shaderCount(), found.size() > Most ? ", the first 100 listed" : "");
    }
    else if (command == "shader")
    {
        std::istringstream arguments(rest);
        std::string which, action;
        arguments >> which >> action;
        if (which.empty() || (!action.empty() && action != "save")) running.output += "error: shader <hash start or part of a source name> [save]\n";
        else if (action == "save")
        {
            const auto saved = frame_log::saveShader(which);
            running.output += saved ? "saved " + utf8(*saved) + " and its .asm\n" : "error: no shader matches " + which + "\n";
        }
        else
        {
            const auto text = frame_log::disassemble(which);
            running.output += text ? *text : "error: no shader matches " + which + "\n";
        }
    }
    else if (command == "case_done")
    {
        // What the case's lines said, kept with the case.
        const auto folder = caseFolder(rest);
        if (!folder) running.output += "error: case names are one word\n";
        else
        {
            std::ofstream file(*folder / "log.txt", std::ios::binary | std::ios::trunc);
            file << running.output;
            file.close();
            if (file.fail()) running.output += "error: the case log could not be written to cases/" + rest + "\n";
            else
            {
                devkit::log("Debug case " + rest + " saved.");
                devkit::toast(Tone::Ok, "Debug case saved", "cases/" + rest);
            }
        }
    }
    else if (const auto found = commandTable.find(command); found != commandTable.end())
        found->second.fn(reinterpret_cast<DevKitScript*>(&running), rest.c_str(), found->second.user);
    else running.output += "error: unknown command " + command + ": no loaded add-on adds it (help lists them)\n";
}

// Whether the line that last ran still has an add-on working for it.
bool isWaiting(Script& running)
{
    for (auto wait = waits.begin(); wait != waits.end();)
    {
        const bool timedOut = commands::presents() >= wait->deadline;
        if (wait->fn(reinterpret_cast<DevKitScript*>(&running), timedOut ? 1 : 0, wait->user) != 0 && !timedOut) return true;

        wait = waits.erase(wait);
    }

    return false;
}

// F9: what the add-on that took it does.
void onCaseKey()
{
    if (commands::isRunning())
    {
        devkit::toast(Tone::Warn, "A debug case is running");
        return;
    }

    if (!caseKeyFn)
    {
        devkit::toast(Tone::Warn, "No debug case", "No add-on has one to take.");
        return;
    }

    caseKeyFn(caseKeyUser);
}

// The game's startup logos, dropped for a copy that holds `skip-logos` in its folder: a fresh run spends
// eighteen seconds on them before the title (init_sequence.lua, LogoSkipScript).
void skipStartupLogos() noexcept
{
    if (logosPassed || ++framesSinceLogos < LogoPollFrames) return;

    framesSinceLogos = 0;
    std::error_code error;
    const auto& folder = commands::folder();
    if (folder.empty() || !std::filesystem::exists(folder / "skip-logos", error)) return;

    const auto outcome = game_lua::run(LogoSkipScript, "=skip logos", false);
    if (!outcome) return;

    const auto& result = outcome->text;
    if (result.find(" dropped ") != std::string::npos || result.starts_with("Seq_Demo_ShowKonamiAndFoxLogo")) devkit::log("Startup logos: " + result);
    logosPassed = result.starts_with("Seq_Game_");
}

// The API the add-ons are handed.
std::deque<std::string> linesOf(const char* text)
{
    std::deque<std::string> lines;
    std::istringstream stream(text ? text : "");
    for (std::string line; std::getline(stream, line);)
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
    }

    return lines;
}

Script& scriptOf(DevKitScript* script)
{
    return *reinterpret_cast<Script*>(script);
}

int addCommand(const char* name, DevKitCommandFn fn, void* user)
{
    if (!name || !*name || !fn) return 0;

    return commandTable.try_emplace(name, Command{fn, user}).second ? 1 : 0;
}

void addState(DevKitTextFn fn, void* user)
{
    if (fn) stateTexts.push_back({fn, user});
}

void addStatus(DevKitTextFn fn, void* user)
{
    if (fn) statusTexts.push_back({fn, user});
}

void setKey(DevKitKeyFn fn, void* user)
{
    caseKeyFn = fn;
    caseKeyUser = user;
}

void say(DevKitScript* script, const char* text)
{
    if (script && text) scriptOf(script).output += text;
}

void queue(DevKitScript* script, const char* lines)
{
    if (!script) return;

    auto& pending = scriptOf(script).lines;
    const auto added = linesOf(lines);
    pending.insert(pending.begin(), added.begin(), added.end());
}

void waitUntil(DevKitScript*, DevKitWaitFn fn, std::uint64_t presents, void* user)
{
    if (fn) waits.push_back({fn, user, commands::presents() + presents});
}

void presentJob(DevKitScript*, DevKitPresentFn fn, void* user)
{
    if (!fn) return;

    commands::askPresent([fn, user](ID3D11DeviceContext* context, ID3D11Resource* backBuffer) {
        std::string text(JobTextBytes, '\0');
        const auto length = fn(context, backBuffer, text.data(), text.size(), user);
        text.resize(std::min(length == DevKitNone ? std::size_t{} : length, text.size() - 1));

        return text;
    });
}

void start(const char* lines)
{
    commands::start(linesOf(lines));
}

std::uint64_t presents()
{
    return commands::presents();
}

std::size_t folder(char* out, std::size_t cap)
{
    const auto text = utf8(commands::folder());
    if (out && cap)
    {
        const auto count = std::min(text.size(), cap - 1);
        std::memcpy(out, text.data(), count);
        out[count] = '\0';
    }

    return text.size();
}

void showToast(int tone, const char* text, const char* detail)
{
    constexpr Tone Tones[]{Tone::Dim, Tone::Ok, Tone::Warn, Tone::Bad};
    devkit::toast(Tones[std::clamp(tone, 0, 3)], text ? text : "", detail ? detail : "");
}

void writeLog(const char* text)
{
    devkit::log(text ? text : "");
}

std::size_t recordingFolder(char* out, std::size_t cap)
{
    const auto text = utf8(frame_log::recordingDirectory());
    if (out && cap)
    {
        const auto count = std::min(text.size(), cap - 1);
        std::memcpy(out, text.data(), count);
        out[count] = '\0';
    }

    return text.size();
}

const DevKitApi Api{sizeof(DevKitApi), DevKitApiVersion, &addCommand, &addState, &addStatus, &setKey, &say, &queue, &waitUntil, &presentJob, &start,
    &presents, &folder, &showToast, &writeLog, &recordingFolder};
}

const DevKitApi* extensionApi() noexcept
{
    return &Api;
}

void install(const std::filesystem::path& addonDirectory) noexcept
{
    std::error_code error;
    const auto candidate = addonDirectory / "DevKit";

    // The automation copy must not change the player's saves. Left running without its own it would, even with
    // no commands, so it quits. Without a `saves` folder this is the player's game.
    const auto saves = candidate / "saves", userData = candidate / "local";
    const auto savesStatus = std::filesystem::status(saves, error);
    // A lookup that failed is not "no saves folder": running on the player's saves is the one thing this must not do.
    if (error && error != std::errc::no_such_file_or_directory)
    {
        devkit::log("The automation copy quits: " + saves.string() + " could not be looked at (" + error.message() + ").");
        TerminateProcess(GetCurrentProcess(), 1);
    }

    if (savesStatus.type() == std::filesystem::file_type::directory
        && !(std::filesystem::is_directory(userData, error) && save_redirect::install(saves, userData)))
    {
        devkit::log("The automation copy quits: it needs " + saves.string() + " with the saves to start from and " + userData.string() + ", and the game's files redirected there.");
        TerminateProcess(GetCurrentProcess(), 1);
    }

    std::filesystem::create_directories(candidate / "cases", error);
    if (error) return;

    commands::install(candidate, run, isWaiting, resetWaits);
    devkit::log("Commands are read from " + candidate.string() + ".");
}

void caseKey() noexcept
{
    if (!commands::folder().empty()) caseAsked = true;
}

void onFrameStart() noexcept
{
    skipStartupLogos();
    if (caseAsked.exchange(false)) onCaseKey();

    commands::onFrameStart();
}

void onPresent(ID3D11DeviceContext* context, ID3D11Resource* backBuffer) noexcept
{
    commands::onPresent(context, backBuffer);
}
}
