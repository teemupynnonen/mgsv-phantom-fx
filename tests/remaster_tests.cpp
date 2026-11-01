#include "remaster/rule_file.hpp"
#include "remaster/rules.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace phantom_fx::remaster;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint64_t SelfColor = 0x557B9224571E, SelfColorIntensity = 0xE40A3A2518CF;

// The rule file reads as written by hand: a parameter by its name, a material by its hash.
void readsARule()
{
    const auto parsed = parseRules(R"({
        "materials": [{
            "label": "Corridor ceiling lamp",
            "material": "e45333b9516f",
            "parameters": { "SelfColorIntensity": { "from": [221182], "to": [40000] } }
        }]
    })");

    expect(parsed.error.empty(), "A good file reads without an error.");
    expect(parsed.rules.size() == 1, "The file holds one rule.");

    const auto& rule = parsed.rules[0];
    expect(rule.label == "Corridor ceiling lamp", "The rule keeps its label.");
    expect(rule.material == 0xE45333B9516F, "The material is its hash.");
    expect(rule.changes.size() == 1 && rule.changes[0].parameter == SelfColorIntensity, "A parameter's name is its StrCode64.");
    expect(rule.changes[0].from == Value{221182} && rule.changes[0].to == Value{40000}, "The change keeps its values.");
}

// Most parameters' names are unknown: those go by their hash.
void readsAParameterByItsHash()
{
    const auto parsed = parseRules(R"({
        "materials": [{ "material": "ca117249330f", "parameters": { "8a68b3c232ee": { "from": [1, 2], "to": [3, 4] } } }]
    })");

    expect(parsed.error.empty(), "A hash is as good as a name.");
    expect(parsed.rules[0].changes[0].parameter == 0x8A68B3C232EE, "The hash is the parameter.");
    expect(parsed.rules[0].label.empty(), "A label may be left out.");
}

// A hand edit gone wrong gives a reason and no rules, whatever is wrong with the file.
void refusesABrokenFile()
{
    constexpr const char* Broken[]{
        "not json",
        "[]",
        R"({ "rules": [] })",
        R"({ "materials": {} })",
        R"({ "materials": [{ "parameters": {} }] })",
        R"({ "materials": [{ "material": "zz", "parameters": { "SelfColor": { "from": [1], "to": [2] } } }] })",
        R"({ "materials": [{ "material": "e45333b9516f" }] })",
        R"({ "materials": [{ "material": "e45333b9516f", "parameters": {} }] })",
        R"({ "materials": [{ "material": "e45333b9516f", "parameters": { "NoSuchName": { "from": [1], "to": [2] } } }] })",
        R"({ "materials": [{ "material": "e45333b9516f", "parameters": { "SelfColor": { "from": [1] } } }] })",
        R"({ "materials": [{ "material": "e45333b9516f", "parameters": { "SelfColor": { "from": [1, 2], "to": [2] } } }] })",
        R"({ "materials": [{ "material": "e45333b9516f", "parameters": { "SelfColor": { "from": [], "to": [] } } }] })",
        R"({ "materials": [{ "material": "e45333b9516f", "parameters": { "SelfColor": { "from": [1, 2, 3, 4, 5], "to": [1, 2, 3, 4, 5] } } }] })",
        R"({ "materials": [{ "material": "e45333b9516f", "parameters": { "SelfColor": { "from": ["a"], "to": [2] } } }] })",
        R"({ "materials": [{ "material": "e45333b9516f", "parameters": { "SelfColor": { "from": [1], "to": [1e39] } } }] })",
        R"({ "materials": [{ "material": "e45333b9516f", "parameters": { "SelfColor": { "from": [1], "to": [-1e40] } } }] })",
    };

    for (const auto* text : Broken)
    {
        const auto parsed = parseRules(text);
        expect(parsed.rules.empty() && !parsed.error.empty(), text);
    }
}

