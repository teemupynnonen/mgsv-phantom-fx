#include "remaster/remaster.hpp"

#include "dev/remaster/tool_hooks.hpp"
#include "dev_tools.hpp"
#include "game_code.hpp"
#include "game_memory.hpp"
#include "phantom_fx_settings.hpp"
#include "remaster/game_materials.hpp"
#include "remaster/in_force.hpp"
#include "remaster/rule_file.hpp"

#include <Windows.h>
#include <atomic>
#include <format>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace phantom_fx::remaster
{
namespace
{
constexpr int MaterialsResource = 1; // materials.rc.in

// The script variables' block, and where in it vars.locationCode is; its registration's "mov r8, [VarsRva]; add r8, 0x10".
constexpr std::uintptr_t VarsRva = 0x2C16920;
constexpr std::size_t VarLocation = 0x10;
constexpr unsigned char LocationCode[]{0x4C, 0x8B, 0x05, 0xC6, 0xF9, 0x27, 0x02, 0x49, 0x83, 0xC0, 0x10};
const game_code::Expected LocationChecked[]{{0x996F53, LocationCode}};

std::atomic<std::shared_ptr<const std::vector<Rule>>> inForce{std::make_shared<const std::vector<Rule>>()};
std::atomic<bool> isOn{true};
// The tool's "without any rules" hold (dev builds): the rules stay in force but off everything while it lasts.
std::atomic<bool> isHeldOff{};
std::atomic<std::uint64_t> version{};
std::atomic<std::shared_ptr<const std::vector<LampRule>>> lampsInForceNow{std::make_shared<const std::vector<LampRule>>()};

// The map the player is on, nothing where the game's code is not what it was read from.
std::optional<std::uint16_t> currentLocation() noexcept
{
    static const bool isKnown = game_code::matches(LocationChecked);
    if (!isKnown) return std::nullopt;

    const auto vars = game_memory::read<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(game_code::image()) + VarsRva);

    return vars && *vars ? game_memory::read<std::uint16_t>(*vars + VarLocation) : std::nullopt;
}

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

// Whether the rules are on the game now: the enhancements switch (F1) is on and no hold has them off.
bool areRulesApplied() noexcept
{
    return isOn.load(std::memory_order_relaxed) && !isHeldOff.load(std::memory_order_relaxed);
}

void onNewMaterial(const game_materials::Material& material) noexcept
{
    if (areRulesApplied()) applyFirstRule(material, *inForce.load(), Direction::Forward);
}

void sweep(const std::vector<Rule>& rules, Direction direction)
{
    game_materials::forEachMaterial([&](const game_materials::Material& material) { applyFirstRule(material, rules, direction); });
}
}

void putInForce(std::vector<Rule> rules)
{
    inForce.store(std::make_shared<const std::vector<Rule>>(std::move(rules)));
    ++version;
}

void putLampsInForce(std::vector<LampRule> lamps)
{
    lampsInForceNow.store(std::make_shared<const std::vector<LampRule>>(std::move(lamps)));
}

#if PHANTOM_FX_DEV_TOOLS
std::shared_ptr<const std::vector<LampRule>> lampsInForce()
{
    return lampsInForceNow.load();
}

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

bool areRulesHeldOff() noexcept
{
    return isHeldOff.load(std::memory_order_relaxed);
}

void holdRulesOff(bool isHeld)
{
    const bool wasApplied = areRulesApplied();
    isHeldOff = isHeld;
    if (wasApplied == areRulesApplied()) return;

    sweep(*inForce.load(), wasApplied ? Direction::Back : Direction::Forward);
    ++version; // The rays' self-lit light follows (tuneFileSelfLight)
}
#endif

std::string hook() noexcept
{
    constexpr std::string_view NotUsed = "Materials: the rules are not used: ";

    try
    {
        auto parsed = parseRules(rulesText());
        if (!parsed.error.empty()) return std::format("{}{}", NotUsed, parsed.error);
        if (parsed.rules.empty() && parsed.lamps.empty() && !DevTools) return "Materials: no rules.";

        const auto count = parsed.rules.size(), lamps = parsed.lamps.size();
        putInForce(std::move(parsed.rules));
        putLampsInForce(std::move(parsed.lamps));

        auto failure = game_materials::hookNewMaterials(onNewMaterial);
        if constexpr (DevTools)
            if (failure.empty()) failure = hookTool();

        if (failure.empty()) return std::format("Materials: {} rules, {} lamps.", count, lamps);

        // The rays read the rules too, so without the hook they go as well.
        putInForce({});
        putLampsInForce({});

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
    if (isOn.load() == isTurnedOn) return;

    // A flash, a compare or a hold ends first, putting back what it held, while the rules are still as they were.
    if constexpr (DevTools) settleTool();

    const bool wasApplied = areRulesApplied();
    isOn = isTurnedOn;
    if (wasApplied != areRulesApplied()) sweep(*inForce.load(), wasApplied ? Direction::Back : Direction::Forward);
}

std::uint64_t rulesVersion() noexcept
{
    return version.load();
}

float lampBrightness(const std::array<float, 3>& at, const std::array<float, 3>& color) noexcept
{
    const auto location = currentLocation();
    float brightness = 1;
    if (location && areRulesApplied())
        if (const auto* rule = lampRuleAt(*lampsInForceNow.load(), *location, at)) brightness = rule->brightness;

    if constexpr (DevTools) noteLamp(location.value_or(0), at, color, brightness);

    return brightness;
}

void tuneFileSelfLight(std::uint64_t material, std::array<float, 4>& color, std::array<float, 4>& intensity)
{
    if (areRulesApplied()) tuneSelfLight(*inForce.load(), material, color, intensity);
}
}
