#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// The Developer page's travel: free roam in another of the game's three maps after one loading
// screen, without the helicopter and the sortie screens. At the main loop's frame start it runs Lua in
// the game's script state (game_lua.hpp): the mission clear the game's scripts reserve when free roam
// ends, with the player's arrival moved to a fixed spot on foot. docs/DEVELOPMENT.md, "Travel".
namespace phantom_fx::travel
{
enum class Map : std::uint8_t { Afghanistan, Africa, MotherBase, Count };

const char* name(Map map) noexcept;
// Leaves for the map at the next frame start, arriving at `place` in place of its fixed spot. A toast
// says whether the game went or why not.
void go(Map map, std::optional<std::array<float, 3>> place = std::nullopt) noexcept;
// For the tools that send the game to another mission, on the game thread: runs `code`, which can call
// arriveOnFoot and leaveFor (travel.cpp) and returns nothing when the game leaves, or why not. `tool`
// heads the toast that says why not, and `target` goes in the log. True when the game leaves.
bool depart(std::string_view code, std::string_view tool, std::string_view target);
// The game thread, at the main loop's frame start.
void onFrameStart() noexcept;
}
