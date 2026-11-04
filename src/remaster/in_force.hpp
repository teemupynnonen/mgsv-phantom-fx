#pragma once

#include "remaster/lamps.hpp"
#include "remaster/rules.hpp"

#include <memory>
#include <vector>

// Inside the remastering tool, for tool.cpp: the rules the game's materials are under (remaster.cpp).
namespace phantom_fx::remaster
{
std::shared_ptr<const std::vector<Rule>> rulesInForce();

// For materials made from now on, as sortRules has them.
void putInForce(std::vector<Rule> rules);

std::shared_ptr<const std::vector<LampRule>> lampsInForce();
void putLampsInForce(std::vector<LampRule> lamps);

// Whether the rules are on the materials: not while the enhancements switch (F1) is off.
bool areRulesOn() noexcept;

// Takes a rule on or off the materials already made.
void sweep(const Rule& rule, Direction direction);

// For as long as it is held, the game shows without any rule, materials, lamps and the rays' self-lit light alike; the rules
// stay in force. Nothing while the enhancements switch has them off.
void holdRulesOff(bool isHeld);
bool areRulesHeldOff() noexcept;
}
