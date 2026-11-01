#include "dev/dev.hpp"

#include "core.hpp"
#include "dev/automation.hpp"
#include "dev/cutscenes.hpp"
#include "dev/game_lua.hpp"
#include "dev/perf_record.hpp"
#include "dev/remaster/tool.hpp"
#include "dev/travel.hpp"
#include "fxui/fxui.hpp"
#include "log.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>

namespace phantom_fx::dev
{
namespace
{
using fxui::State;
using fxui::Tone;

// A tool that runs Lua has nothing to run on a game version without it.
fxui::ItemSpec luaTool(fxui::ItemSpec spec)
{
    if (game_lua::isSupported()) return spec;

    spec.state = State::Unavailable;
    spec.message = "Unavailable for this game version.";

    return spec;
}

// Free roam in another of the game's maps.
void drawTravel()
{
    const fxui::Item item(luaTool({.id = "travel",
        .name = "Travel",
        .summary = "Free roam in another map, after one loading screen.",
        .keywords = "teleport map location Afghanistan Africa Mother Base free roam"}));
    if (!item || !game_lua::isSupported()) return;

    std::array<fxui::Option, static_cast<std::size_t>(travel::Map::Count)> maps{};
    for (std::size_t i = 0; i < maps.size(); ++i) maps[i].label = travel::name(static_cast<travel::Map>(i));
    const int pressed = fxui::segmented("Go to", -1, maps,
        "Leaves free roam as the helicopter would and arrives on foot: in Afghanistan and Africa where the game puts a player found off the map, "
        "at Mother Base on the Command platform. The game saves on the way, as it does when leaving free roam.");
    if (pressed >= 0) travel::go(static_cast<travel::Map>(pressed));
}

// Any of the game's cutscenes, in its own mission.
void drawCutscenes()
{
    const fxui::Item item(luaTool({.id = "cutscenes",
        .name = "Cutscenes",
        .summary = "Plays any cutscene the game's scripts name, after one loading screen.",
        .keywords = "demo movie story mission play watch scene"}));
    if (!item || !game_lua::isSupported()) return;

    static std::optional<std::size_t> selected;
    const auto labels = cutscenes::labels();
    const char* const preview = selected ? labels[*selected].data() : "Pick a cutscene";
    if (const auto picked = fxui::searchCombo("Cutscene", preview, labels, selected)) selected = picked;

    if (fxui::button("Play",
            "From free roam or the ACC. A story cutscene loads its mission and ends in the ACC, with no mission clear; a Mother Base one ends in free "
            "roam there. The game saves on the way, as it does when leaving free roam. The story stays where it is, unless the game plays another "
            "Mother Base cutscene in its place, which a toast says. Cutscenes start in the mission's own time and weather, not always the ones they "
            "have in the story.",
            selected.has_value()))
        cutscenes::play(*selected);
}

// Lua typed here runs in the game's own script state.
void drawLuaConsole()
{
    const fxui::Item item(luaTool({.id = "luaConsole",
        .name = "Lua console",
        .summary = "Runs Lua in the game's own scripts at the next frame: an expression shows its value, print() its lines.",
        .keywords = "script execute run code vars gvars mvars svars TppMission"}));
    if (!item || !game_lua::isSupported()) return;

    static std::array<char, 16 * 1024> code{};
    const bool submitted = fxui::textBox("##luaCode", code, 8);
    const auto console = game_lua::console();
    if (fxui::button("Run", "Ctrl+Enter in the box runs it too. It runs on the game thread at the next frame start, as the game's own scripts do.",
            !console.waiting)
        || submitted)
        game_lua::submit(code.data());
    if (!console.last) return;

    fxui::gap(6);
    const auto& last = *console.last;
    fxui::note(last.ran ? Tone::Ok : Tone::Bad, std::format("Run {}: {}", console.runs, last.ran ? "done" : "error"));
    if (last.text.empty()) return;

    std::string output = last.text;
    const auto lines = static_cast<float>(std::ranges::count(output, '\n') + 1);
    fxui::textBox("##luaOutput", {output.data(), output.size() + 1}, std::clamp(lines, 1.0f, 12.0f), true);
}
}

void install(const std::filesystem::path& addonDirectory) noexcept
{
    automation::install(addonDirectory);
    perf_record::install(addonDirectory / "PhantomFX-automation" / "perf");
}

void onFrameStart() noexcept
{
    travel::onFrameStart();
    cutscenes::onFrameStart();
    game_lua::onFrameStart();
    automation::onFrameStart();
    perf_record::onFrameStart();
}

void onPresent(ID3D11DeviceContext* context, ID3D11Resource* backBuffer) noexcept
{
    automation::onPresent(context, backBuffer);
}

void onCaseKey() noexcept
{
    automation::caseKey();
}

void drawPage()
{
    const fxui::Page page(Section::Developer, "Tools for testing in the game. Only the dev build has this page.");
    if (!page) return;

    drawTravel();
    drawCutscenes();
    drawLuaConsole();
    remaster::drawTool();
    if constexpr (DiagnosticLog)
    {
        auto options = core::currentDiagnostics();
        const fxui::Item item({.id = "globalIlluminationLog", .name = "Global illumination log", .keywords = "log often light fade"});
        if (item
            && fxui::checkbox("Log global illumination often", options.logGlobalIlluminationOften,
                "With global illumination on, its light and each step's largest value go to the log every few frames, not every ten seconds: to see "
                "how fast light fades from each step after a light goes away."))
            core::applyDiagnostics(options);
    }
}
}