// The file a save writes, as a reviewer sees it in a diff.
void writesTheFile()
{
    const auto text = writeRules({{.label = "Corridor \"ceiling\" lamp", .material = 0xE45333B9516F,
        .changes = {{.parameter = SelfColorIntensity, .from = {221182}, .to = {40000}}, {.parameter = 0x8A68B3C232EE, .from = {1, 2}, .to = {0.5f, 2}}}}});

    expect(text == R"({
  "materials": [
    {
      "label": "Corridor \"ceiling\" lamp",
      "material": "e45333b9516f",
      "parameters": {
        "8a68b3c232ee": { "from": [1.0,2.0], "to": [0.5,2.0] },
        "SelfColorIntensity": { "from": [221182.0], "to": [40000.0] }
      }
    }
  ]
}
)", "The file lays out one rule a block and one parameter a line.");
}

// No edit order shows in the file, so a save changes only the lines that changed.
void writesTheSameFileInAnyOrder()
{
    const Rule first{.material = 1, .changes = {{.parameter = SelfColor, .from = {1}, .to = {2}}}};
    const Rule second{.material = 1, .changes = {{.parameter = SelfColor, .from = {3}, .to = {4}}}};
    const Rule third{.material = 2, .changes = {{.parameter = SelfColor, .from = {1}, .to = {2}}}};

    expect(writeRules({first, second, third}) == writeRules({third, second, first}), "The order of the rules does not show.");

    const Rule color{.material = 1, .changes = {{.parameter = SelfColor, .from = {1}, .to = {2}}}};
    const Rule intensity{.material = 1, .changes = {{.parameter = SelfColorIntensity, .from = {1}, .to = {2}}}};
    expect(writeRules({color, intensity}) == writeRules({intensity, color}), "Nor when two rules change different parameters from one value.");
}

// What a save wrote reads back to the bit: a rule's `from` must equal what the game holds.
void readsBackWhatItWrote()
{
    const Value odd{0.1f, 0.16238699853420258f, 1e-7f, 341182};
    const auto parsed = parseRules(writeRules({{.label = "Lamp", .material = 0xCA117249330F, .changes = {{.parameter = SelfColor, .from = odd, .to = odd}}}}));

    expect(parsed.error.empty() && parsed.rules.size() == 1, "A saved file reads.");
    expect(parsed.rules[0].changes[0].from == odd, "A saved value reads back exactly.");
    expect(parseRules(writeRules({})).error.empty(), "A file without rules reads.");
}

// A self-lit material as the game holds one (measured in the game, 2026-11-01): the technique's parameters by the
// low half of their names; a byte for each register component, parameter << 2 | the value's component;
// SelfColor in register 0, SelfColorIntensity in x of register 1, a third parameter in the rest.
struct LampMaterial
{
    std::array<std::uint32_t, 3> parameters{static_cast<std::uint32_t>(SelfColor), static_cast<std::uint32_t>(SelfColorIntensity), 0x1234};
    std::array<std::uint8_t, 8> packing{0, 1, 2, 3, 1 << 2, 2 << 2, 2 << 2 | 1, 2 << 2 | 2};
    std::array<float, 8> registers{1, 1, 1, 1, 221182, 7, 8, 9};

    PackedMaterial packed() { return {parameters, packing, registers}; }
};

const Rule DimLamp{.material = 0xCA117249330F, .changes = {{.parameter = SelfColorIntensity, .from = {221182}, .to = {40000}}}};

void readsAParameter()
{
    LampMaterial lamp;
    expect(valueOf(lamp.packed(), SelfColor) == Value{1, 1, 1, 1}, "A parameter fills a register.");
    expect(valueOf(lamp.packed(), SelfColorIntensity) == Value{221182}, "A parameter takes part of a register.");
    expect(valueOf(lamp.packed(), 0x8A68B3C232EE).empty(), "A technique without the parameter has no value.");

    lamp.packing = {1, 0, 3, 2, 1 << 2, 2 << 2, 2 << 2 | 1, 2 << 2 | 2};
    lamp.registers = {20, 10, 40, 30, 221182, 7, 8, 9};
    expect(valueOf(lamp.packed(), SelfColor) == Value{10, 20, 30, 40}, "The packing says which component goes where.");
}

