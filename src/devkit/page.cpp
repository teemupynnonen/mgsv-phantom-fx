#include "devkit/page.hpp"

#include "devkit/cutscenes.hpp"
#include "devkit/frame_log.hpp"
#include "devkit/game.hpp"
#include "devkit/game_lua.hpp"
#include "devkit/host.hpp"
#include "devkit/travel.hpp"

#include <imgui.h>
#include <reshade_overlay.hpp> // ImGui, through the overlay's function table.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cfloat>
#include <optional>
#include <string>
#include <string_view>

namespace devkit::page
{
namespace
{
std::optional<std::size_t> selectedCutscene;
std::array<char, 256> cutsceneFilter{};
std::array<char, 16 * 1024> luaCode{};
std::string luaOutput;
std::array<char, 256> shaderFilter{};
std::string selectedShader, shaderAssembly, shaderNote;

bool luaAvailable() noexcept
{
    return game::isSupportedBuild() && game_lua::isSupported();
}

bool containsIgnoringCase(std::string_view text, std::string_view filter)
{
    const auto same = [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
    return std::search(text.begin(), text.end(), filter.begin(), filter.end(), same) != text.end();
}

void drawTravel()
{
    ImGui::SeparatorText("Travel");

    const bool available = luaAvailable();
    ImGui::BeginDisabled(!available);
    static int map{};
    std::array<const char*, static_cast<std::size_t>(travel::Map::Count)> names{};
    for (std::size_t i = 0; i < names.size(); ++i) names[i] = travel::name(static_cast<travel::Map>(i));
    ImGui::SetNextItemWidth(200.0f);
    ImGui::Combo("Map", &map, names.data(), static_cast<int>(names.size()));
    ImGui::SameLine();
    if (ImGui::Button("Travel")) travel::go(static_cast<travel::Map>(map));
    ImGui::EndDisabled();
    if (!available) ImGui::TextDisabled("Needs the game's Lua and a known build.");
}

void drawCutscenes()
{
    ImGui::SeparatorText("Cutscenes");

    const bool available = luaAvailable();
    ImGui::BeginDisabled(!available);
    ImGui::SetNextItemWidth(300.0f);
    ImGui::InputTextWithHint("##cutsceneFilter", "Filter cutscenes", cutsceneFilter.data(), cutsceneFilter.size());

    const auto labels = cutscenes::labels();
    const std::string_view filter(cutsceneFilter.data());
    ImGui::BeginChild("##cutsceneList", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 8.0f), ImGuiChildFlags_Borders);
    for (std::size_t i = 0; i < labels.size(); ++i)
    {
        if (!containsIgnoringCase(labels[i], filter)) continue;
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(labels[i].data(), selectedCutscene == i)) selectedCutscene = i;
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::BeginDisabled(!selectedCutscene);
    if (ImGui::Button("Play cutscene") && selectedCutscene) cutscenes::play(*selectedCutscene);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!available) ImGui::TextDisabled("Needs the game's Lua and a known build.");
}

// The shaders the game created: searched by stage, hash or source name, taken apart on a click.
void drawShaders()
{
    ImGui::SeparatorText("Shaders");

    ImGui::SetNextItemWidth(300.0f);
    ImGui::InputTextWithHint("##shaderFilter", "Filter by stage, hash or source name", shaderFilter.data(), shaderFilter.size());
    const auto found = frame_log::shaders(shaderFilter.data());
    ImGui::SameLine();
    ImGui::TextDisabled("%zu of %zu", found.size(), frame_log::shaderCount());

    // A long list is searched, not scrolled: the first rows only.
    constexpr std::size_t Most = 300;
    ImGui::BeginChild("##shaderList", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 10.0f), ImGuiChildFlags_Borders);
    for (std::size_t i = 0; i < found.size() && i < Most; ++i)
    {
        const auto& shader = found[i];
        const auto label = shader.stage + " " + shader.hash + "  " + shader.name + "  (" + std::to_string(shader.size) + " bytes)";
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(label.c_str(), selectedShader == shader.hash))
        {
            selectedShader = shader.hash;
            shaderAssembly = frame_log::disassemble(shader.hash).value_or("");
            shaderNote.clear();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (!selectedShader.empty())
    {
        if (ImGui::Button("Copy hash")) ImGui::SetClipboardText(selectedShader.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Save .dxbc and .asm"))
        {
            const auto saved = frame_log::saveShader(selectedShader);
            shaderNote = saved ? "Saved " + saved->filename().string() + " in DevKit/shaders" : "Could not save it";
        }
        if (!shaderNote.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", shaderNote.c_str());
        }

        ImGui::InputTextMultiline("##shaderAssembly", shaderAssembly.data(), shaderAssembly.size() + 1, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 14.0f),
            ImGuiInputTextFlags_ReadOnly);
    }

    if (frame_log::installed())
    {
        if (ImGui::Button("Record a frame (F10)")) frame_log::request();
        const auto last = frame_log::lastRecording();
        if (!last.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Last: %s in DevKit/frame-log", last.c_str());
        }
    }
    else ImGui::TextDisabled("Make a DevKit/frame-log folder beside the add-ons and restart the game to record frames.");
}

void drawLuaConsole()
{
    ImGui::SeparatorText("Lua console");

    const bool available = luaAvailable();
    ImGui::BeginDisabled(!available);
    ImGui::InputTextMultiline("##luaCode", luaCode.data(), luaCode.size(), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8.0f),
        ImGuiInputTextFlags_AllowTabInput);
    const auto console = game_lua::console();
    ImGui::BeginDisabled(console.waiting);
    if (ImGui::Button("Run")) game_lua::submit(luaCode.data());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (console.waiting) ImGui::TextDisabled("Waiting for the game's next frame.");
    ImGui::EndDisabled();
    if (!available) ImGui::TextDisabled("Needs the game's Lua and a known build.");

    if (!console.last) return;
    ImGui::TextDisabled("Run %d: %s", console.runs, console.last->ran ? "done" : "error");
    if (console.last->text.empty()) return;
    luaOutput = console.last->text;
    ImGui::InputTextMultiline("##luaOutput", luaOutput.data(), luaOutput.size() + 1, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 6.0f),
        ImGuiInputTextFlags_ReadOnly);
}
}

void draw()
{
    const auto addon = name();
    if (!addon.empty()) ImGui::TextDisabled("%.*s developer tools", static_cast<int>(addon.size()), addon.data());
    if (!game::isSupportedBuild())
    {
        ImGui::TextWrapped("This is not the game build the devkit knows: its tools cannot run.");
        return;
    }

    drawTravel();
    drawCutscenes();
    drawShaders();
    drawLuaConsole();
}
}
