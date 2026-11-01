// The remastering tool's state and actions; only a dev build has this file (remaster.cmake).
#include "remaster/tool.hpp"

#include "fxui/fxui.hpp"
#include "game_code.hpp"
#include "game_loop.hpp"
#include "game_lua.hpp"
#include "game_memory.hpp"
#include "log.hpp"
#include "remaster/in_force.hpp"
#include "remaster/remaster.hpp"
#include "remaster/rule_file.hpp"
#include "travel.hpp"
#include "weather.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <utility>

namespace phantom_fx::remaster
{
namespace
{
using fxui::Tone;
using game_materials::Material;

constexpr std::uintptr_t ReplayRva = 0x2D6310;
// The script variables' block, and where in it vars.locationCode, missionCode, playerPosX and playerRotY are.
constexpr std::uintptr_t VarsRva = 0x2C16920;
constexpr std::size_t VarLocation = 0x10, VarMission = 0x12, VarPlayerPosition = 0xEC, VarPlayerRotation = 0x108;

constexpr unsigned char ReplayCode[]{0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30};
// Their registrations' "mov r8, [VarsRva]; add r8, offset".
constexpr unsigned char LocationCode[]{0x4C, 0x8B, 0x05, 0xC6, 0xF9, 0x27, 0x02, 0x49, 0x83, 0xC0, 0x10};
constexpr unsigned char MissionCode[]{0x4C, 0x8B, 0x05, 0x61, 0xF9, 0x27, 0x02, 0x49, 0x83, 0xC0, 0x12};
constexpr unsigned char PositionCode[]{0x4C, 0x8B, 0x05, 0x0D, 0xE4, 0x27, 0x02, 0x49, 0x81, 0xC0, 0xEC, 0x00, 0x00, 0x00};
constexpr unsigned char RotationCode[]{0x4C, 0x8B, 0x05, 0x61, 0xE2, 0x27, 0x02, 0x49, 0x81, 0xC0, 0x08, 0x01, 0x00, 0x00};
const game_code::Expected Checked[]{{ReplayRva, ReplayCode}, {0x996F53, LocationCode}, {0x996FB8, MissionCode}, {0x99850C, PositionCode}, {0x9986B8, RotationCode}};

// A draw packet's first word holds the row of its material in the low bits, all ones for none.
constexpr std::uint64_t PacketMaterialBits = 0x7FFF;
// Drawn within this many game frames is drawn now.
constexpr std::uint32_t DrawnNowFrames = 60;
constexpr int FramesBetweenRefreshes = 30;
constexpr ULONGLONG FlashMilliseconds = 400;
constexpr std::uint64_t PresentsBetweenTravelChecks = 60;
// Long enough for the slowest loading screen and the walk out of it.
constexpr int MostTravelChecks = 180;

// What blinks: the first of these a material has, else its first parameter.
constexpr std::uint64_t FlashingParameters[]{SelfColorIntensityParameter, GlassColorParameter, ReflectionColorParameter};

// What the game has loaded of a marked material.
struct Loaded
{
    std::uint32_t poolRow{};
    bool isTuned{};
};

// A parameter held at nothing for the dark half of a flash.
struct Flash
{
    std::uint32_t poolRow{};
    std::uint64_t parameter{};
    Value held;
};

// A trip to a mark: the warp waits for the game to be playing there.
struct Travel
{
    Mark mark;
    int consoleRuns{};
    int checks{};
    bool isChecking{};
};

std::array<std::atomic<std::uint32_t>, PacketMaterialBits + 1> lastDrawn{};

using Replay = void (*)(void* state, const std::uint64_t* packet, std::uint32_t first, std::uint32_t second);
Replay originalReplay{};

std::vector<Mark> markList;
std::unordered_map<std::uint64_t, Loaded> loaded;
int framesToRefresh{};
std::optional<std::uint32_t> selectedRow;
std::uint64_t selectedName{};
std::optional<std::uint32_t> flashWanted;
std::optional<Flash> flashing;
std::optional<Rule> compared;
std::optional<Travel> travelling;
std::uint64_t presents{};
std::vector<Rule> savedRules;
bool hasSaveFailed{};

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

// Beside the add-on: a dev build's scratch, not the repository's.
const std::filesystem::path& marksFile()
{
    static const std::filesystem::path file = [] {
        HMODULE self{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&marksFile), &self);

        std::wstring path(MAX_PATH, L'\0');
        path.resize(GetModuleFileNameW(self, path.data(), MAX_PATH));

        return std::filesystem::path(path).parent_path() / "PhantomFX-marks.json";
    }();

