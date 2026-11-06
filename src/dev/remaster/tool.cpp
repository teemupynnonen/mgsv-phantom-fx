// The remastering tool's state and actions; only a dev build has this file (remaster.cmake).
#include "dev/remaster/tool.hpp"

#include "fxui/fxui.hpp"
#include "game_code.hpp"
#include "game_loop.hpp"
#include "dev/game_lua.hpp"
#include "game_memory.hpp"
#include "log.hpp"
#include "remaster/in_force.hpp"
#include "remaster/remaster.hpp"
#include "remaster/rule_file.hpp"
#include "dev/travel.hpp"
#include "weather.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
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

// Changes to the same thing closer together than this are one step to undo: a slider's drag, a label's typing.
constexpr ULONGLONG ChangeGapMilliseconds = 600;
constexpr std::size_t MostUndoSteps = 200;

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

// A lamp's light draw seen lately (noteLamp).
struct SeenLamp
{
    std::uint16_t location{};
    std::array<float, 3> at{}, color{};
    std::uint32_t frame{};
};

std::array<std::atomic<std::uint32_t>, PacketMaterialBits + 1> lastDrawn{};

// The lamps' light draws come on the game's render threads: what they read here, under this.
std::mutex lampMutex;
std::unordered_map<std::uint64_t, SeenLamp> seenLamps; // By where the lamp is, to the centimetre
std::optional<std::array<float, 3>> lampFlashWanted, flashingLamp;
std::optional<LampRule> comparedLamp;

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
std::vector<LampRule> savedLamps;

// The rules before each change, to undo and redo.
struct Snapshot
{
    std::vector<Rule> rules;
    std::vector<LampRule> lamps;
};
std::vector<Snapshot> undoSteps, redoSteps;
std::string lastChange;
ULONGLONG lastChangeAt{};
std::uint64_t restoreCount{};
// The labels the tool gave this session: a rule still under one has not been named.
std::set<std::string> defaultLabels;
// Who holds the game without rules (holdWithoutRules), by Holder: the rules come back once none does.
std::array<bool, 2> isHeldBy{};
std::optional<LampRow> chosenLamp;
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

std::uint64_t lampKey(const std::array<float, 3>& at)
{
    std::uint64_t key = 0xCBF29CE484222325;
    for (const float coordinate : at) key = (key ^ static_cast<std::uint64_t>(std::llround(coordinate * 100))) * 0x100000001B3;

    return key;
}

bool isLampDrawnNow(const std::array<float, 3>& at)
{
    std::lock_guard lock(lampMutex);
    const auto seen = seenLamps.find(lampKey(at));

    return seen != seenLamps.end() && game_loop::currentFrame() - seen->second.frame < DrawnNowFrames;
}

std::uint16_t currentLocation()
{
    const auto vars = varsBlock();
    return vars ? game_memory::read<std::uint16_t>(vars + VarLocation).value_or(0) : 0;
}

Snapshot snapshot()
{
    return {*rulesInForce(), *lampsInForce()};
}

// Before each change: a step to undo, unless it goes on the last one. A change of nothing named (a removal) is always a
// step of its own.
void noteChange(const std::string& what = {})
{
    const auto now = GetTickCount64();
    if (what.empty() || what != lastChange || now - lastChangeAt > ChangeGapMilliseconds)
    {
        undoSteps.push_back(snapshot());
        if (undoSteps.size() > MostUndoSteps) undoSteps.erase(undoSteps.begin());
        redoSteps.clear();
    }

    lastChange = what;
    lastChangeAt = now;
}

// The materials taken off the rules in force and put under these, which go in force. While the enhancements switch (F1)
// has the rules off, the materials stay as the game made them, and turning it on puts these on.
void restore(Snapshot to)
{
    settleTool();
    const bool isOn = areRulesOn() && !areRulesHeldOff();
    if (isOn)
        for (const auto& rule : *rulesInForce()) sweep(rule, Direction::Back);

    putInForce(std::move(to.rules));
    if (isOn)
        for (const auto& rule : *rulesInForce()) sweep(rule, Direction::Forward);

    putLampsInForce(std::move(to.lamps));
    lastChange.clear();
    framesToRefresh = 0;
    ++restoreCount;
}

