#pragma once

#include <cstddef>
#include <span>
#include <string_view>

// The Developer page's cutscene player: any cutscene the game's scripts name, from free roam or the
// ACC, after one loading screen. At the main loop's frame start it runs Lua in the game's script
// state (game_lua.hpp): the cutscene's mission starts in a sequence of ours that plays it, and the
// game leaves for the ACC without a mission clear. docs/DEVELOPMENT.md, "Cutscenes".
namespace phantom_fx::cutscenes
{
// One label for each cutscene; play takes the index of one.
std::span<const std::string_view> labels() noexcept;
// Goes to the cutscene's mission at the next frame start. A toast says whether the game went or why
// not, and why a cutscene did not play.
void play(std::size_t index) noexcept;
// The game thread, at the main loop's frame start.
void onFrameStart() noexcept;
}
