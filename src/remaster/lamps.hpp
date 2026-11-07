#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

// The game's point and spot lights the rules tune (README.md, "Lamps"). The game has no name for a lamp, so a lamp is where
// it stands on a map, as its light draw puts it, and a rule scales its light.
namespace phantom_fx::remaster
{
struct LampRule
{
    std::string label;
    std::uint16_t location{}; // vars.locationCode
    std::array<float, 3> at{};
    float brightness{1};

    bool operator==(const LampRule&) const = default;
};

// Whether a lamp at `at` is the one at `place`.
bool isLampAt(const std::array<float, 3>& place, const std::array<float, 3>& at);

// The rule for the lamp at `at` on `location`, null for none. Lamps stand where the game's files put them, so a lamp within
// a few centimetres of a rule's place is its lamp.
const LampRule* lampRuleAt(std::span<const LampRule> rules, std::uint16_t location, const std::array<float, 3>& at);
}