    return file;
}

// The file whole or not at all, as an interrupted write must not cost what was there.
bool writeFile(const std::filesystem::path& file, const std::string& text)
{
    auto temporary = file;
    temporary += ".tmp";

    bool isWritten{};
    {
        std::ofstream output(temporary, std::ios::binary);
        output << text;
        isWritten = output.good();
    }

    std::error_code error;
    if (isWritten) std::filesystem::rename(temporary, file, error);

    return isWritten && !error;
}

void loadMarks()
{
    std::ifstream file(marksFile(), std::ios::binary);
    if (!file) return;

    const std::string text(std::istreambuf_iterator<char>(file), {});
    auto parsed = parseMarks(text);
    if (parsed.error.empty()) markList = std::move(parsed.marks);
    else log("Materials: the marks are not used: " + parsed.error);
}

void saveMarks()
{
    if (!writeFile(marksFile(), writeMarks(markList))) fxui::toast(Tone::Bad, "Marks not saved", marksFile().string());
}

std::uintptr_t varsBlock() noexcept
{
    const auto vars = game_memory::read<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(game_code::image()) + VarsRva);
    return vars ? *vars : 0;
}

float brightnessOf(const Material& material)
{
    const auto intensity = valueOf(material.packed, SelfColorIntensityParameter);
    const auto color = valueOf(material.packed, SelfColorParameter);
    if (intensity.empty()) return 0;

    return intensity[0] * (color.size() < 3 ? 1 : std::max({color[0], color[1], color[2]}));
}

std::array<float, 3> colorOf(const Material& material)
{
    const auto color = valueOf(material.packed, SelfColorParameter);
    if (color.size() < 3) return {1, 1, 1};

    return {color[0], color[1], color[2]};
}

MaterialKind kindOf(const Material& material)
{
    if (!valueOf(material.packed, SelfColorIntensityParameter).empty()) return MaterialKind::SelfLit;

    return valueOf(material.packed, GlassColorParameter).empty() ? MaterialKind::Other : MaterialKind::Glass;
}

// The rule that made the material what it is now. Rules match by value, so another material of the
// name that holds this `to` by itself reads as tuned too.
template<class Rules> auto ruleAppliedTo(const Material& material, Rules& rules)
{
    return std::ranges::find_if(rules, [&](const Rule& rule) { return rule.material == material.name && isAwaited(material.packed, rule, Direction::Back); });
}

// The same, and still that rule while a compare holds the material at its original.
const Rule* ruleOn(const Material& material, const std::vector<Rule>& rules)
{
    const auto applied = ruleAppliedTo(material, rules);
    if (applied != rules.end()) return &*applied;
    if (!compared || compared->material != material.name || !isAwaited(material.packed, *compared, Direction::Forward)) return nullptr;

    const auto held = std::ranges::find(rules, *compared);

    return held != rules.end() ? &*held : nullptr;
}

std::uint64_t flashingParameter(const Material& material)
{
    for (const auto parameter : FlashingParameters)
        if (!valueOf(material.packed, parameter).empty()) return parameter;

    return parameterOfLowHalf(material.packed.parameters.front());
}

// Waits for the game to play, wherever it is, and warps once it does.
std::string warpScript(const Mark& mark)
{
    return std::format(R"(local sequence = TppSequence.GetCurrentSequenceName()
if vars.locationCode ~= {} or sequence == nil or sequence == "" or sequence == "Seq_Mission_Prepare" then return "waiting" end
TppPlayer.Warp{{pos = {{{}, {}, {}}}, rotY = {}}}
return "arrived")",
        mark.location, mark.position[0], mark.position[1], mark.position[2], mark.rotation);
}

std::optional<travel::Map> mapOf(std::uint16_t location)
{
    if (location == 10) return travel::Map::Afghanistan;
    if (location == 20) return travel::Map::Africa;
    if (location == 50) return travel::Map::MotherBase;

    return std::nullopt;
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
    if (status != MH_OK) return MH_StatusToString(status);

    registerPanel();
    savedRules = *rulesInForce();
    try
    {
        loadMarks();
    }
    catch (const std::exception& error)
    {
        log(std::string("Materials: the marks are not used: ") + error.what());
    }

    return "";
}

void endFlash() noexcept
{
    if (!flashing) return;

    if (const auto material = game_materials::materialAt(flashing->poolRow)) setValue(material->packed, flashing->parameter, flashing->held);

    flashing.reset();
}

void settleTool() noexcept
{
    endFlash();
    if (compared) sweep(*compared, Direction::Forward);

    compared.reset();
}

void refreshLoaded()
{
    if (--framesToRefresh > 0) return;

    framesToRefresh = FramesBetweenRefreshes;
    loaded.clear();

    const auto rules = rulesInForce();
    game_materials::forEachMaterial([&](const Material& material) {
        loaded[fingerprintOf(material.packed, material.name, *rules)] = {material.index, ruleOn(material, *rules) != nullptr};
    });
}

