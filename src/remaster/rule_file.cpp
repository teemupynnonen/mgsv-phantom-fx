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

Value valueOf(const Json& entry)
{
    auto value = entry.get<Value>();
    if (value.empty() || value.size() > MostComponents) throw std::runtime_error("a value has one to four components");

    return value;
}

Rule ruleOf(const Json& entry)
{
    const auto material = hashOf(entry.at("material").get<std::string>());
    if (!material) throw std::runtime_error("the material is no hash of twelve digits");

    Rule rule{.label = entry.value("label", ""), .material = *material};
    for (const auto& [name, change] : entry.at("parameters").items())
    {
        const auto parameter = parameterOf(name);
        if (!parameter) throw std::runtime_error(std::format("no parameter is called {}", name));

        rule.changes.push_back({.parameter = *parameter, .from = valueOf(change.at("from")), .to = valueOf(change.at("to"))});
        if (rule.changes.back().from.size() != rule.changes.back().to.size()) throw std::runtime_error(std::format("{} changes its number of components", name));
    }

    if (rule.changes.empty()) throw std::runtime_error("it changes nothing");

    return rule;
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
            rules.push_back(ruleOf(materials[i]));
        }
        catch (const std::exception& error)
        {
            return {.error = std::format("Rule {}: {}", i + 1, error.what())};
        }
    }

    sortRules(rules);

    return {.rules = std::move(rules)};
}

std::string writeRules(std::vector<Rule> rules)
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

    return text + (rules.empty() ? "]\n}\n" : "\n  ]\n}\n");
}
}
