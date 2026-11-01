#include "remaster/remaster.hpp"

#include "dev_tools.hpp"
#include "phantom_fx_settings.hpp"
#include "remaster/game_materials.hpp"
#include "remaster/in_force.hpp"
#include "remaster/rule_file.hpp"
#include "remaster/tool.hpp"

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
    if constexpr (DevTools)
        if (auto saved = savedRulesText()) return std::move(*saved);

    return std::string(embeddedRulesText());
}

// A material takes the first rule for it.
void applyFirstRule(const game_materials::Material& material, const std::vector<Rule>& rules, Direction direction)
{
    for (const auto& rule : rulesOf(rules, material.name))
        if (applyRule(material.packed, rule, direction)) return;
}

void onNewMaterial(const game_materials::Material& material) noexcept
{
    if (isOn.load(std::memory_order_relaxed)) applyFirstRule(material, *inForce.load(), Direction::Forward);
}

void sweep(const std::vector<Rule>& rules, Direction direction)
{
    game_materials::forEachMaterial([&](const game_materials::Material& material) { applyFirstRule(material, rules, direction); });
}
}

void putInForce(std::vector<Rule> rules)
{
    inForce.store(std::make_shared<const std::vector<Rule>>(std::move(rules)));
}

#if PHANTOM_FX_DEV_TOOLS
std::shared_ptr<const std::vector<Rule>> rulesInForce()
{
    return inForce.load();
}

bool areRulesOn() noexcept
{
    return isOn.load(std::memory_order_relaxed);
}

void sweep(const Rule& rule, Direction direction)
{
    sweep(std::vector{rule}, direction);
}
#endif

std::string hook() noexcept
{
    constexpr std::string_view NotUsed = "Materials: the rules are not used: ";

    try
    {
        auto parsed = parseRules(rulesText());
        if (!parsed.error.empty()) return std::format("{}{}", NotUsed, parsed.error);
        if (parsed.rules.empty() && !DevTools) return "Materials: no rules.";

        const auto count = parsed.rules.size();
        putInForce(std::move(parsed.rules));

        auto failure = game_materials::hookNewMaterials(onNewMaterial);
        if constexpr (DevTools)
            if (failure.empty()) failure = hookTool();

        if (failure.empty()) return std::format("Materials: {} rules.", count);

        // The rays read the rules too, so without the hook they go as well.
        putInForce({});

        return std::format("{}{}", NotUsed, failure);
    }
    catch (const std::exception& error)
    {
        return std::string(NotUsed) + error.what();
    }
}

void configure(const Configuration& configuration) noexcept
{
    const bool isTurnedOn = configuration.areEnhancementsOn;
    if (isOn.exchange(isTurnedOn) == isTurnedOn) return;

    if constexpr (DevTools) settleTool();

    sweep(*inForce.load(), isTurnedOn ? Direction::Forward : Direction::Back);
}

void tuneFileSelfLight(std::uint64_t material, std::array<float, 4>& color, std::array<float, 4>& intensity)
{
    tuneSelfLight(*inForce.load(), material, color, intensity);
}
}