void aRuleReplacesItsValue()
{
    LampMaterial lamp;
    expect(applyRule(lamp.packed(), DimLamp, Direction::Forward), "The rule meets its value.");
    expect(lamp.registers == std::array<float, 8>{1, 1, 1, 1, 40000, 7, 8, 9}, "Only the parameter's register component changes.");
}

// ca117249330f names lamps of 221182, 100000 and 600 in the game: the rule is for one of them.
void aRuleLeavesOtherValuesAlone()
{
    LampMaterial other;
    other.registers[4] = 100000;
    expect(!applyRule(other.packed(), DimLamp, Direction::Forward), "Another value of the name is not the rule's.");
    expect(other.registers[4] == 100000, "Another value stays, as the game animating one would.");
}

// The game clones and merges materials again at run time, so a rule meets its own result.
void aRuleAppliesOnce()
{
    LampMaterial lamp;
    applyRule(lamp.packed(), DimLamp, Direction::Forward);
    expect(!applyRule(lamp.packed(), DimLamp, Direction::Forward), "A tuned material is no longer the rule's.");
    expect(lamp.registers[4] == 40000, "Applying again changes nothing.");
}

void aRuleNeedsAllItsValues()
{
    const Rule tinted{.material = 1,
        .changes = {{.parameter = SelfColor, .from = {1, 1, 1, 1}, .to = {1, 0, 0, 1}}, {.parameter = SelfColorIntensity, .from = {600}, .to = {300}}}};

    LampMaterial lamp;
    expect(!applyRule(lamp.packed(), tinted, Direction::Forward), "The intensity differs, so this is another lamp.");
    expect(lamp.registers[1] == 1, "Nothing of the rule applies to another lamp.");
}

void aRuleLeavesATechniqueWithoutItsParameter()
{
    const Rule other{.material = 1, .changes = {{.parameter = 0x8A68B3C232EE, .from = {0}, .to = {1}}}};

    LampMaterial lamp;
    expect(!applyRule(lamp.packed(), other, Direction::Forward), "A technique without the parameter does not match.");
}

void aRuleGoesBack()
{
    LampMaterial lamp;
    applyRule(lamp.packed(), DimLamp, Direction::Forward);
    expect(applyRule(lamp.packed(), DimLamp, Direction::Back), "A tuned material goes back.");
    expect(lamp.registers == LampMaterial{}.registers, "It goes back to what the game gave it.");
    expect(!applyRule(lamp.packed(), DimLamp, Direction::Back), "An untuned material has nothing to go back from.");
}

void retuningStartsAChange()
{
    Rule rule{.material = 1};
    retune(rule, SelfColorIntensity, {221182}, {40000});

    expect(rule.changes.size() == 1 && rule.changes[0].parameter == SelfColorIntensity, "The first move of a slider starts a change.");
    expect(rule.changes[0].from == Value{221182} && rule.changes[0].to == Value{40000}, "It goes from what the material held.");
}

void retuningKeepsWhereItStarted()
{
    Rule rule{.material = 1};
    retune(rule, SelfColorIntensity, {221182}, {40000});
    retune(rule, SelfColorIntensity, {40000}, {20000});

    expect(rule.changes[0].from == Value{221182} && rule.changes[0].to == Value{20000}, "A later move changes only where it goes.");
}

void retuningBackDropsTheChange()
{
    Rule rule{.material = 1};
    retune(rule, SelfColorIntensity, {221182}, {40000});
    retune(rule, SelfColorIntensity, {40000}, {221182});

    expect(rule.changes.empty(), "A value back where it started changes nothing.");
}

