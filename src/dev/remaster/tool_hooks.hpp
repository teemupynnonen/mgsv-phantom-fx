#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

// What the rules ask of the remastering tool (remaster.cpp), each call behind DevTools (dev_tools.hpp).
namespace phantom_fx::remaster
{
// A dev build's rules come from the checkout, so a save holds across restarts without a build: the text
// of its materials.json, nothing without one.
std::optional<std::string> savedRulesText();

// Hooks what the tool watches and registers the panel; empty when hooked, or why not.
std::string hookTool() noexcept;

// A flash or a compare holds a material away from its values, and a sweep goes by value: they end first.
void settleTool() noexcept;

// A lamp's light draw, for the marks: where it is and its colour. A flash or a compare may change what the rules made
// of its brightness. On the game's render threads.
void noteLamp(std::uint16_t location, const std::array<float, 3>& at, const std::array<float, 3>& color, float& brightness) noexcept;
}