void addMark()
{
    const auto vars = varsBlock();
    if (!vars) return;

    Mark mark{.location = game_memory::read<std::uint16_t>(vars + VarLocation).value_or(0),
        .mission = game_memory::read<std::uint16_t>(vars + VarMission).value_or(0),
        .position = game_memory::read<std::array<float, 3>>(vars + VarPlayerPosition).value_or(std::array<float, 3>{}),
        .rotation = game_memory::read<float>(vars + VarPlayerRotation).value_or(0),
        .clock = weather::state().clock};

    const auto rules = rulesInForce();
    game_materials::forEachMaterial([&](const Material& material) {
        if (isDrawnNow(material.index)) mark.materials.push_back({material.name, fingerprintOf(material.packed, material.name, *rules), kindOf(material), brightnessOf(material), material.uses});
    });

    if (mark.materials.empty())
    {
        fxui::toast(Tone::Warn, "Nothing to mark", "The game has drawn no materials.");
        return;
    }

    const auto selfLit = countOf(mark, MaterialKind::SelfLit);
    markList.push_back(std::move(mark));
    saveMarks();
    fxui::toast(Tone::Ok, std::format("Mark {}", markList.size()), std::format("{} self-lit. F7 opens the marks.", selfLit));
}

const std::vector<Mark>& marks()
{
    return markList;
}

void removeMark(std::size_t index)
{
    if (index >= markList.size()) return;

    markList.erase(markList.begin() + static_cast<std::ptrdiff_t>(index));
    saveMarks();
}

std::size_t tunedCount(const Mark& mark)
{
    return static_cast<std::size_t>(std::ranges::count_if(mark.materials, [](const MarkedMaterial& material) {
        const auto found = loaded.find(material.fingerprint);
        return found != loaded.end() && found->second.isTuned;
    }));
}

std::size_t countOf(const Mark& mark, MaterialKind kind)
{
    return static_cast<std::size_t>(std::ranges::count(mark.materials, kind, &MarkedMaterial::kind));
}

bool canGoTo(const Mark& mark)
{
    return game_lua::isSupported() && mapOf(mark.location).has_value();
}

bool isGoing() noexcept
{
    return travelling.has_value();
}

void goTo(const Mark& mark)
{
    const auto vars = varsBlock();
    const auto location = vars ? game_memory::read<std::uint16_t>(vars + VarLocation) : std::nullopt;
    const auto map = mapOf(mark.location);
    if (!map) return;

    if (location != mark.location) travel::go(*map);

    travelling = Travel{.mark = mark};
}

void followTravel()
{
    if (!travelling || ++presents % PresentsBetweenTravelChecks) return;

    const auto console = game_lua::console();
    if (console.waiting) return;

    if (travelling->isChecking && console.last && console.last->text == "arrived")
    {
        weather::setClock(travelling->mark.clock);
        travelling.reset();
        return;
    }

    if (++travelling->checks > MostTravelChecks)
    {
        fxui::toast(Tone::Bad, "Could not go there", "See the Lua console on the Developer page.");
        travelling.reset();
        return;
    }

    game_lua::submit(warpScript(travelling->mark));
    travelling->isChecking = true;
}

std::vector<MaterialRow> rowsOf(const Mark& mark, std::optional<MaterialKind> kind)
{
    std::vector<const MarkedMaterial*> chosen;
    for (const auto& marked : mark.materials)
        if (!kind || marked.kind == *kind) chosen.push_back(&marked);

    // By their value at the mark: live, a row would jump under the slider moving it.
    std::ranges::sort(chosen, [](const MarkedMaterial* a, const MarkedMaterial* b) { return a->brightness != b->brightness ? a->brightness > b->brightness : a->name < b->name; });

    std::vector<MaterialRow> rows;
    const auto rules = rulesInForce();
    for (const auto* markedRow : chosen)
    {
        const auto& marked = *markedRow;
        MaterialRow row{.name = marked.name, .kind = marked.kind, .brightness = marked.brightness, .uses = marked.uses};
        const auto found = loaded.find(marked.fingerprint);
        const auto material = found != loaded.end() ? game_materials::materialAt(found->second.poolRow) : std::nullopt;
        if (material && material->name == marked.name)
        {
            row.poolRow = material->index;
            row.brightness = brightnessOf(*material);
            row.color = colorOf(*material);
            row.uses = material->uses;

            const auto* rule = ruleOn(*material, *rules);
            row.isTuned = rule != nullptr;
            if (rule) row.label = rule->label;
        }

        rows.push_back(std::move(row));
    }

    return rows;
}

