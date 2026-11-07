#include "remaster/rule_file.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <format>
#include <stdexcept>
#include <utility>

namespace phantom_fx::remaster
{
namespace
{
// Floats, so that a value reads back as the very number the game has.
using Json = nlohmann::basic_json<nlohmann::ordered_map, std::vector, std::string, bool, std::int64_t, std::uint64_t, float>;

constexpr std::size_t MostComponents = 4;

Value parsedValue(const Json& entry)
{
    auto value = entry.get<Value>();
    if (value.empty() || value.size() > MostComponents) throw std::runtime_error("a value has one to four components");

    return value;
}

Rule parsedRule(const Json& entry)
{
    const auto material = hashOf(entry.at("material").get<std::string>());
    if (!material) throw std::runtime_error("the material is no hash of twelve digits");

    Rule rule{.label = entry.value("label", ""), .material = *material};
    for (const auto& [name, change] : entry.at("parameters").items())
    {
        const auto parameter = parameterOf(name);
        if (!parameter) throw std::runtime_error(std::format("no parameter is called {}", name));

        rule.changes.push_back({.parameter = *parameter, .from = parsedValue(change.at("from")), .to = parsedValue(change.at("to"))});
        if (rule.changes.back().from.size() != rule.changes.back().to.size()) throw std::runtime_error(std::format("{} changes its number of components", name));
    }

    if (rule.changes.empty()) throw std::runtime_error("it changes nothing");

    return rule;
}

LampRule parsedLamp(const Json& entry)
{
    const auto at = entry.at("at").get<std::vector<float>>();
    if (at.size() != 3) throw std::runtime_error("a place has three components");

    const auto location = entry.at("location").get<std::int64_t>();
    if (location < 0 || location > 0xFFFF) throw std::runtime_error("the location is out of range");

    const auto brightness = entry.at("brightness").get<float>();
    if (!(brightness >= 0)) throw std::runtime_error("the brightness is below zero");

    return {.label = entry.value("label", ""), .location = static_cast<std::uint16_t>(location), .at = {at[0], at[1], at[2]}, .brightness = brightness};
}
}

ParsedRules parseRules(std::string_view text)
{
    const auto file = Json::parse(text, nullptr, false);
    if (file.is_discarded()) return {.error = "The file is not JSON."};
    if (!file.is_object() || !file.contains("materials") || !file["materials"].is_array()) return {.error = "The file has no list of materials."};

    std::vector<Rule> rules;
    const auto& materials = file["materials"];
    for (std::size_t i = 0; i < materials.size(); ++i)
    {
        try
        {
            rules.push_back(parsedRule(materials[i]));
        }
        catch (const std::exception& error)
        {
            return {.error = std::format("Rule {}: {}", i + 1, error.what())};
        }
    }

    sortRules(rules);

    std::vector<LampRule> lamps;
    if (file.contains("lamps"))
    {
        const auto& list = file["lamps"];
        if (!list.is_array()) return {.error = "The lamps are no list."};

        for (std::size_t i = 0; i < list.size(); ++i)
        {
            try
            {
                lamps.push_back(parsedLamp(list[i]));
            }
            catch (const std::exception& error)
            {
                return {.error = std::format("Lamp {}: {}", i + 1, error.what())};
            }
        }
    }

    return {.rules = std::move(rules), .lamps = std::move(lamps)};
}

#if PHANTOM_FX_DEV_TOOLS
std::string writeRules(std::vector<Rule> rules, const std::vector<LampRule>& lamps)
{
    sortRules(rules);

    const auto dump = [](const Json& json) { return json.dump(-1, ' ', false, Json::error_handler_t::replace); };

    // By hand, for one line a parameter: a dump has a line for each component.
    std::string text = "{\n  \"materials\": [";
    for (std::size_t i = 0; i < rules.size(); ++i)
    {
        const auto& rule = rules[i];
        text += std::format("{}\n    {{\n      \"label\": {},\n      \"material\": \"{}\",\n      \"parameters\": {{", i ? "," : "", dump(rule.label), hashText(rule.material));
        for (std::size_t c = 0; c < rule.changes.size(); ++c)
        {
            const auto& change = rule.changes[c];
            text += std::format("{}\n        \"{}\": {{ \"from\": {}, \"to\": {} }}", c ? "," : "", parameterText(change.parameter), dump(change.from), dump(change.to));
        }

        text += "\n      }\n    }";
    }

    text += rules.empty() ? "]" : "\n  ]";
    if (lamps.empty()) return text + "\n}\n";

    text += ",\n  \"lamps\": [";
    for (std::size_t i = 0; i < lamps.size(); ++i)
    {
        const auto& lamp = lamps[i];
        text += std::format("{}\n    {{ \"label\": {}, \"location\": {}, \"at\": {}, \"brightness\": {} }}", i ? "," : "", dump(lamp.label), lamp.location,
            dump(lamp.at), dump(lamp.brightness));
    }

    return text + "\n  ]\n}\n";
}
#endif
}
