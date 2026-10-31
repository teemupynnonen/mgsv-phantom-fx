#include "remaster/remaster.hpp"

#include "phantom_fx_settings.hpp"
#include "remaster/game_materials.hpp"
#include "remaster/in_force.hpp"
#include "remaster/rule_file.hpp"

#include <Windows.h>
#include <atomic>
#include <format>
#include <string_view>
#include <utility>

namespace phantom_fx::remaster
{
namespace
{
constexpr int MaterialsResource = 1; // materials.rc.in

std::atomic<std::shared_ptr<const std::vector<Rule>>> inForce{std::make_shared<const std::vector<Rule>>()};
std::atomic<bool> isOn{true};
std::atomic<std::uint64_t> met{}, changed{};

std::string_view embeddedRulesText() noexcept
{
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&embeddedRulesText), &self);
    const auto resource = FindResourceA(self, MAKEINTRESOURCEA(MaterialsResource), RT_RCDATA);
    const auto loaded = resource ? LoadResource(self, resource) : nullptr;
    const auto* text = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;

    return text ? std::string_view(text, SizeofResource(self, resource)) : std::string_view{};
}

std::string rulesText()
{
#if PHANTOM_FX_DEV_TOOLS
    if (auto saved = savedRulesText()) return std::move(*saved);
#endif

    return std::string(embeddedRulesText());
}

void onNewMaterial(const game_materials::Material& material) noexcept
{
    met.fetch_add(1, std::memory_order_relaxed);
    if (!isOn.load(std::memory_order_relaxed)) return;

    const auto rules = inForce.load();
    for (const auto& rule : rulesOf(*rules, material.name))
    {
        if (!applyRule(material.packed, rule, Direction::Forward)) continue;

        changed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

void sweep(const std::vector<Rule>& rules, Direction direction)
{
    game_materials::forEachMaterial([&](const game_materials::Material& material) {
        for (const auto& rule : rulesOf(rules, material.name))
            if (applyRule(material.packed, rule, direction)) return;
    });
}
}

std::shared_ptr<const std::vector<Rule>> rulesInForce()
{
    return inForce.load();
}

void putInForce(std::vector<Rule> rules)
{
    inForce.store(std::make_shared<const std::vector<Rule>>(std::move(rules)));
}

void sweep(const Rule& rule, Direction direction)
{
    sweep(std::vector{rule}, direction);
}

Counts counts() noexcept
{
    return {met.load(std::memory_order_relaxed), changed.load(std::memory_order_relaxed)};
}

std::string hook() noexcept
{
    try
    {
        auto parsed = parseRules(rulesText());
        if (!parsed.error.empty()) return "Materials: the rules are not used: " + parsed.error;
        if (parsed.rules.empty() && !DevTools) return "Materials: no rules.";

        const auto count = parsed.rules.size();
        putInForce(std::move(parsed.rules));

        auto failure = game_materials::hookNewMaterials(onNewMaterial);
#if PHANTOM_FX_DEV_TOOLS
        if (failure.empty()) failure = hookTool();
#endif
        if (failure.empty()) return std::format("Materials: {} rules.", count);

        // The rays read the rules too, so without the hook they go as well.
        putInForce({});

        return "Materials: the rules are not used: " + failure;
    }
    catch (const std::exception& error)
    {
        return std::string("Materials: the rules are not used: ") + error.what();
    }
}

void configure(const Configuration& configuration) noexcept
{
    if (isOn.exchange(configuration.enhancements) == configuration.enhancements) return;

    sweep(*inForce.load(), configuration.enhancements ? Direction::Forward : Direction::Back);
}

void tuneFileSelfLight(std::uint64_t material, std::array<float, 4>& color, std::array<float, 4>& intensity)
{
    tuneSelfLight(*inForce.load(), material, color, intensity);
}
}
