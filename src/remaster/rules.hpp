#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What the remastering tool saves: which values of the game's materials become which (README.md).
namespace phantom_fx::remaster
{
// A parameter's components, from x on.
using Value = std::vector<float>;

inline constexpr std::uint64_t SelfColorParameter = 0x557B9224571E, SelfColorIntensityParameter = 0xE40A3A2518CF;

// A parameter of a material goes from one value to another. `parameter` is the StrCode64 of its name.
struct Change
{
    std::uint64_t parameter{};
    Value from, to;
};

// The changes to the materials of one name (a StrCode64). Names are not unique, and `from` tells apart
// materials that share one.
struct Rule
{
    std::string label;
    std::uint64_t material{};
    std::vector<Change> changes;
};

// A material's values as the game holds them. A technique packs its parameters into registers:
// `parameters` lists them by the low half of their names, and for each of the registers' components
// `packing` has parameter << 2 | the value's component.
struct PackedMaterial
{
    std::span<const std::uint32_t> parameters;
    std::span<const std::uint8_t> packing;
    std::span<float> registers;
};

enum class Direction : std::uint8_t
{
    Forward,
    Back
};

// The parameter's components the material has, empty when its technique has no such parameter.
Value valueOf(const PackedMaterial& material, std::uint64_t parameter);

// Puts `value` in the components the material has.
void setValue(const PackedMaterial& material, std::uint64_t parameter, const Value& value);

// Whether the material holds all of what the rule replaces going that way: Forward its `from`s, so the
// rule is for it; Back its `to`s, so the rule has been at it.
bool isAwaited(const PackedMaterial& material, const Rule& rule, Direction direction);

// Replaces each `from` with `to`, or the other way, where the rule is awaited: a value the game changes
// passes, and so does a material already done. True when it replaced them.
bool applyRule(const PackedMaterial& material, const Rule& rule, Direction direction);

// Rules by material, then by what they change from: the order rulesOf and the file have.
void sortRules(std::vector<Rule>& rules);

// The rules of a material, in rules that sortRules has ordered.
std::span<const Rule> rulesOf(std::span<const Rule> rules, std::uint64_t material);

// A self-lit material's SelfColor and SelfColorIntensity, as a model file has them, as the rules make
// them, for what reads the file and not the game's materials.
void tuneSelfLight(std::span<const Rule> rules, std::uint64_t material, std::array<float, 4>& color, std::array<float, 4>& intensity);

// A StrCode64 as twelve hex digits, and back: nothing for other text.
std::string hashText(std::uint64_t hash);
std::optional<std::uint64_t> hashOf(std::string_view text);

// A parameter's name where it is known, else its hash as text, and back: nothing for other text.
std::string parameterText(std::uint64_t parameter);
std::optional<std::uint64_t> parameterOf(std::string_view text);

// A technique keeps only the low half of a name: the whole hash where the name is known.
std::uint64_t parameterOfLowHalf(std::uint32_t lowHalf);
}
