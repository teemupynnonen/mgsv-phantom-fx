#include "dev/cutscenes.hpp"

#include "dev/cutscene_list.hpp"
#include "fxui/fxui.hpp"
#include "dev/game_lua.hpp"
#include "log.hpp"
#include "dev/travel.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <string>

namespace phantom_fx::cutscenes
{
namespace
{
std::string labelOf(const Cutscene& cutscene)
{
    return std::format("{} {}: {} ({})", cutscene.mission.code, cutscene.mission.title, cutscene.name, cutscene.demoId);
}

// After the lines that set missionCode, demoName, stage and place. The status goes in a module's
// table, as the game's _G drops new globals without a word.
constexpr std::string_view LeaveChecks = R"(
if not (mvars and svars) then return "The game's scripts are not ready." end
if not (TppMission.IsFreeMission(vars.missionCode) or TppMission.IsHelicopterSpace(vars.missionCode)) then return "Only from free roam or the ACC." end
if not TppMission.CheckMissionState() then return "Not now: the game is busy." end
)";

// The cutscene's mission loads as usual, then starts in a sequence of ours in place of its own. A
// cutscene on a later stage has the sequence reload to it first, as the mission's own scripts do. A
// start position from the ACC goes first, both the initial one and the mission's that the reload copies
// back over it: on another location it is nowhere, and the mission would wait for its blocks for ever.
// The game takes that reload for a mission start and puts the mission's preloaded demo block back, so
// the block loads after it.
// ReserveInTheBackGround moves the player and the map's streaming to where the cutscene starts and the
// texture wait streams from its first camera, as the game does for a cutscene far from the player.
// The way out is the abort of the game's menu, so no story mission is cleared and no score is kept. A
// mission that starts on a cutscene is not in game until it ends, and until then an abort only waits.
// A cutscene's start wait turns a soldier into Snake and its end turns them back, so a cutscene that
// never starts has to do that itself.
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
    local hasDemoBlock = TppScriptBlock.FindPackList("demo_block", name) ~= nil
    if hasDemoBlock then TppScriptBlock.LoadDemoBlock(name) end
    TppDemo.ReserveInTheBackGround{demoName = name}
    TppDemo.Play(name, {
      onInit = place and function() place(id) end,
      onStart = function()
        deadline = nil
        TppDemo.phantomFxStatus = "playing"
      end,
      onEnd = function() leave("done") end,
    }, {useDemoBlock = hasDemoBlock, finishFadeOut = true, waitTextureLoadOnDemoPlay = true})
  end,
  OnUpdate = function()
    if not deadline or Time.GetRawElapsedTimeSinceStartUp() < deadline then return end

    deadline = nil
    for name, value in pairs(mvars.dem_tempPlayerInfo or {}) do vars[name] = value end
    leave("It did not load in its mission.")
  end,
}

local undoArrival = arriveOnFoot(missionCode)

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

return leaveFor(missionCode, nil, function()
  undoArrival()
  TppSequence.SaveMissionStartSequence = saveStart
end)
)";

// Mother Base's cutscenes go through the game's own queue, which sets up the cluster, the time and the
// weather and leaves the player in free roam. Arriving from the ACC the game fills the queue itself,
// so the pick goes back in after it, until the game plays it: reloads on the way ask again. A cutscene
// can lead to the next one, and all of them run this way. Their ends can start story events, change
// the buddy or end the mission (TppMbFreeDemo.demoOptions) and mark them as played, all of it kept in
// the save, which a test must not change: the end's script stays out and the flag goes back. The
// nuclear ceremony's sequence goes on to the credits and a mission clear, which give heroism and write
// the save, unless the sequence forgets the ceremony. If the game plays anything else, that was the
// game's own choice and it runs as usual.
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

local expected = demoName
TppMbFreeDemo.PlayMtbsEventDemo = function(params)
  local name = params.demoName
  if name ~= expected then
    unwrap()
    TppDemo.phantomFxStatus = "The game played " .. tostring(name) .. " in its place."
    return play(params)
  end

  expected = TppMbFreeDemo.GetNextDemo(name)
  local isLast = expected == nil
  if isLast then unwrap() end

  local options, playedIndex = TppMbFreeDemo.demoOptions[name], TppDefine.MB_FREEPLAY_DEMO_ENUM[name]
  local endScript = options and options.OnEnd
  local wasPlayed = playedIndex and gvars.mbFreeDemoPlayedFlag[playedIndex]
  if options then options.OnEnd = nil end
  TppDemo.phantomFxStatus = "playing"

  local onEnd = params.onEnd
  params.onEnd = function()
    if options then options.OnEnd = endScript end
    if playedIndex then gvars.mbFreeDemoPlayedFlag[playedIndex] = wasPlayed end
    if isLast then
      if mvars.f30050_demoName == "NuclearEliminationCeremony" then mvars.f30050_demoName = nil end
      TppDemo.phantomFxStatus = "done"
    end
    onEnd()
  end
  play(params)
end

TppDemo.SetNextMBDemo(demoName)
return leaveFor(missionCode, HeliRoute, function()
  unwrap()
  TppDemo.SetNextMBDemo()
end)
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

// Only the game thread touches these. Leaving mid-cutscene would lose its end, which puts the end
// scripts back, so a second Play waits for the first's "done".
const Cutscene* current{};
bool isPlaying{};
int framesToRead{};

void start(const Cutscene& cutscene)
{
    if (isPlaying)
    {
        fxui::toast(fxui::Tone::Warn, "Cutscene", "Not now: one is playing.");
        return;
    }

    const auto setup = std::format("{}local missionCode, demoName, stage = {}, \"{}\", {}\nlocal place = {}\n", ListHelpers, cutscene.mission.code, cutscene.name,
        cutscene.stage, cutscene.place);
    const auto route = cutscene.mission.code == MotherBase.code ? ToMotherBase : ToMission;
    const auto label = labelOf(cutscene);
    if (!travel::depart(setup + std::string(LeaveChecks) + std::string(route), "Cutscene", label)) return;

    log("Cutscene: going to " + label + ".");
    fxui::toast(fxui::Tone::Ok, "Going to the cutscene", label);
    current = &cutscene;
    framesToRead = FramesPerRead;
}

void readStatus()
{
    if (--framesToRead > 0) return;

    framesToRead = FramesPerRead;
    const auto outcome = game_lua::run(ReadStatus, "=PhantomFX cutscene status");
    if (!outcome || !outcome->ran || outcome->text.empty()) return;

    log(std::format("Cutscene {}: {}", labelOf(*current), outcome->text));
    isPlaying = outcome->text == "playing";
    if (isPlaying) return;

    if (outcome->text != "done") fxui::toast(fxui::Tone::Warn, "Cutscene", outcome->text);
    current = nullptr;
}
}

std::span<const std::string_view> labels() noexcept
{
    // Built on first use: a release build holds this file but never opens the page.
    static const auto texts = [] {
        std::array<std::string, std::size(List)> built;
        std::ranges::transform(List, built.begin(), labelOf);

        return built;
    }();
    static const auto views = [] {
        std::array<std::string_view, std::size(List)> built;
        std::ranges::copy(texts, built.begin());

        return built;
    }();

    return views;
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
