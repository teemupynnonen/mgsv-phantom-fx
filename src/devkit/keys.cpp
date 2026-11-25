#include "devkit/keys.hpp"

#include <Windows.h>
#include <charconv>
#include <cstdint>
#include <utility>

namespace devkit::keys
{
namespace
{
// The game's own window: the visible top-level one this process owns.
struct Find
{
    DWORD process{};
    HWND found{};
};
BOOL CALLBACK onWindow(HWND window, LPARAM user)
{
    auto& find = *reinterpret_cast<Find*>(user);
    DWORD owner{};
    GetWindowThreadProcessId(window, &owner);
    if (owner != find.process || !IsWindowVisible(window)) return TRUE;

    find.found = window;
    return FALSE;
}

HWND gameWindow()
{
    Find find{GetCurrentProcessId()};
    EnumWindows(&onWindow, reinterpret_cast<LPARAM>(&find));

    return find.found;
}

// The key as the game's input reads it: a scan code through SendInput, which needs the game in front, or the window
// message when it cannot be brought there.
void press(std::uint32_t virtualKey)
{
    const auto game = gameWindow();
    if (!game) return;

    const auto previous = GetForegroundWindow();
    if (previous != game)
    {
        SetForegroundWindow(game);
        // Windows may refuse a process that is not in front; then the message is all there is.
        if (GetForegroundWindow() != game)
        {
            PostMessageW(game, WM_KEYDOWN, virtualKey, 0);
            PostMessageW(game, WM_KEYUP, virtualKey, 0);
            return;
        }
    }

    // The arrows come back from MAPVK_VK_TO_VSC_EX with the extended prefix in the high byte; without it the scan
    // code is the keypad's.
    const auto scan = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC_EX);
    const DWORD extended = (scan & 0xff00) ? KEYEVENTF_EXTENDEDKEY : 0;
    INPUT input[2]{};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = static_cast<WORD>(virtualKey);
    input[0].ki.wScan = static_cast<WORD>(scan & 0xff);
    input[0].ki.dwFlags = KEYEVENTF_SCANCODE | extended;
    input[1] = input[0];
    input[1].ki.dwFlags = input[0].ki.dwFlags | KEYEVENTF_KEYUP;
    SendInput(2, input, sizeof(INPUT));

    if (previous && previous != game && IsWindow(previous)) SetForegroundWindow(previous);
}
}

std::optional<std::uint32_t> keyOf(std::string_view name)
{
    constexpr std::pair<std::string_view, std::uint32_t> Names[]{{"space", VK_SPACE}, {"enter", VK_RETURN}, {"escape", VK_ESCAPE},
        {"tab", VK_TAB}, {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT}, {"e", 'E'}, {"f", 'F'}, {"q", 'Q'}};
    for (const auto& [key, value] : Names)
        if (name == key) return value;

    // A hexadecimal virtual-key code, such as 0x20.
    if (name.starts_with("0x"))
    {
        std::uint32_t value{};
        const auto [stop, error] = std::from_chars(name.data() + 2, name.data() + name.size(), value, 16);
        if (error == std::errc{} && stop == name.data() + name.size() && value && value < 0x100) return value;
    }

    return std::nullopt;
}

void press(std::string_view name, int times)
{
    const auto key = keyOf(name);
    if (!key) return;

    for (int i = 0; i < times; ++i) press(*key);
}
}
