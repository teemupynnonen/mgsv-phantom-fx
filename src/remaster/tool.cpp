// The remastering tool on the Developer page; only a dev build has this file (remaster.cmake).
#include "remaster/tool.hpp"

#include "fxui/fxui.hpp"
#include "game_code.hpp"
#include "game_loop.hpp"
#include "remaster/game_materials.hpp"
#include "remaster/in_force.hpp"
#include "remaster/remaster.hpp"
#include "remaster/rule_file.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

namespace phantom_fx::remaster
{
namespace
{
using fxui::Tone;
using game_materials::Material;

constexpr std::uintptr_t ReplayRva = 0x2D6310;
constexpr unsigned char ReplayCode[]{0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30};
const game_code::Expected Checked[]{{ReplayRva, ReplayCode}};

// A draw packet's first word holds the row of its material in the low bits, all ones for none.
constexpr std::uint64_t PacketMaterialBits = 0x7FFF;
// Drawn within this many game frames is drawn now.
constexpr std::uint32_t DrawnNowFrames = 60;
constexpr int FramesBetweenRefreshes = 30;
constexpr ULONGLONG BlinkMilliseconds = 400;
// The page counts as closed after this many presents without it.
constexpr std::uint64_t PresentsUnseen = 2;

// What blinks: the first of these a material has, else its first parameter.
constexpr std::uint64_t BlinkingParameters[]{SelfColorIntensityParameter, GlassColorParameter, ReflectionColorParameter};

enum class Show : std::uint8_t
{
    SelfLit,
    Glass,
    All
};

constexpr fxui::Choice<Show> ShowChoices[]{{Show::SelfLit, "Self-lit"}, {Show::Glass, "Glass"}, {Show::All, "All"}};

// A line of the list: a material by its row in the game's pool.
struct Row
{
    std::uint32_t poolRow{};
    std::uint64_t name{};
    float brightness{};
    std::string text;
};

// A parameter of the selected material, zeroed for the dark half of a blink.
struct Blink
{
    std::uint32_t poolRow{};
    std::uint64_t parameter{};
    Value held;
};

std::array<std::atomic<std::uint32_t>, PacketMaterialBits + 1> lastDrawn{};

using Replay = void (*)(void* state, const std::uint64_t* packet, std::uint32_t first, std::uint32_t second);
Replay originalReplay{};

Show show{};
bool isDrawnOnly{true}, isBlinking{true}, hasUnsavedChanges{};
std::vector<Row> rows;
int framesToRefresh{};
std::optional<Row> selected;
std::array<char, 96> label{};
std::optional<Blink> blink;
std::uint64_t presents{}, lastShown{};

void replayDraw(void* state, const std::uint64_t* packet, std::uint32_t first, std::uint32_t second)
{
    lastDrawn[*packet & PacketMaterialBits].store(game_loop::currentFrame(), std::memory_order_relaxed);
    originalReplay(state, packet, first, second);
}

bool isDrawnNow(std::uint32_t poolRow)
{
    return game_loop::currentFrame() - lastDrawn[poolRow].load(std::memory_order_relaxed) < DrawnNowFrames;
}

const std::filesystem::path& rulesFile()
{
    static const std::filesystem::path file(u8"" PHANTOM_FX_MATERIALS_FILE);
    return file;
}

float brightnessOf(const Material& material)
{
    const auto intensity = valueOf(material.packed, SelfColorIntensityParameter);
    const auto color = valueOf(material.packed, SelfColorParameter);
    if (intensity.empty()) return 0;

    return intensity[0] * (color.size() < 3 ? 1 : std::max({color[0], color[1], color[2]}));
}

bool isShown(const Material& material)
{
    if (show == Show::SelfLit) return !valueOf(material.packed, SelfColorIntensityParameter).empty();
    if (show == Show::Glass) return !valueOf(material.packed, GlassColorParameter).empty();

    return true;
}

// The rule that made the material what it is now. Rules match by value, so another material of the
// name that holds this `to` by itself reads as tuned too.
template<class Rules> auto ruleAppliedTo(const Material& material, Rules& rules)
{
    return std::ranges::find_if(rules, [&](const Rule& rule) { return rule.material == material.name && isAwaited(material.packed, rule, Direction::Back); });
}

std::string rowText(const Material& material, const std::vector<Rule>& rules)
{
    const auto rule = ruleAppliedTo(material, rules);
    const auto tuned = rule == rules.end() ? std::string{} : "  [" + (rule->label.empty() ? "tuned" : rule->label) + "]";
    const auto first = show == Show::SelfLit ? std::format("{:>9.0f}", brightnessOf(material)) : "technique " + hashText(material.technique);

    return std::format("{}  {}  used {}{}", first, hashText(material.name), material.uses, tuned);
}

void refreshRows()
{
    if (--framesToRefresh > 0) return;

    framesToRefresh = FramesBetweenRefreshes;
    rows.clear();

    const auto rules = rulesInForce();
    game_materials::forEachMaterial([&](const Material& material) {
        if (isShown(material) && (!isDrawnOnly || isDrawnNow(material.index)))
            rows.push_back({material.index, material.name, brightnessOf(material), rowText(material, *rules)});
    });

    std::ranges::sort(rows, [](const Row& a, const Row& b) { return a.brightness != b.brightness ? a.brightness > b.brightness : a.name < b.name; });
}

void select(const Row& row)
{
    selected = row;
    label = {};
    framesToRefresh = 0;

    const auto material = game_materials::materialAt(row.poolRow);
    if (!material) return;

    const auto rules = rulesInForce();
    const auto rule = ruleAppliedTo(*material, *rules);
    if (rule != rules->end()) rule->label.copy(label.data(), label.size() - 1);
}

// The selected material, while its row still holds it.
std::optional<Material> selectedMaterial()
{
    const auto material = selected ? game_materials::materialAt(selected->poolRow) : std::nullopt;
    if (material && material->name == selected->name) return material;

    selected.reset();

    return std::nullopt;
}

std::uint64_t blinkingParameter(const Material& material)
{
    for (const auto parameter : BlinkingParameters)
        if (!valueOf(material.packed, parameter).empty()) return parameter;

    return parameterOfLowHalf(material.packed.parameters.front());
}

void startBlink(const Material& material)
{
    if (!isBlinking || GetTickCount64() / BlinkMilliseconds % 2) return;

    const auto parameter = blinkingParameter(material);
    blink = {material.index, parameter, valueOf(material.packed, parameter)};
    setValue(material.packed, parameter, Value(blink->held.size(), 0));
}

void noteChange()
{
    hasUnsavedChanges = true;
    framesToRefresh = 0;
}

// The parameter of the material goes to `value`: the materials under the rule on it come back and go
// under the rule as it is now, or a new one.
void edit(const Material& material, std::uint64_t parameter, const Value& value)
{
    auto rules = *rulesInForce();
    auto rule = ruleAppliedTo(material, rules);
    if (rule == rules.end()) rule = rules.insert(rules.end(), Rule{.material = material.name});

    const auto before = *rule;
    retune(*rule, parameter, valueOf(material.packed, parameter), value);

    const auto after = *rule;
    if (after.changes.empty()) rules.erase(rule);

    sweep(before, Direction::Back);
    sweep(after, Direction::Forward);
    sortRules(rules);
    putInForce(std::move(rules));
    noteChange();
}

void save()
{
    auto temporary = rulesFile();
    temporary += ".tmp";

    bool isWritten{};
    {
        std::ofstream output(temporary, std::ios::binary);
        output << writeRules(*rulesInForce());
        isWritten = output.good();
    }

    std::error_code error;
    if (isWritten) std::filesystem::rename(temporary, rulesFile(), error);
    if (!isWritten || error)
    {
        fxui::toast(Tone::Bad, "Materials not saved", rulesFile().string());
        return;
    }

    hasUnsavedChanges = false;
    fxui::toast(Tone::Ok, "Materials saved", rulesFile().string());
}

// What a slider moves through for a component that started at `original`: wide values by ratio, as a
// lamp's intensity goes from hundreds to hundreds of thousands.
fxui::SliderSpec sliderFor(float original)
{
    const float size = std::abs(original);
    if (size >= 10) return {.min = size / 1000, .max = size * 4, .standard = original, .format = "%.0f", .logarithmic = true};

    return {.min = std::min(0.0f, original * 4), .max = std::max(1.0f, size * 4), .standard = original, .format = "%.3f"};
}

// What the rule on the material found in the parameter, or what it holds under no rule.
Value originalOf(const Material& material, std::uint64_t parameter, const Value& value)
{
    const auto rules = rulesInForce();
    const auto rule = ruleAppliedTo(material, *rules);
    if (rule == rules->end()) return value;

    const auto change = std::ranges::find(rule->changes, parameter, &Change::parameter);

    return change == rule->changes.end() ? value : change->from;
}

void drawParameter(const Material& material, std::uint64_t parameter)
{
    auto value = valueOf(material.packed, parameter);
    if (value.empty()) return;

    const auto original = originalOf(material, parameter, value);
    const auto name = parameterText(parameter);
    bool isChanged{};
    for (std::size_t component = 0; component < value.size(); ++component)
    {
        const auto text = value.size() == 1 ? name : std::format("{}.{}", name, "xyzw"[component]);
        isChanged |= fxui::slider(text.c_str(), value[component], sliderFor(component < original.size() ? original[component] : value[component]));
    }

    if (value != original && original[0] != 0) fxui::hint(std::format("{:.3g} times the game's value", value[0] / original[0]));
    if (isChanged) edit(material, parameter, value);
}

void drawSelected(const Material& material)
{
    fxui::gap(4);
    fxui::fact("Name", hashText(material.name));
    fxui::fact("Technique", hashText(material.technique));
    fxui::fact("In use", std::format("{} times", material.uses));
    fxui::checkbox("Blink on screen", isBlinking, "Shows which material this is: its first parameter goes to nothing and back.");

    fxui::subheading("Parameters");
    for (const auto lowHalf : material.packed.parameters) drawParameter(material, parameterOfLowHalf(lowHalf));

    auto rules = *rulesInForce();
    const auto rule = ruleAppliedTo(material, rules);
    if (rule == rules.end()) return;

    fxui::subheading("Label");
    fxui::textBox("##remasterLabel", label, 1);
    if (const std::string_view text = label.data(); text != rule->label)
    {
        rule->label = text;
        putInForce(std::move(rules));
        noteChange();
    }
}

void drawList()
{
    if (fxui::segmented("Show", show, ShowChoices)) framesToRefresh = 0;
    if (fxui::checkbox("Drawn now", isDrawnOnly, "Only materials the game drew in the last second, so what is around you and not all it has loaded.")) framesToRefresh = 0;

    refreshRows();

    std::vector<std::string_view> names;
    names.reserve(rows.size());
    std::optional<std::size_t> shownRow;
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        names.push_back(rows[i].text);
        if (selected && rows[i].poolRow == selected->poolRow) shownRow = i;
    }