// A file's rule may give fewer components than the material holds, and a tuned one must still read back:
// the file refuses a `to` of another size than its `from`, and with it every rule.
void retuningKeepsAHandWrittenChangesSize()
{
    Rule rule{.label = "Lamp", .material = 1, .changes = {{.parameter = SelfColor, .from = {1}, .to = {0.5f}}}};
    retune(rule, SelfColor, {0.5f, 1, 1, 1}, {0.25f, 1, 1, 1});

    expect(rule.changes[0].to == Value{0.25f}, "A hand-written change keeps its components.");
    expect(parseRules(writeRules({rule})).error.empty(), "The tuned rule still reads.");

    Rule longer{.material = 1, .changes = {{.parameter = SelfColorIntensity, .from = {600, 7}, .to = {300, 7}}}};
    retune(longer, SelfColorIntensity, {300}, {150});

    expect(longer.changes[0].to == Value{150, 7}, "What the material does not hold stays as it was.");

    Rule moved{.material = 1, .changes = {{.parameter = SelfColorIntensity, .from = {600, 7}, .to = {300, 9}}}};
    retune(moved, SelfColorIntensity, {300}, {150});

    expect(moved.changes[0].to == Value{150, 9}, "A component the file changed and the material does not hold keeps the file's value.");
}

void findsTheRulesOfAMaterial()
{
    std::vector<Rule> rules{{.material = 2}, {.material = 1}, {.material = 2}};
    sortRules(rules);

    expect(rulesOf(rules, 2).size() == 2, "Both of a material's rules are found.");
    expect(rulesOf(rules, 3).empty(), "A material without rules has none.");
}

// The ray-traced scene reads a model file, not the game's materials, and must see what the game draws.
void tunesASelfLitMaterialAsAFileHasIt()
{
    const std::vector rules{DimLamp};

    std::array<float, 4> color{1, 1, 1, 1}, intensity{221182, 0, 0, 0};
    tuneSelfLight(rules, DimLamp.material, color, intensity);
    expect(intensity == std::array<float, 4>{40000, 0, 0, 0}, "The file's value becomes the rule's.");

    intensity = {100000, 0, 0, 0};
    tuneSelfLight(rules, DimLamp.material, color, intensity);
    expect(intensity[0] == 100000, "Another value of the name stays.");

    intensity = {221182, 0, 0, 0};
    tuneSelfLight(rules, 0x123, color, intensity);
    expect(intensity[0] == 221182, "Another material stays.");
}

void aSelfLitRuleNeedsBothValues()
{
    const std::vector<Rule> rules{{.material = 1,
        .changes = {{.parameter = SelfColor, .from = {1, 1, 1, 1}, .to = {1, 0, 0, 1}}, {.parameter = SelfColorIntensity, .from = {600}, .to = {300}}}}};

    std::array<float, 4> color{1, 1, 1, 1}, intensity{221182, 0, 0, 0};
    tuneSelfLight(rules, 1, color, intensity);
    expect(color[1] == 1, "The same name at another intensity is another lamp.");

    intensity[0] = 600;
    tuneSelfLight(rules, 1, color, intensity);
    expect(color == std::array<float, 4>{1, 0, 0, 1} && intensity[0] == 300, "Both values change together.");
}
}

int main()
{
    try
    {
        readsARule();
        readsAParameterByItsHash();
        refusesABrokenFile();
        writesTheFile();
        writesTheSameFileInAnyOrder();
        readsBackWhatItWrote();
        readsAParameter();
        aRuleReplacesItsValue();
        aRuleLeavesOtherValuesAlone();
        aRuleAppliesOnce();
        aRuleNeedsAllItsValues();
        aRuleLeavesATechniqueWithoutItsParameter();
        aRuleGoesBack();
        retuningStartsAChange();
        retuningKeepsWhereItStarted();
        retuningBackDropsTheChange();
        retuningKeepsAHandWrittenChangesSize();
        findsTheRulesOfAMaterial();
        tunesASelfLitMaterialAsAFileHasIt();
        aSelfLitRuleNeedsBothValues();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
