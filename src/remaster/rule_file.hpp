#pragma once

#include "remaster/lamps.hpp"
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
    std::vector<LampRule> lamps;
    std::string error;
};

ParsedRules parseRules(std::string_view text);

#if PHANTOM_FX_DEV_TOOLS
std::string writeRules(std::vector<Rule> rules, const std::vector<LampRule>& lamps = {});
#endif
}
