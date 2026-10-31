#pragma once

#include "remaster/rules.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Inside the remastering tool: the rules the game's materials are under, between remaster.cpp and tool.cpp.
namespace phantom_fx::remaster
{
std::shared_ptr<const std::vector<Rule>> rulesInForce();

// For materials made from now on, as sortRules has them.
void putInForce(std::vector<Rule> rules);

// Takes a rule on or off the materials already made.
void sweep(const Rule& rule, Direction direction);

// How many materials have met the rules since the start, and how many of those a rule changed.
struct Counts
{
    std::uint64_t met{}, changed{};
};
Counts counts() noexcept;

#if PHANTOM_FX_DEV_TOOLS
// A dev build's rules come from the checkout, so a save holds across restarts without a build: the text
// of its materials.json, nothing without one.
std::optional<std::string> savedRulesText();

// Hooks what the tool watches; empty when hooked, or why not.
std::string hookTool() noexcept;
#endif
}
