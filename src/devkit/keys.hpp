#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

// Keys to the game's own window, for the screens that wait for one: the loading screens' "press space" and the
// likes. Sent as the game's input reads them, from the game thread.
namespace devkit::keys
{
// space, enter, escape, tab, up, down, left, right, e, f, q, or 0x<virtual-key code>.
std::optional<std::uint32_t> keyOf(std::string_view name);
void press(std::string_view name, int times = 1);
}