std::string givenLabel(std::string label)
{
    defaultLabels.insert(label);
    return label;
}

auto lampRuleOf(std::vector<LampRule>& lamps, const LampRow& lamp)
{
    return std::ranges::find_if(lamps, [&](const LampRule& each) { return each.location == lamp.location && isLampAt(each.at, lamp.at); });
}

std::optional<travel::Map> mapOf(std::uint16_t location)
{
    if (location == 10) return travel::Map::Afghanistan;
    if (location == 20) return travel::Map::Africa;
    if (location == 50) return travel::Map::MotherBase;

    return std::nullopt;
}

bool isSameLampRule(const LampRule& a, const LampRule& b)
{
    return a.location == b.location && isLampAt(a.at, b.at);
}

// The row's rule taken out of `rules` and the file's put back.
template<class R> void putBack(std::vector<R>& rules, const R& rule, const std::optional<R>& saved, RuleState state)
{
    if (state != RuleState::Removed)
        if (const auto found = std::ranges::find(rules, rule); found != rules.end()) rules.erase(found);

    if (saved) rules.push_back(*saved);
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
    savedLamps = *lampsInForce();
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

void noteLamp(std::uint16_t location, const std::array<float, 3>& at, const std::array<float, 3>& color, float& brightness) noexcept
{
    try
    {
        std::lock_guard lock(lampMutex);
        seenLamps[lampKey(at)] = {location, at, color, game_loop::currentFrame()};
        if (comparedLamp && comparedLamp->location == location && isLampAt(comparedLamp->at, at)) brightness = 1;
        if (flashingLamp && isLampAt(*flashingLamp, at)) brightness = 0;
    }
    catch (...)
    {
    }
}

void endFlash() noexcept
{
    {
        std::lock_guard lock(lampMutex);
        flashingLamp.reset();
    }

    if (!flashing) return;

    if (const auto material = game_materials::materialAt(flashing->poolRow)) setValue(material->packed, flashing->parameter, flashing->held);

    flashing.reset();
}

void settleTool() noexcept
{
    endFlash();
    {
        std::lock_guard lock(lampMutex);
        comparedLamp.reset();
    }

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
    // A flash holds its material at zero until the next overlay, and a mark must know it by its real values.
    endFlash();

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

    {
        std::lock_guard lock(lampMutex);
        for (const auto& [key, lamp] : seenLamps)
            if (lamp.location == mark.location && game_loop::currentFrame() - lamp.frame < DrawnNowFrames) mark.lamps.push_back({lamp.at, lamp.color});
    }

    if (mark.materials.empty())
    {
        fxui::toast(Tone::Warn, "Nothing to mark", "The game has drawn no materials.");
        return;
    }

    const auto selfLit = countOf(mark, MaterialKind::SelfLit);
    markList.push_back(std::move(mark));
    saveMarks();
    const auto lamps = markList.back().lamps.size();
    fxui::toast(Tone::Ok, std::format("Mark {}", markList.size()), std::format("{} self-lit, {} lamps. F7 opens the marks.", selfLit, lamps));
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
    for (const auto& pair : paired(*rules, savedRules, isSameMaterialRule))
    {
        const auto& rule = pair.now ? *pair.now : *pair.saved;

        // What the rule says, until a loaded material says more.
        MaterialRow row{.name = rule.material, .label = rule.label};
        const auto intensity = std::ranges::find(rule.changes, SelfColorIntensityParameter, &Change::parameter);
        if (intensity != rule.changes.end())
        {
            row.kind = MaterialKind::SelfLit;
            row.brightness = intensity->to[0];
        }

        rows.push_back({rule, pair.saved ? std::optional(*pair.saved) : std::nullopt, std::move(row), stateOf(pair)});
    }

    // A removed rule is on no material: it shows as the file has it.
    game_materials::forEachMaterial([&](const Material& material) {
        for (auto& row : rows)
        {
            if (row.state == RuleState::Removed || row.material.poolRow || row.rule.material != material.name || !isAwaited(material.packed, row.rule, Direction::Back))
                continue;

            row.material.poolRow = material.index;
            row.material.kind = kindOf(material);
            row.material.brightness = brightnessOf(material);
            row.material.color = colorOf(material);
        }
    });

    return rows;
}

std::vector<LampRow> lampRowsOf(const Mark& mark)
{
    std::vector<LampRow> rows;
    const auto rules = lampsInForce();
    for (const auto& lamp : mark.lamps)
    {
        LampRow row{.location = mark.location, .at = lamp.at, .color = lamp.color, .isDrawn = isLampDrawnNow(lamp.at)};
        if (const auto* rule = lampRuleAt(*rules, mark.location, lamp.at))
        {
            row.brightness = rule->brightness;
            row.label = rule->label;
            row.isTuned = true;
        }

        rows.push_back(std::move(row));
    }

    // By their colour at the mark, which no rule changes.
    std::ranges::sort(rows, [](const LampRow& a, const LampRow& b) { return std::ranges::max(a.color) > std::ranges::max(b.color); });

    return rows;
}

std::vector<LampRuleRow> lampRuleRows()
{
    std::vector<LampRuleRow> rows;
    const auto lamps = lampsInForce();
    for (const auto& pair : paired(*lamps, savedLamps, isSameLampRule))
        rows.push_back({pair.now ? *pair.now : *pair.saved, pair.saved ? std::optional(*pair.saved) : std::nullopt, stateOf(pair)});

    return rows;
}

void select(std::uint32_t poolRow, std::uint64_t name)
{
    selectedRow = poolRow;
    selectedName = name;
    chosenLamp.reset();
}

void clearSelection()
{
    selectedRow.reset();
    chosenLamp.reset();
}

void selectLamp(const LampRow& lamp)
{
    selectedRow.reset();
    chosenLamp = lamp;
}

std::optional<LampRow> selectedLamp()
{
    if (!chosenLamp) return std::nullopt;

    auto lamp = *chosenLamp;
    const auto rules = lampsInForce();
    const auto* rule = lampRuleAt(*rules, lamp.location, lamp.at);
    lamp.brightness = rule ? rule->brightness : 1;
    lamp.label = rule ? rule->label : std::string{};
    lamp.isTuned = rule != nullptr;
    lamp.isDrawn = isLampDrawnNow(lamp.at);

    return lamp;
}

void flashLamp(const std::array<float, 3>& at)
{
    std::lock_guard lock(lampMutex);
    lampFlashWanted = at;
}

void editLamp(const LampRow& lamp, float brightness)
{
    noteChange(std::format("lamp {}", lampKey(lamp.at)));

    auto lamps = *lampsInForce();
    auto rule = lampRuleOf(lamps, lamp);
    if (rule == lamps.end())
    {
        const auto& [x, y, z] = lamp.at;
        const auto label = std::format("{}, lamp {:.0f} at {:.1f}, {:.1f}, {:.1f}", locationName(lamp.location), std::ranges::max(lamp.color), x, y, z);
        rule = lamps.insert(lamps.end(), LampRule{.label = givenLabel(label), .location = lamp.location, .at = lamp.at});
    }

    // Back at 1 under no name of its own, there is nothing to keep.
    rule->brightness = brightness;
    if (brightness == 1 && (rule->label.empty() || defaultLabels.contains(rule->label))) lamps.erase(rule);

    putLampsInForce(std::move(lamps));
}

void setLampLabel(const LampRow& lamp, const std::string& label)
{
    auto lamps = *lampsInForce();
    const auto rule = lampRuleOf(lamps, lamp);
    if (rule == lamps.end()) return;

    noteChange(std::format("lamp label {}", lampKey(lamp.at)));
    rule->label = label;
    putLampsInForce(std::move(lamps));
}

void removeLampRule(const LampRule& rule)
{
    auto lamps = *lampsInForce();
    const auto found = std::ranges::find(lamps, rule);
    if (found == lamps.end()) return;

    noteChange();
    lamps.erase(found);
    putLampsInForce(std::move(lamps));
}

void resetLamp(const LampRow& lamp)
{
    auto lamps = *lampsInForce();
    const auto rule = lampRuleOf(lamps, lamp);
    if (rule != lamps.end()) removeLampRule(*rule);
}

void holdOriginalLamp(const LampRow& lamp, bool isHeld)
{
    std::lock_guard lock(lampMutex);
    comparedLamp = isHeld && lamp.isTuned ? std::optional(LampRule{.location = lamp.location, .at = lamp.at}) : std::nullopt;
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
    {
        std::lock_guard lock(lampMutex);
        const auto lamp = std::exchange(lampFlashWanted, std::nullopt);
        if (lamp && !comparedLamp && !(GetTickCount64() / FlashMilliseconds % 2)) flashingLamp = lamp;
    }

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
    noteChange(std::format("material {} {}", material.index, parameter));

    auto rules = *rulesInForce();
    auto rule = ruleAppliedTo(material, rules);
    if (rule == rules.end())
    {
        const auto label = kindOf(material) == MaterialKind::SelfLit
            ? std::format("{}, glow {:.6g}, {} uses", locationName(currentLocation()), brightnessOf(material), material.uses)
            : std::format("{}, {} {}, {} uses", locationName(currentLocation()), kindOf(material) == MaterialKind::Glass ? "glass" : "material", hashText(material.name),
                  material.uses);
        rule = rules.insert(rules.end(), Rule{.label = givenLabel(label), .material = material.name});
    }

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

    noteChange(std::format("material label {}", material.index));
    rule->label = label;
    putInForce(std::move(rules));
}

void removeRule(const Rule& rule)
{
    auto rules = *rulesInForce();
    const auto found = std::ranges::find(rules, rule);
    if (found == rules.end()) return;

    noteChange();
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

std::uint64_t restores()
{
    return restoreCount;
}

bool undo()
{
    if (undoSteps.empty()) return false;

    redoSteps.push_back(snapshot());
    auto step = std::move(undoSteps.back());
    undoSteps.pop_back();
    restore(std::move(step));

    return true;
}

bool redo()
{
    if (redoSteps.empty()) return false;

    undoSteps.push_back(snapshot());
    auto step = std::move(redoSteps.back());
    redoSteps.pop_back();
    restore(std::move(step));

    return true;
}

std::size_t defaultNamedRules()
{
    const auto isDefault = [](const auto& rule) { return defaultLabels.contains(rule.label); };

    return static_cast<std::size_t>(std::ranges::count_if(*rulesInForce(), isDefault) + std::ranges::count_if(*lampsInForce(), isDefault));
}

bool isComparing() noexcept
{
    std::lock_guard lock(lampMutex);
    return compared.has_value() || comparedLamp.has_value() || areRulesHeldOff();
}

void holdWithoutRules(Holder holder, bool isHeld)
{
    isHeldBy[static_cast<std::size_t>(holder)] = isHeld;
    const bool isWanted = std::ranges::any_of(isHeldBy, std::identity{});
    if (isWanted == areRulesHeldOff()) return;

    // A flash or a one-light compare holds its own values: it ends before the rules come off, so they come back right.
    if (isWanted) settleTool();

    holdRulesOff(isWanted);
    framesToRefresh = 0;
}

std::size_t unsavedRules()
{
    const auto isUnsaved = [](const auto& pair) { return stateOf(pair) != RuleState::Saved; };
    const auto rules = rulesInForce();
    const auto lamps = lampsInForce();

    return static_cast<std::size_t>(std::ranges::count_if(paired(*rules, savedRules, isSameMaterialRule), isUnsaved)
        + std::ranges::count_if(paired(*lamps, savedLamps, isSameLampRule), isUnsaved));
}

void saveRules()
{
    const auto rules = rulesInForce();
    const auto lamps = lampsInForce();
    hasSaveFailed = !writeFile(rulesFile(), writeRules(*rules, *lamps));
    if (hasSaveFailed)
    {
        fxui::toast(Tone::Bad, "Materials not saved", rulesFile().string());
        return;
    }

    savedRules = *rules;
    savedLamps = *lamps;
    fxui::toast(Tone::Ok, "Materials saved", rulesFile().string());
}

bool didSaveFail()
{
    return hasSaveFailed;
}

void revertRule(const RuleRow& row)
{
    auto to = snapshot();
    putBack(to.rules, row.rule, row.saved, row.state);
    sortRules(to.rules);

    noteChange();
    restore(std::move(to));
}

void revertLampRule(const LampRuleRow& row)
{
    auto to = snapshot();
    putBack(to.lamps, row.rule, row.saved, row.state);

    noteChange();
    restore(std::move(to));
}

void discardUnsaved()
{
    if (!unsavedRules()) return;

    noteChange();
    restore({savedRules, savedLamps});
    fxui::toast(Tone::Ok, "Unsaved changes discarded", "Ctrl+Z brings them back");
}

std::string runLine(std::string_view line)
{
    std::istringstream words{std::string(line)};
    std::string command;
    words >> command;

    framesToRefresh = 0;
    refreshLoaded();

    // The panel waits while the game shows without rules, and so do these: a rule is matched by the values the game holds.
    constexpr std::string_view Edits[]{"set", "original", "lamp", "reset", "compare", "undo", "redo", "discard"};
    if (areRulesHeldOff() && std::ranges::find(Edits, command) != std::end(Edits)) return "error: the game is held without rules: without 0 first\n";

    if (command == "mark")
    {
        addMark();
        return markList.empty() ? "error: nothing marked\n" : std::format("mark {}: {} materials\n", markList.size(), markList.back().materials.size());
    }

    if (command == "without")
    {
        int isHeld{};
        words >> isHeld;
        holdWithoutRules(Holder::Line, isHeld != 0);
        return std::format("without rules: {}\n", areRulesHeldOff() ? "held" : "let go");
    }

    if (command == "undo" || command == "redo")
    {
        const bool isDone = command == "undo" ? undo() : redo();
        return std::format("{}: {}; {} material and {} lamp rules\n", command, isDone ? "done" : "nothing to do", rulesInForce()->size(), lampsInForce()->size());
    }

    if (command == "rules")
    {
        constexpr std::string_view States[]{"saved", "new", "changed", "removed"};
        std::string text;
        for (const auto& row : ruleRows())
            text += std::format("material {} \"{}\" {}\n", hashText(row.rule.material), row.rule.label, States[static_cast<std::size_t>(row.state)]);
        for (const auto& row : lampRuleRows())
            text += std::format("lamp {} \"{}\" {}\n", row.rule.brightness, row.rule.label, States[static_cast<std::size_t>(row.state)]);

        return std::format("{}{} unsaved, {} under default names\n", text, unsavedRules(), defaultNamedRules());
    }

    if (command == "discard")
    {
        const auto unsaved = unsavedRules();
        discardUnsaved();

        return std::format("{} unsaved discarded\n", unsaved);
    }

    if (command == "loaded")
    {
        std::string text;
        game_materials::forEachMaterial([&](const Material& material) {
            if (kindOf(material) != MaterialKind::SelfLit && !isDrawnNow(material.index)) return;

            text += std::format("{} row {} brightness {:.6g} uses {} technique {:x}{}:", hashText(material.name), material.index, brightnessOf(material),
                material.uses, material.technique, isDrawnNow(material.index) ? " drawn" : "");
            for (const auto lowHalf : material.packed.parameters)
            {
                const auto parameter = parameterOfLowHalf(lowHalf);
                const auto value = valueOf(material.packed, parameter);
                text += " " + parameterText(parameter) + "=" + (value.empty() ? std::string("-") : std::format("{:.4g}", value[0]));
            }
            text += "\n";
        });

        return text;
    }

    if (markList.empty()) return "error: no mark yet\n";

    const auto& mark = markList.back();
    if (command == "lamps")
    {
        std::string text;
        const auto rows = lampRowsOf(mark);
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            const auto& [x, y, z] = rows[i].at;
            text += std::format("{} at {:.2f} {:.2f} {:.2f} colour {:.4g} brightness {:.4g}{}{}\n", i + 1, x, y, z, std::ranges::max(rows[i].color), rows[i].brightness,
                rows[i].isTuned ? " tuned" : "", rows[i].isDrawn ? " drawn" : "");
        }

        return text.empty() ? "no lamps at the last mark\n" : text;
    }

    if (command == "lamp")
    {
        std::size_t number{};
        float brightness{};
        const auto rows = lampRowsOf(mark);
        // As the rule file reads it: a value it refuses would cost every rule at the next start.
        if (!(words >> number >> brightness) || number < 1 || number > rows.size() || !(brightness >= 0)) return "error: lamp <number> <brightness>\n";

        editLamp(rows[number - 1], brightness);
        return std::format("lamp {} brightness {}\n", number, lampRowsOf(mark)[number - 1].brightness);
    }

    if (command == "reset")
    {
        std::size_t number{};
        const auto rows = lampRowsOf(mark);
        if (!(words >> number) || number < 1 || number > rows.size()) return "error: reset <number>\n";

        resetLamp(rows[number - 1]);
        return std::format("lamp {} brightness {}\n", number, lampRowsOf(mark)[number - 1].brightness);
    }

    if (command == "compare" || command == "panel")
    {
        std::size_t number{};
        int isHeld{};
        const auto rows = lampRowsOf(mark);
        if (!(words >> number) || number < 1 || number > rows.size() || (command == "compare" && !(words >> isHeld))) return "error: compare|panel <number> ...\n";

        if (command == "panel") showLamp(markList.size() - 1, rows[number - 1]);
        else holdOriginalLamp(rows[number - 1], isHeld != 0);

        return "done\n";
    }
    if (command == "rows")
    {
        std::string kindWord;
        words >> kindWord;
        const auto kind = kindWord == "all" ? std::nullopt : kindWord == "glass" ? std::optional(MaterialKind::Glass) : std::optional(MaterialKind::SelfLit);
        std::string text;
        for (const auto& row : rowsOf(mark, kind))
            text += std::format("{} {} brightness {:.6g} uses {}{}{}\n", hashText(row.name), row.poolRow ? std::format("row {}", *row.poolRow) : "not loaded",
                row.brightness, row.uses, row.isTuned ? " tuned" : "", row.label.empty() ? "" : " \"" + row.label + "\"");

        return text;
    }

    std::string name;
    words >> name;
    const auto hash = hashOf(name);
    const auto rows = rowsOf(mark, std::nullopt);
    const auto row = std::ranges::find_if(rows, [&](const MaterialRow& each) { return each.name == hash && each.poolRow; });
    auto material = row != rows.end() ? game_materials::materialAt(*row->poolRow) : std::nullopt;
    // A mark takes only what its hooked draws drew: a material it missed is still found by its name.
    if (!material && hash)
        game_materials::forEachMaterial([&](const Material& each) {
            if (!material && each.name == *hash) material = each;
        });
    if (!material) return "error: no such material loaded\n";

    if (command == "original")
    {
        removeRuleOn(*material);
        return "back to the original\n";
    }

    if (command == "set")
    {
        std::string parameterName;
        words >> parameterName;
        const auto parameter = parameterOf(parameterName);
        Value value;
        for (float component{}; words >> component;) value.push_back(component);
        if (!parameter || value.empty()) return "error: set <material> <parameter> <value>\n";

        edit(*material, *parameter, value);
        return std::format("{} {} now {}\n", name, parameterName, valueOf(game_materials::materialAt(material->index)->packed, *parameter)[0]);
    }

    return "error: unknown remaster line\n";
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
