// DevKit.addon64: the developer tools as their own ReShade add-on, beside PhantomFX and not inside
// it. It links only devkit and the game's build check, draws its page in ReShade's default ImGui
// style, and hooks the main loop for travel, cutscenes, the Lua console and command files.
#include "devaddon/game_loop.hpp"

#include "devkit/automation.hpp"
#include "devkit/cutscenes.hpp"
#include "devkit/extension.hpp"
#include "devkit/frame_log.hpp"
#include "devkit/gamepad.hpp"
#include "devkit/game.hpp"
#include "devkit/game_lua.hpp"
#include "devkit/host.hpp"
#include "devkit/log.hpp"
#include "devkit/page.hpp"
#include "devkit/save_redirect.hpp"
#include "devkit/travel.hpp"
#include "device_check.hpp"
#include "log.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <d3d11.h>
// Before reshade.hpp, so register_addon also connects the ImGui calls of the overlay to ReShade's.
#include <imgui.h>
#include <reshade.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string.h>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

extern "C" __declspec(dllexport) const char* NAME = "DevKit";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Developer tools for MGSV add-ons: travel, cutscenes, Lua, command files, saves redirection and an extension API, with no other add-on inside.";

namespace
{
using namespace reshade::api;

HMODULE self{};

// The add-on's folder, as ReShade keeps its own files beside it.
std::filesystem::path dataDirectory()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        const auto length = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return std::filesystem::current_path();
        if (length < path.size()) return std::filesystem::path(path.substr(0, length)).parent_path();

        path.resize(path.size() * 2);
    }
}

void writeLog(std::string_view message)
{
    phantom_fx::log(message);
}

void toast(devkit::Tone tone, std::string_view text, std::string_view detail)
{
    const char* kind = "note";
    switch (tone)
    {
    case devkit::Tone::Dim: kind = "note"; break;
    case devkit::Tone::Ok: kind = "ok"; break;
    case devkit::Tone::Warn: kind = "warn"; break;
    case devkit::Tone::Bad: kind = "error"; break;
    }

    std::string line = std::string(kind) + ": " + std::string(text);
    if (!detail.empty()) line += " (" + std::string(detail) + ")";
    writeLog(line);
}

ID3D11DeviceContext* native(command_list* list)
{
    return reinterpret_cast<ID3D11DeviceContext*>(list->get_native());
}

// The command runner's present jobs, and the frame count `wait` counts in.
void onPresent(command_queue* queue, swapchain* swapChain, const rect*, const rect*, uint32_t, const rect*)
{
    devaddon::onPresent();

    auto* list = queue->get_immediate_command_list();
    if (!list) return;

    devkit::automation::onPresent(native(list), reinterpret_cast<ID3D11Resource*>(swapChain->get_current_back_buffer().handle));
}

// Every frame, with the overlay open or closed: F9 is for the add-on that takes it (a debug case, say).
void onReshadePresent(effect_runtime* runtime)
{
    if (runtime->is_key_pressed(VK_F9)) devkit::automation::caseKey();
}

void onOverlay(effect_runtime*)
{
    devkit::page::draw();
}
}

namespace devkit::game
{
bool isSupportedBuild() noexcept
{
    return phantom_fx::isSupportedGameBuild();
}
}

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon, HMODULE reshadeModule)
{
    if (!reshade::register_addon(addon, reshadeModule)) return false;

    self = addon;
    const auto directory = dataDirectory();

    phantom_fx::openLog(directory / "DevKit.log");
    phantom_fx::log("DevKit: the developer tools as their own add-on.");
    phantom_fx::log(phantom_fx::isSupportedGameBuild() ? "Known game build." : "Unknown game build; the tools that need its code cannot run.");
    phantom_fx::log(phantom_fx::passDeviceCheck());

    devkit::setName("DevKit");
    devkit::setLogger(&writeLog);
    devkit::setToast(&toast);

    MH_Initialize();
    devkit::gamepad::install(directory);
    devaddon::defer();
    phantom_fx::log("Waiting for the first frame to register on PhantomFX's frame start.");

    devkit::automation::install(directory);
    devkit::frame_log::install(directory);

    reshade::register_event<reshade::addon_event::present>(onPresent);
    reshade::register_event<reshade::addon_event::reshade_present>(onReshadePresent);
    reshade::register_overlay("DevKit", onOverlay);

    return true;
}

// What another add-on plugs its own command lines, state and F9 into (devkit/extension.hpp).
extern "C" __declspec(dllexport) const DevKitApi* DevKitGetApi()
{
    return devkit::automation::extensionApi();
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon, HMODULE reshadeModule)
{
    devaddon::unhook();
    devkit::gamepad::uninstall();
    devkit::frame_log::uninstall();
    reshade::unregister_overlay("DevKit", onOverlay);
    reshade::unregister_event<reshade::addon_event::reshade_present>(onReshadePresent);
    reshade::unregister_event<reshade::addon_event::present>(onPresent);
    MH_Uninitialize();
    reshade::unregister_addon(addon, reshadeModule);
}
