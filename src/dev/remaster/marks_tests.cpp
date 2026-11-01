#include "dev/remaster/marks.hpp"

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

const Mark HelipadMark{.location = 50,
    .mission = 30050,
    .position = {8.815599f, 1.500121f, -47.530197f},
    .rotation = 0.25f,
    .clock = 79200,
    .materials = {{.name = 0x50FFD14C26F0, .fingerprint = 0x1234567890ABCDEF, .kind = MaterialKind::SelfLit, .brightness = 30000, .uses = 4},
        {.name = 0x0BE61C83A81D, .fingerprint = 0xFFFFFFFFFFFFFFFF, .kind = MaterialKind::Glass, .brightness = 0, .uses = 311}}};

void readsBackItsMarks()
{
    const auto parsed = parseMarks(writeMarks({HelipadMark, {.location = 10, .mission = 10070}}));

    expect(parsed.error.empty(), "Written marks read.");
    expect(parsed.marks == std::vector<Mark>{HelipadMark, {.location = 10, .mission = 10070}}, "Marks read back as they were, to the bit.");
    expect(parseMarks(writeMarks({})).marks.empty() && parseMarks(writeMarks({})).error.empty(), "No marks is a good file.");
}

// A file left by an older build or a bad edit costs the marks, never the game.
void refusesABrokenMarksFile()
{
    constexpr const char* Broken[]{
        "not json",
        "[]",
        R"({ "marks": {} })",
        R"({ "marks": [{ "location": 50 }] })",
        R"({ "marks": [{ "location": 50, "mission": 30050, "position": [1, 2], "rotation": 0, "clock": 0, "materials": [] }] })",
        R"({ "marks": [{ "location": 50, "mission": 30050, "position": [1, 2, 3], "rotation": 0, "clock": 0, "materials": [{ "name": "zz" }] }] })",
        R"({ "marks": [{ "location": 70000, "mission": 30050, "position": [1, 2, 3], "rotation": 0, "clock": 0, "materials": [] }] })",
    };

    for (const auto* text : Broken)
    {
        const auto parsed = parseMarks(text);
        expect(parsed.marks.empty() && !parsed.error.empty(), text);
    }
}

void saysWhereAMarkIs()
{
    expect(placeText(HelipadMark) == "Mother Base, at 9, 2, -48", "A mark's place is its map and position in whole metres.");
    expect(placeText({.location = 10}) == "Afghanistan, at 0, 0, 0", "Afghanistan is location 10.");
    expect(placeText({.location = 20}) == "Africa, at 0, 0, 0", "Africa is location 20.");
    expect(placeText({.location = 40}) == "Location 40, at 0, 0, 0", "A location without a name shows its number.");
}

void saysWhenAMarkWas()
{
    expect(timeText(79200) == "22:00", "Ten at night.");
    expect(timeText(0) == "00:00", "Midnight.");
    expect(timeText(3 * 3600 + 7 * 60 + 59) == "03:07", "Seconds do not show.");
    expect(timeText(86400) == "00:00", "The day wraps.");
    expect(timeText(1e10f) == "17:46", "A clock far past a day still reads as a time of day.");
    expect(timeText(-60) == "23:59", "A clock before midnight reads as the day before.");
}

// A mark taken before tuning must find its material after it, and the rule must not join it to another.
void aMaterialStaysItselfWhenTuned()
{
    const std::vector rules{DimLamp};

    LampMaterial lamp;
    const auto before = fingerprintOf(lamp.packed(), DimLamp.material, rules);
    applyRule(lamp.packed(), DimLamp, Direction::Forward);
    expect(fingerprintOf(lamp.packed(), DimLamp.material, rules) == before, "Tuning does not change which material it is.");
    expect(lamp.registers[4] == 40000, "Asking does not touch the game's material.");

    LampMaterial other;
    other.registers[4] = 100000;
    expect(fingerprintOf(other.packed(), DimLamp.material, rules) != before, "Another value of the name is another material.");

    LampMaterial sameValues;
    expect(fingerprintOf(sameValues.packed(), 0x123, rules) != before, "Another name is another material.");
}
}

int main()
{
    try
    {
        readsBackItsMarks();
        refusesABrokenMarksFile();
        saysWhereAMarkIs();
        saysWhenAMarkWas();
        aMaterialStaysItselfWhenTuned();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
