#pragma once

#include <optional>
#include <string>

// Inside the remastering tool, for remaster.cpp: what it asks of tool.cpp, which only a dev build has.
namespace phantom_fx::remaster
{
// A dev build's rules come from the checkout, so a save holds across restarts without a build: the text
// of its materials.json, nothing without one.
std::optional<std::string> savedRulesText();

// Hooks what the tool watches; empty when hooked, or why not.
std::string hookTool() noexcept;

// A blink's dark half holds no value a rule knows, so a sweep goes after it ends.
void endBlink() noexcept;

void presentTool() noexcept;
}
