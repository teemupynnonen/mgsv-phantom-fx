#pragma once

#include "remaster/game_materials.hpp"
#include "remaster/lamps.hpp"
#include "dev/remaster/marks.hpp"
#include "dev/remaster/tool_hooks.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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
    Rule rule; // In force, or the file's when it was removed
    std::optional<Rule> saved;
    MaterialRow material;
    RuleState state{};
};
// The rules in force and the file's removed ones.
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
// The game without any rule, for as long as any holder holds it; editing waits meanwhile (isComparing).
enum class Holder : std::uint8_t
{
    Panel, // V
    Line   // Automation's `without`
};
void holdWithoutRules(Holder holder, bool isHeld);
bool isComparing() noexcept;

// A lamp of a mark: its rule's brightness, 1 without one.
struct LampRow
{
    std::uint16_t location{};
    std::array<float, 3> at{}, color{};
    float brightness{1};
    std::string label;
    bool isTuned{};
    bool isDrawn{}; // The game draws it now
};

// The marked lamps, brightest first.
std::vector<LampRow> lampRowsOf(const Mark& mark);
// For automation (runLine): the panel open on a mark's lamps, with that lamp chosen.
void showLamp(std::size_t mark, const LampRow& lamp);
struct LampRuleRow
{
    LampRule rule; // In force, or the file's when it was removed
    std::optional<LampRule> saved;
    RuleState state{};
};
// The lamp rules in force and the file's removed ones.
std::vector<LampRuleRow> lampRuleRows();

void selectLamp(const LampRow& lamp);
std::optional<LampRow> selectedLamp();
void flashLamp(const std::array<float, 3>& at);
// The lamp's light times `brightness`, under a rule on the lamp; 1 with no label takes the rule away.
void editLamp(const LampRow& lamp, float brightness);
void setLampLabel(const LampRow& lamp, const std::string& label);
void removeLampRule(const LampRule& rule);
// Its rule taken away, whatever it was.
void resetLamp(const LampRow& lamp);
void holdOriginalLamp(const LampRow& lamp, bool isHeld);

// Every change to the rules, materials' and lamps' alike, is a step back. False when there was none.
bool undo();
bool redo();
// Counts the undos and redos done, by any hand, so what shows a rule knows to read it again.
std::uint64_t restores();

// The rules still under the label the tool gave them: a reader of the file cannot tell what they are for.
std::size_t defaultNamedRules();

std::size_t unsavedRules();
void saveRules();
// One rule, or every one, back as the file has it: a new rule goes, a removed one comes back. Each is a step to undo.
void revertRule(const RuleRow& row);
void revertLampRule(const LampRuleRow& row);
void discardUnsaved();
// Since the last save that worked.
bool didSaveFail();

// An automation line (automation.hpp, `remaster`), on the render thread as the panel: what it did, as text.
//   mark                                 a mark here, as F6
//   loaded                               every self-lit material loaded and every material drawn now, with its parameters
//   rows [self-lit|glass|all]            the last mark's materials of that kind
//   set <material> <parameter> <value>   tunes the material (the last mark's, else any loaded of the name), as the panel's controls do
//   original <material>                  takes its rule away
//   lamps                                the last mark's lamps, numbered
//   lamp <number> <brightness>           tunes that lamp, as the panel's control does; 1 takes its rule away
//   compare <number> 0|1                 holds that lamp at its original, as the panel's Compare does
//   panel <number>                       opens the panel on the last mark's lamps, that lamp chosen
//   reset <number>                       takes that lamp's rule away, as the panel's R does
//   undo, redo                           as Ctrl+Z and Ctrl+Y
//   without 0|1                          holds the game without any rule, as the panel's V does
//   rules                                every rule's label and state against the file, and how many are still under the one the tool gave
//   discard                              every rule back as the file has it, as the panel's Discard
std::string runLine(std::string_view line);
}
