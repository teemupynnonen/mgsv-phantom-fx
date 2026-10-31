#pragma once

#include "remaster/rules.hpp"

#include <string>
#include <string_view>
#include <vector>

// The rules as text, for materials.json.
namespace phantom_fx::remaster
{
// The rules, or none and why.
struct ParsedRules
{
    std::vector<Rule> rules;
    std::string error;
};

ParsedRules parseRules(std::string_view text);

#if PHANTOM_FX_DEV_TOOLS
std::string writeRules(std::vector<Rule> rules);
#endif
}
