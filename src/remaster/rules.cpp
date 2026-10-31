#include "remaster/rules.hpp"

#include <algorithm>
#include <charconv>
#include <format>

namespace phantom_fx::remaster
{
namespace
{
struct KnownParameter
{
    std::string_view name;
    std::uint64_t hash;
};

// The names found so far; the game's files keep only the hashes.
constexpr KnownParameter KnownParameters[]{
    {"SelfColor", 0x557B9224571E},
    {"SelfColorIntensity", 0xE40A3A2518CF},
    {"GlassColor", 0x0BE61C83A81D},
    {"GlassRoughness", 0xCD7F1E1D4699},
    {"ReflectionColor", 0x279B21D6EEA8},
    {"Incidence_Color", 0xCF54D1661DF2},
    {"URepeat_UV", 0xE544FE2CA129},
    {"VRepeat_UV", 0xE7C7FA3697B2},
    {"HeightScale", 0xB0D2BCCEC16B},
    {"StartFadeDot", 0x1D2DB7CCF131},
    {"EndFadeDot", 0xADE052CD23C9},
    {"MatParamIndex_0", 0xCAB8E38487FF},
    {"MatParamIndex_1", 0x7BBD89662077},
    {"MatParamIndex_2", 0xE715065F2F0E},
    {"MatParamIndex_3", 0xC9E199A419A6},
};

// Calls visit(the register's component, the value's component) for each component of the parameter
// that the material holds.
template<class Visit> void forEachComponent(const PackedMaterial& material, std::uint64_t parameter, Visit visit)
{
    const auto found = std::ranges::find(material.parameters, static_cast<std::uint32_t>(parameter));
    if (found == material.parameters.end()) return;

    const auto index = static_cast<std::size_t>(found - material.parameters.begin());
    const auto count = std::min(material.packing.size(), material.registers.size());
    for (std::size_t i = 0; i < count; ++i)
        if (material.packing[i] >> 2 == index) visit(material.registers[i], std::size_t{material.packing[i]} & 3);
}

// Whether the material has `value`, in the components it holds and `value` gives.
bool holds(const PackedMaterial& material, std::uint64_t parameter, const Value& value)
{
    bool isCompared{}, isSame{true};
    forEachComponent(material, parameter, [&](float held, std::size_t component) {
        if (component >= value.size()) return;

        isCompared = true;
        isSame = isSame && held == value[component];
    });

    return isCompared && isSame;
}

// The same for a vector as a model file has it: all four components.
bool holds(const std::array<float, 4>& vector, const Value& value)
{
    return std::equal(value.begin(), value.end(), vector.begin());
}
}

Value valueOf(const PackedMaterial& material, std::uint64_t parameter)
{
    Value value;
    forEachComponent(material, parameter, [&](float held, std::size_t component) {
        if (value.size() <= component) value.resize(component + 1);

        value[component] = held;
    });

    return value;
}

void setValue(const PackedMaterial& material, std::uint64_t parameter, const Value& value)
{
    forEachComponent(material, parameter, [&](float& held, std::size_t component) {
        if (component < value.size()) held = value[component];
    });
}

bool isAwaited(const PackedMaterial& material, const Rule& rule, Direction direction)
{
    const auto isHeld = [&](const Change& change) { return holds(material, change.parameter, direction == Direction::Forward ? change.from : change.to); };

    return !rule.changes.empty() && std::ranges::all_of(rule.changes, isHeld);
}

bool applyRule(const PackedMaterial& material, const Rule& rule, Direction direction)
{
    if (!isAwaited(material, rule, direction)) return false;

    for (const auto& change : rule.changes) setValue(material, change.parameter, direction == Direction::Forward ? change.to : change.from);

    return true;
}

void sortRules(std::vector<Rule>& rules)
{
    for (auto& rule : rules) std::ranges::sort(rule.changes, {}, [](const Change& change) { return parameterText(change.parameter); });

    std::ranges::sort(rules, [](const Rule& a, const Rule& b) {
        if (a.material != b.material) return a.material < b.material;

        return std::ranges::lexicographical_compare(a.changes, b.changes, {}, &Change::from, &Change::from);
    });
}

std::span<const Rule> rulesOf(std::span<const Rule> rules, std::uint64_t material)
{
    const auto found = std::ranges::equal_range(rules, material, {}, &Rule::material);

    return {found.begin(), found.end()};
}

void tuneSelfLight(std::span<const Rule> rules, std::uint64_t material, std::array<float, 4>& color, std::array<float, 4>& intensity)
{
    const auto vectorOf = [&](std::uint64_t parameter) -> std::array<float, 4>* {
        if (parameter == SelfColorParameter) return &color;

        return parameter == SelfColorIntensityParameter ? &intensity : nullptr;
    };

    // The rule's other parameters are not in the file's reading, so only these two tell lamps apart.
    for (const auto& rule : rulesOf(rules, material))
    {
        const auto isTheRules = [&](const Change& change) {
            const auto* vector = vectorOf(change.parameter);
            return !vector || holds(*vector, change.from);
        };
        const auto isLight = [&](const Change& change) { return vectorOf(change.parameter) != nullptr; };
        if (!std::ranges::any_of(rule.changes, isLight) || !std::ranges::all_of(rule.changes, isTheRules)) continue;

        for (const auto& change : rule.changes)
            if (auto* vector = vectorOf(change.parameter)) std::ranges::copy(change.to, vector->begin());

        return;
    }
}

std::string hashText(std::uint64_t hash)
{
    return std::format("{:012x}", hash);
}

std::optional<std::uint64_t> hashOf(std::string_view text)
{
    std::uint64_t hash{};
    const auto* end = text.data() + text.size();
    const auto [at, error] = std::from_chars(text.data(), end, hash, 16);
    if (text.size() != 12 || error != std::errc{} || at != end) return std::nullopt;

    return hash;
}

std::string parameterText(std::uint64_t parameter)
{
    const auto known = std::ranges::find(KnownParameters, parameter, &KnownParameter::hash);

    return known != std::end(KnownParameters) ? std::string(known->name) : hashText(parameter);
}

std::optional<std::uint64_t> parameterOf(std::string_view text)
{
    const auto known = std::ranges::find(KnownParameters, text, &KnownParameter::name);

    return known != std::end(KnownParameters) ? known->hash : hashOf(text);
}

std::uint64_t parameterOfLowHalf(std::uint32_t lowHalf)
{
    const auto known = std::ranges::find(KnownParameters, lowHalf, [](const KnownParameter& parameter) { return static_cast<std::uint32_t>(parameter.hash); });

    return known != std::end(KnownParameters) ? known->hash : lowHalf;
}
}
