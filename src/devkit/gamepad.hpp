#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

// A synthetic gamepad press, without the keyboard: the game polls XInput every frame, so a button merged into what it
// reads reaches it with no focus taken from the user. A loading screen that waits for a key stops the game's command
// files, so the press is asked for through a file instead: `<folder>/pad`, whose contents name a button, is read by the
// hook and removed. `fx.py` writes it; `pad <name>` does the same from a command file while the loop runs.
namespace devkit::gamepad
{
// Hooks XInputGetState (and the extended one a few games use) and watches `<addonDirectory>/DevKit/pad`.
void install(const std::filesystem::path& addonDirectory) noexcept;
void uninstall() noexcept;

// a, b, x, y, start, back, lb, rb, up, down, left, right, or 0x<mask>.
std::optional<std::uint16_t> buttonOf(std::string_view name);
// The button in what the game reads, for its next `polls` calls.
void press(std::uint16_t buttons, int polls = 6) noexcept;
}
