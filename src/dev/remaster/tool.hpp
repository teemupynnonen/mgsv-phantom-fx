#pragma once

#include "remaster/game_materials.hpp"
#include "dev/remaster/marks.hpp"
#include "dev/remaster/tool_hooks.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Inside the remastering tool, which only a dev build has: its state and actions in tool.cpp, for the panel that draws
// them (panel.cpp).
namespace phantom_fx::remaster
{
// The tool's line on the Developer page, saying how to reach it.
void drawTool();

void registerPanel() noexcept;

// A material of a mark or a rule, as a row of the panel's lists.
struct MaterialRow
{
    std::uint64_t name{};
    MaterialKind kind{};
    std::optional<std::uint32_t> poolRow; // Nothing while the game has not loaded it
    float brightness{};
    std::array<float, 3> color{};
    std::int32_t uses{};
    std::string label; // Its rule's
    bool isTuned{};
};

// Each frame the panel is open: what the game has loaded now.
void refreshLoaded();

void addMark();
const std::vector<Mark>& marks();
void removeMark(std::size_t index);
std::size_t tunedCount(const Mark& mark);
std::size_t countOf(const Mark& mark, MaterialKind kind);

// Back to where and when a mark was, by travel if it is another map.
bool canGoTo(const Mark& mark);
bool isGoing() noexcept;
void goTo(const Mark& mark);
// Once a present.
void followTravel();

// The marked materials of a kind, all of them without one, brightest first.
std::vector<MaterialRow> rowsOf(const Mark& mark, std::optional<MaterialKind> kind);

// A rule, with the material it is on when one is loaded here.
struct RuleRow
{
    Rule rule;
    MaterialRow material;
};
std::vector<RuleRow> ruleRows();

void select(std::uint32_t poolRow, std::uint64_t name);
void clearSelection();
std::optional<game_materials::Material> selectedMaterial();

// This frame's flash, drawn as the frame ends, and gone as the next one starts.
void flash(std::uint32_t poolRow);
void endFlash() noexcept;
void startFlash();

Value originalOf(const game_materials::Material& material, std::uint64_t parameter, const Value& value);
void edit(const game_materials::Material& material, std::uint64_t parameter, const Value& value);
std::string labelOf(const game_materials::Material& material);
void setLabel(const game_materials::Material& material, const std::string& label);
void removeRule(const Rule& rule);
void removeRuleOn(const game_materials::Material& material);
bool hasRule(const game_materials::Material& material);

// Shows the game's values of the rule on the material for as long as it is called with true.
void holdOriginal(const game_materials::Material& material, bool isHeld);
bool isComparing() noexcept;

std::size_t unsavedRules();
void saveRules();
// Since the last save that worked.
bool didSaveFail();
}
