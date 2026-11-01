#include "cutscenes.hpp"

#include "cutscene_list.hpp"
#include "fxui/fxui.hpp"
#include "game_lua.hpp"
#include "log.hpp"

#include <array>
#include <atomic>
#include <format>
#include <string>

namespace phantom_fx::cutscenes
{
namespace
{
constexpr int MotherBase = 30050;

constexpr auto Labels = [] {
    std::array<std::string_view, std::size(List)> labels{};
    for (std::size_t i = 0; i < labels.size(); ++i) labels[i] = List[i].label;

    return labels;
}();

// After the lines that set missionCode, demoName, stage and place. These chunks return nothing when
// the game leaves, or why not. The mission clear is Travel's: free roam and the ACC have nothing to
// clear. The status goes in a module's table, as the game's _G drops new globals without a word.
constexpr std::string_view CanLeave = R"(
if not (mvars and svars) then return "The game's scripts are not ready." end
if not (TppMission.IsFreeMission(vars.missionCode) or TppMission.IsHelicopterSpace(vars.missionCode)) then return "Only from free roam or the ACC." end
if not TppMission.CheckMissionState() then return "Not now: the game is busy." end
)";

// The cutscene's mission loads as usual, then starts in a sequence of ours in place of its own. It
// arrives on foot: a start in the helicopter would keep the player in it. A cutscene on a later stage
// has the sequence reload to it first, as the mission's own scripts do. A start position from the ACC
// goes first, both the initial one and the mission's that the reload copies back over it: on another
// location it is nowhere, and the mission would wait for its blocks for ever. The game takes that
// reload for a mission start and puts the mission's preloaded demo block back, so the block loads
// after it.
// ReserveInTheBackGround moves the player and the map's streaming to where the cutscene starts and the
// texture wait streams from its first camera, as the game does for a cutscene far from the player.
// The way out is the abort of the game's menu, so no story mission is cleared and no score is kept. A
// mission that starts on a cutscene is not in game until it ends, and until then an abort only waits.
constexpr std::string_view ToMission = R"(
local Sequence, LoadSeconds = "Seq_Demo_PhantomFX", 60

local function leave(status)
  TppDemo.phantomFxStatus = status
  TppMission.EnableInGameFlag()
  TppMission.AbortForRideOnHelicopter()
end

local isOnStage, deadline = stage == nil
local cutscene = {
  OnEnter = function()
    local id = mvars.dem_demoList[demoName]
    local name = mvars.dem_invDemoList[id]
    if not name then return leave("This mission does not hold it.") end

    if not isOnStage then
      isOnStage = true
      TppPlayer.ResetInitialPosition()
      TppPlayer.ResetMissionStartPosition()
      stage.isNoFade, stage.showLoadingTips = true, false
      return TppMission.Reload(stage)
    end

    deadline = Time.GetRawElapsedTimeSinceStartUp() + LoadSeconds
    local useDemoBlock = TppScriptBlock.FindPackList("demo_block", name) ~= nil
    if useDemoBlock then TppScriptBlock.LoadDemoBlock(name) end
    TppDemo.ReserveInTheBackGround{demoName = name}
    TppDemo.Play(name, {
      onInit = place and function() place(id) end,
      onStart = function()
        deadline = nil
        TppDemo.phantomFxStatus = "playing"
      end,
      onEnd = function() leave("done") end,
    }, {useDemoBlock = useDemoBlock, finishFadeOut = true, waitTextureLoadOnDemoPlay = true})
  end,
  OnUpdate = function()
    if not deadline or Time.GetRawElapsedTimeSinceStartUp() < deadline then return end

    deadline = nil
    leave("It did not load in its mission.")
  end,
}

local reserve = TppMain.ReservePlayerLoadingPosition
TppMain.ReservePlayerLoadingPosition = function(...)
  TppMain.ReservePlayerLoadingPosition = reserve
  reserve(...)
  if vars.missionCode ~= missionCode then return end

  TppPlayer.SetStartStatus(TppDefine.INITIAL_PLAYER_STATE.ON_FOOT)
  TppHelicopter.ResetMissionStartHelicopterRoute()
end

local saveStart = TppSequence.SaveMissionStartSequence
TppSequence.SaveMissionStartSequence = function()
  if isOnStage then TppSequence.SaveMissionStartSequence = saveStart end
  saveStart()
  if vars.missionCode ~= missionCode then return end

  local names, index = mvars.seq_sequenceNames, #mvars.seq_sequenceNames + 1
  names[index], names[Sequence] = Sequence, index
  mvars.seq_sequenceTable[Sequence] = cutscene
  mvars.seq_demoSequneceList[index] = true
  mvars.seq_isHelicopterStart = false
  mvars.seq_missionStartSequence = index
end

if not TppMission.ReserveMissionClear{missionClearType = TppDefine.MISSION_CLEAR_TYPE.FROM_HELISPACE, nextMissionId = missionCode} then
  TppMain.ReservePlayerLoadingPosition = reserve
  TppSequence.SaveMissionStartSequence = saveStart
  return "The game would not leave now."
end
)";