std::vector<RuleRow> ruleRows()
{
    std::vector<RuleRow> rows;
    const auto rules = rulesInForce();
    for (const auto& rule : *rules)
    {
        // What the rule says, until a loaded material says more.
        MaterialRow row{.name = rule.material, .label = rule.label};
        const auto intensity = std::ranges::find(rule.changes, SelfColorIntensityParameter, &Change::parameter);
        if (intensity != rule.changes.end())
        {
            row.kind = MaterialKind::SelfLit;
            row.brightness = intensity->to[0];
        }

        rows.push_back({rule, std::move(row)});
    }

    game_materials::forEachMaterial([&](const Material& material) {
        for (auto& row : rows)
        {
            if (row.material.poolRow || row.rule.material != material.name || !isAwaited(material.packed, row.rule, Direction::Back)) continue;

            row.material.poolRow = material.index;
            row.material.kind = kindOf(material);
            row.material.brightness = brightnessOf(material);
            row.material.color = colorOf(material);
        }
    });

    return rows;
}

void select(std::uint32_t poolRow, std::uint64_t name)
{
    selectedRow = poolRow;
    selectedName = name;
}

void clearSelection()
{
    selectedRow.reset();
}

std::optional<Material> selectedMaterial()
{
    const auto material = selectedRow ? game_materials::materialAt(*selectedRow) : std::nullopt;
    if (material && material->name == selectedName) return material;

    selectedRow.reset();

    return std::nullopt;
}

void flash(std::uint32_t poolRow)
{
    flashWanted = poolRow;
}

void startFlash()
{
    const auto wanted = std::exchange(flashWanted, std::nullopt);
    if (!wanted || compared || GetTickCount64() / FlashMilliseconds % 2) return;

    const auto material = game_materials::materialAt(*wanted);
    if (!material) return;

    const auto parameter = flashingParameter(*material);
    flashing = Flash{*wanted, parameter, valueOf(material->packed, parameter)};
    setValue(material->packed, parameter, Value(flashing->held.size(), 0));
}

Value originalOf(const Material& material, std::uint64_t parameter, const Value& value)
{
    const auto rules = rulesInForce();
    const auto* rule = ruleOn(material, *rules);
    if (!rule) return value;

    const auto change = std::ranges::find(rule->changes, parameter, &Change::parameter);

    return change == rule->changes.end() ? value : change->from;
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
    framesToRefresh = 0;
}

std::string labelOf(const Material& material)
{
    const auto rules = rulesInForce();
    const auto* rule = ruleOn(material, *rules);

    return rule ? rule->label : std::string{};
}

void setLabel(const Material& material, const std::string& label)
{
    auto rules = *rulesInForce();
    const auto rule = ruleAppliedTo(material, rules);
    if (rule == rules.end()) return;

    rule->label = label;
    putInForce(std::move(rules));
}

void removeRule(const Rule& rule)
{
    auto rules = *rulesInForce();
    const auto found = std::ranges::find(rules, rule);
    if (found == rules.end()) return;

    sweep(rule, Direction::Back);
    rules.erase(found);
    putInForce(std::move(rules));
    framesToRefresh = 0;
}

void removeRuleOn(const Material& material)
{
    const auto rules = rulesInForce();
    const auto rule = ruleAppliedTo(material, *rules);
    if (rule != rules->end()) removeRule(Rule{*rule});
}

bool hasRule(const Material& material)
{
    const auto rules = rulesInForce();
    return ruleOn(material, *rules) != nullptr;
}

void holdOriginal(const Material& material, bool isHeld)
{
    if (isHeld == compared.has_value()) return;

    if (!isHeld)
    {
        settleTool();
        return;
    }

    const auto rules = rulesInForce();
    const auto rule = ruleAppliedTo(material, *rules);
    if (rule == rules->end()) return;

    compared = *rule;
    sweep(*compared, Direction::Back);
}

bool isComparing() noexcept
{
    return compared.has_value();
}

std::size_t unsavedRules()
{
    return unsavedCount(savedRules, *rulesInForce());
}

void saveRules()
{
    const auto rules = rulesInForce();
    hasSaveFailed = !writeFile(rulesFile(), writeRules(*rules));
    if (hasSaveFailed)
    {
        fxui::toast(Tone::Bad, "Materials not saved", rulesFile().string());
        return;
    }

    savedRules = *rules;
    fxui::toast(Tone::Ok, "Materials saved", rulesFile().string());
}

bool didSaveFail()
{
    return hasSaveFailed;
}

void drawTool()
{
    const fxui::Item item({.id = "remaster",
        .name = "Materials",
        .summary = "F6 marks what is on screen while you play. F7 opens the marks, to tune them over the game.",
        .keywords = "remaster material lamp self-lit bright emission glass tune rule mark"});
    if (!item) return;

    fxui::fact("Marks", std::to_string(markList.size()));
    fxui::fact("Rules", std::to_string(rulesInForce()->size()));
}
}
