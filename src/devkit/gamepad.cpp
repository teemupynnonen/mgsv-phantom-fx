#include "devkit/gamepad.hpp"

#include "devkit/log.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <Xinput.h>
#include <atomic>
#include <charconv>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>

namespace devkit::gamepad
{
namespace
{
using GetState = DWORD(WINAPI*)(DWORD index, XINPUT_STATE* state);

std::filesystem::path trigger;
std::uint64_t triggerSeen{}; // GetTickCount64 of the look that read it
std::atomic<std::uint16_t> pressing{};
std::atomic<int> pollsLeft{};
GetState original{};
GetState originalEx{};
void* hookedAt[8]{};
void* hookedExAt[8]{};
int hooked{};
int hookedEx{};

void consumeTrigger()
{
    // A look every quarter second is enough for a screen that waits for a key.
    const auto now = GetTickCount64();
    if (trigger.empty() || now - triggerSeen < 250) return;

    triggerSeen = now;
    std::error_code error;
    if (!std::filesystem::exists(trigger, error)) return;

    std::ifstream file(trigger);
    std::string name;
    file >> name;
    file.close();
    std::filesystem::remove(trigger, error);

    if (const auto button = buttonOf(name)) press(*button, 10);
}

DWORD WINAPI getState(DWORD index, XINPUT_STATE* state)
{
    static bool said{};
    if (!said) { said = true; log("Gamepad: the game reads the pad."); }
    consumeTrigger();

    DWORD result = original ? original(index, state) : ERROR_DEVICE_NOT_CONNECTED;
    if (index != 0 || !state) return result;

    const auto left = pollsLeft.load(std::memory_order_relaxed);
    if (left <= 0) return result;

    // A copy with no controller of its own still has one for this press.
    if (result != ERROR_SUCCESS)
    {
        std::memset(state, 0, sizeof(*state));
        result = ERROR_SUCCESS;
    }

    state->Gamepad.wButtons |= pressing.load(std::memory_order_relaxed);
    pollsLeft.store(left - 1, std::memory_order_relaxed);

    return result;
}

DWORD WINAPI getStateEx(DWORD index, XINPUT_STATE* state)
{
    static bool said{};
    if (!said) { said = true; log("Gamepad: the game reads the extended pad state."); }
    consumeTrigger();

    DWORD result = originalEx ? originalEx(index, state) : ERROR_DEVICE_NOT_CONNECTED;
    if (index != 0 || !state) return result;

    const auto left = pollsLeft.load(std::memory_order_relaxed);
    if (left <= 0) return result;

    if (result != ERROR_SUCCESS)
    {
        std::memset(state, 0, sizeof(*state));
        result = ERROR_SUCCESS;
    }

    state->Gamepad.wButtons |= pressing.load(std::memory_order_relaxed);
    pollsLeft.store(left - 1, std::memory_order_relaxed);

    return result;
}

// Every XInput module the game has loaded: it may call any of them, and 1.3 and 1.4 have their own code.
void hookModule(HMODULE module, const char* name, void* detour, GetState* first, void** targets, int& count)
{
    if (!module || count >= 8) return;

    const auto function = GetProcAddress(module, name);
    if (!function) return;

    void* previous{};
    if (MH_CreateHook(function, detour, &previous) != MH_OK || MH_EnableHook(function) != MH_OK) return;
    if (!*first) *first = reinterpret_cast<GetState>(previous);
    targets[count++] = function;
}
}

std::optional<std::uint16_t> buttonOf(std::string_view name)
{
    constexpr std::pair<std::string_view, std::uint16_t> Names[]{{"a", XINPUT_GAMEPAD_A}, {"b", XINPUT_GAMEPAD_B}, {"x", XINPUT_GAMEPAD_X},
        {"y", XINPUT_GAMEPAD_Y}, {"start", XINPUT_GAMEPAD_START}, {"back", XINPUT_GAMEPAD_BACK}, {"lb", XINPUT_GAMEPAD_LEFT_SHOULDER},
        {"rb", XINPUT_GAMEPAD_RIGHT_SHOULDER}, {"up", XINPUT_GAMEPAD_DPAD_UP}, {"down", XINPUT_GAMEPAD_DPAD_DOWN}, {"left", XINPUT_GAMEPAD_DPAD_LEFT},
        {"right", XINPUT_GAMEPAD_DPAD_RIGHT}};
    for (const auto& [key, mask] : Names)
        if (name == key) return mask;

    if (name.starts_with("0x"))
    {
        std::uint32_t mask{};
        const auto [stop, error] = std::from_chars(name.data() + 2, name.data() + name.size(), mask, 16);
        if (error == std::errc{} && stop == name.data() + name.size() && mask && mask <= 0xffff) return static_cast<std::uint16_t>(mask);
    }

    return std::nullopt;
}

void press(std::uint16_t buttons, int polls) noexcept
{
    pressing.store(buttons, std::memory_order_relaxed);
    pollsLeft.store(polls, std::memory_order_relaxed);
}

void install(const std::filesystem::path& addonDirectory) noexcept
{
    trigger = addonDirectory / "DevKit" / "pad";

    for (const wchar_t* name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
    {
        const auto module = GetModuleHandleW(name);
        hookModule(module, "XInputGetState", &getState, &original, hookedAt, hooked);
        hookModule(module, MAKEINTRESOURCEA(100), &getStateEx, &originalEx, hookedExAt, hookedEx);
    }

    log(std::string("Gamepad: ") + (hooked || hookedEx ? "the pad is hooked for keyless presses." : "XInput has no entry point the hook could take; only keys press."));
}

void uninstall() noexcept
{
    for (int i = 0; i < hooked; ++i)
    {
        MH_DisableHook(hookedAt[i]);
        MH_RemoveHook(hookedAt[i]);
    }
    for (int i = 0; i < hookedEx; ++i)
    {
        MH_DisableHook(hookedExAt[i]);
        MH_RemoveHook(hookedExAt[i]);
    }
    hooked = hookedEx = 0;

    original = nullptr;
    originalEx = nullptr;
    pollsLeft.store(0, std::memory_order_relaxed);
}
}