// Mother Base's cutscenes go through the game's own queue, which sets up the cluster, the time and the
// weather and leaves the player in free roam. Arriving from the ACC the game fills the queue itself,
// so the pick goes back in after it. A cutscene's end can start story events, change the buddy or end
// the mission (TppMbFreeDemo.demoOptions) and marks the cutscene as played, all of it kept in the
// save, which a test must not change: the end's script stays out and the flag goes back.
constexpr std::string_view ToMotherBase = R"(
local HeliRoute = "ly003_cl00_30050_heli0000|cl00pl0_mb_fndt_plnt_heli_30050|rt_apr"

local update, play = TppDemo.UpdateMBDemo, TppMbFreeDemo.PlayMtbsEventDemo
local function unwrap()
  TppDemo.UpdateMBDemo = update
  TppMbFreeDemo.PlayMtbsEventDemo = play
end

TppDemo.UpdateMBDemo = function()
  update()
  TppDemo.SetNextMBDemo(demoName)
end

TppMbFreeDemo.PlayMtbsEventDemo = function(params)
  local name = params.demoName
  if not TppMbFreeDemo.GetNextDemo(name) then unwrap() end

  local options, played = TppMbFreeDemo.demoOptions[name], TppDefine.MB_FREEPLAY_DEMO_ENUM[name]
  local endScript = options and options.OnEnd
  local wasPlayed = played and gvars.mbFreeDemoPlayedFlag[played]
  if options then options.OnEnd = nil end
  TppDemo.phantomFxStatus = "playing"

  local onEnd = params.onEnd
  params.onEnd = function()
    if options then options.OnEnd = endScript end
    if played then gvars.mbFreeDemoPlayedFlag[played] = wasPlayed end
    TppDemo.phantomFxStatus = "done"
    onEnd()
  end
  play(params)
end

TppDemo.SetNextMBDemo(demoName)
if not TppMission.ReserveMissionClear{missionClearType = TppDefine.MISSION_CLEAR_TYPE.FROM_HELISPACE, nextMissionId = missionCode, nextHeliRoute = HeliRoute} then
  unwrap()
  TppDemo.SetNextMBDemo()
  return "The game would not leave now."
end
)";

constexpr std::string_view ReadStatus = R"(
local status = TppDemo.phantomFxStatus
TppDemo.phantomFxStatus = nil
return status or ""
)";

// How often the status is read while a cutscene is on its way: the game's scripts cannot call out.
constexpr int FramesPerRead = 60;

constexpr int NoRequest = -1;
std::atomic<int> requested{NoRequest};

// Only the game thread touches these.
const Cutscene* current{};
int framesToRead{};

void start(const Cutscene& cutscene)
{
    const auto setup = std::format("local missionCode, demoName, stage = {}, \"{}\", {}\nlocal place = {}\n", cutscene.mission, cutscene.name, cutscene.stage,
        cutscene.place);
    const auto route = cutscene.mission == MotherBase ? ToMotherBase : ToMission;
    const auto outcome = game_lua::run(setup + std::string(CanLeave) + std::string(route), "=PhantomFX cutscene");
    if (!outcome)
    {
        fxui::toast(fxui::Tone::Warn, "Cutscene", "The game's scripts are not running yet.");
        return;
    }

    if (!outcome->ran)
    {
        log(std::string("Cutscene ") + cutscene.label + " failed: " + outcome->text);
        fxui::toast(fxui::Tone::Bad, "Cutscene failed", "See PhantomFX.log.");
        return;
    }

    if (!outcome->text.empty())
    {
        fxui::toast(fxui::Tone::Warn, "Cutscene", outcome->text);
        return;
    }

    log(std::string("Cutscene: going to ") + cutscene.label + ".");
    fxui::toast(fxui::Tone::Ok, "Going to the cutscene", cutscene.label);
    current = &cutscene;
    framesToRead = FramesPerRead;
}

void readStatus()
{
    if (--framesToRead > 0) return;

    framesToRead = FramesPerRead;
    const auto outcome = game_lua::run(ReadStatus, "=PhantomFX cutscene status");
    if (!outcome || !outcome->ran || outcome->text.empty()) return;

    log(std::format("Cutscene {}: {}", current->label, outcome->text));
    if (outcome->text == "playing") return;

    if (outcome->text != "done") fxui::toast(fxui::Tone::Warn, "Cutscene", outcome->text);
    current = nullptr;
}
}

std::span<const std::string_view> labels() noexcept
{
    return Labels;
}

void play(std::size_t index) noexcept
{
    if (index < std::size(List)) requested = static_cast<int>(index);
}

void onFrameStart() noexcept
{
    const int index = requested.exchange(NoRequest);
    if (index != NoRequest) start(List[index]);
    if (current) readStatus();
}
}