    const auto preview = selected ? selected->text : std::format("{} materials", rows.size());
    if (const auto picked = fxui::searchCombo("Material", preview.c_str(), names, shownRow, "Brightest first on Self-lit. Type to search.")) select(rows[*picked]);
}
}

std::optional<std::string> savedRulesText()
{
    std::ifstream file(rulesFile(), std::ios::binary);
    if (!file) return std::nullopt;

    return std::string(std::istreambuf_iterator<char>(file), {});
}

std::string hookTool() noexcept
{
    if (!game_code::matches(Checked)) return "the game's code differs";

    const auto status = game_code::hookAt(game_code::image() + ReplayRva, replayDraw, originalReplay);

    return status == MH_OK ? "" : MH_StatusToString(status);
}

void endBlink() noexcept
{
    if (!blink) return;

    if (const auto material = game_materials::materialAt(blink->poolRow)) setValue(material->packed, blink->parameter, blink->held);

    blink.reset();
}

void presentTool() noexcept
{
    ++presents;

    // A material must not stay dark once the tool is no longer drawn.
    if (presents - lastShown > PresentsUnseen) endBlink();
}

void drawTool()
{
    lastShown = presents;
    endBlink();

    const fxui::Item item({.id = "remaster",
        .name = "Materials",
        .summary = "Tunes the game's materials and saves the rules into the repository.",
        .keywords = "remaster material lamp self-lit bright emission glass tune rule"});
    if (!item) return;

    // With the rules off, what a material holds is not what a rule made it.
    if (!areRulesOn())
    {
        fxui::note(Tone::Dim, "The rules are off while the visual enhancements are off.");
        return;
    }

    fxui::fact("Rules", std::format("{}{}", rulesInForce()->size(), hasUnsavedChanges ? ", not saved" : ""));

    drawList();

    const auto material = selectedMaterial();
    if (material) drawSelected(*material);

    fxui::gap(6);
    if (fxui::button("Save", "Writes the rules into src/remaster/materials.json in the checkout this build came from.", hasUnsavedChanges)) save();

    if (material) startBlink(*material);
}
}
