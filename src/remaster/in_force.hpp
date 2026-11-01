#pragma once

#include "remaster/rules.hpp"

#include <memory>
#include <vector>

// Inside the remastering tool, for tool.cpp: the rules the game's materials are under (remaster.cpp).
namespace phantom_fx::remaster
{
std::shared_ptr<const std::vector<Rule>> rulesInForce();

// For materials made from now on, as sortRules has them.
void putInForce(std::vector<Rule> rules);

// Whether the rules are on the materials: not while the enhancements switch (F1) is off.
bool areRulesOn() noexcept;

// Takes a rule on or off the materials already made.
void sweep(const Rule& rule, Direction direction);
}
