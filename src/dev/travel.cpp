#include "dev/travel.hpp"

#include "fxui/fxui.hpp"
#include "dev/game_lua.hpp"
#include "log.hpp"

#include <format>
#include <mutex>
#include <string>
#include <utility>

namespace phantom_fx::travel
{
namespace
{
struct Destination
{
    const char* name;
    int missionCode; // Its free roam (TppDefine.SYS_MISSION_ID)
    float position[3];
};

// Spots of the game's own: in Afghanistan and Africa where TppPlayer.FailSafeInitialPositionForFreePlay
// puts a player found off the map; at Mother Base the Command platform's first helipad (landing
// point cl00pl0_mb_fndt_plnt_heli_30050 in mbdvc_map_mbstage_parameter.lua, the same in every layout).
constexpr Destination Destinations[]{
    {"Afghanistan", 30010, {1448.61f, 337.787f, 1466.4f}},
    {"Africa", 30020, {-510.73f, 5.09f, 1183.02f}},
    {"Mother Base", 30050, {9.1156f, 8.7501f, -42.4302f}},
};
static_assert(std::size(Destinations) == static_cast<std::size_t>(Map::Count));

// Ahead of a chunk that leaves. The mission clear is what free roam reserves when the player leaves,
// and free roam and the ACC have nothing else to clear; elsewhere it would end a story mission.
// arriveOnFoot replaces the next loading position the game reserves, once: the helicopter would keep
// the player in it. It returns what undoes that. leaveFor returns nothing when the game leaves, or
// why not after calling undo.
constexpr std::string_view DepartureHelpers = R"(
local function arriveOnFoot(missionCode, onArrival)
  local reserve = TppMain.ReservePlayerLoadingPosition
  TppMain.ReservePlayerLoadingPosition = function(...)
    TppMain.ReservePlayerLoadingPosition = reserve
    reserve(...)
    if vars.missionCode ~= missionCode then return end

    TppPlayer.SetStartStatus(TppDefine.INITIAL_PLAYER_STATE.ON_FOOT)
    TppHelicopter.ResetMissionStartHelicopterRoute()
    if onArrival then onArrival() end
  end

  return function() TppMain.ReservePlayerLoadingPosition = reserve end
end

local function leaveFor(missionCode, heliRoute, undo)
  if TppMission.ReserveMissionClear{missionClearType = TppDefine.MISSION_CLEAR_TYPE.FROM_HELISPACE, nextMissionId = missionCode, nextHeliRoute = heliRoute} then return end

  undo()
  return "The game would not leave now."
end
)";

// After the line that sets missionCode and position. The game puts a free roam arriving from free
// roam where the player stood, which in another map is mid-air or underground; so the arrival is the
// destination, and the map streams in around it. Mother Base on foot with one of its cutscenes waiting
// can stop loading.
constexpr std::string_view Script = R"(
if not (mvars and svars and TppMission.IsFreeMission(vars.missionCode)) then return "Only from free roam." end
if vars.missionCode == missionCode then return "Already there." end
if not TppMission.CheckMissionState() then return "Not now: the game is busy." end
if missionCode == 30050 and gvars.mbFreeDemoPlayNextIndex ~= 0 then return "A Mother Base cutscene is waiting; go by helicopter." end

local undoArrival = arriveOnFoot(missionCode, function()
  TppPlayer.SetInitialPosition(position, 0)
  TppPlayer.SetMissionStartPosition(position, 0)
  StageBlockCurrentPositionSetter.SetEnable(true)
  StageBlockCurrentPositionSetter.SetPosition(position[1], position[3])
end)
return leaveFor(missionCode, nil, undoArrival)
)";

struct Request
{
    Map map{};
    std::optional<std::array<float, 3>> place;
};
std::mutex requestMutex;
std::optional<Request> requested;
}

const char* name(Map map) noexcept
{
    return map < Map::Count ? Destinations[static_cast<std::size_t>(map)].name : "Unknown";
}

void go(Map map, std::optional<std::array<float, 3>> place) noexcept
{
    if (map >= Map::Count) return;

    std::lock_guard lock(requestMutex);
    requested = Request{map, place};
}

bool depart(std::string_view code, std::string_view tool, std::string_view target)
{
    const auto outcome = game_lua::run(std::string(DepartureHelpers).append(code), std::format("=PhantomFX {}", tool));
    if (!outcome)
    {
        fxui::toast(fxui::Tone::Warn, tool, "The game's scripts are not running yet.");
        return false;
    }

    if (!outcome->ran)
    {
        log(std::format("{}, {}, failed: {}", tool, target, outcome->text));
        fxui::toast(fxui::Tone::Bad, std::format("{} failed", tool), "See PhantomFX.log.");
        return false;
    }

    if (!outcome->text.empty())
    {
        fxui::toast(fxui::Tone::Warn, tool, outcome->text);
        return false;
    }

    return true;
}

void onFrameStart() noexcept
{
    std::optional<Request> request;
    {
        std::lock_guard lock(requestMutex);
        request = std::exchange(requested, std::nullopt);
    }

    if (!request) return;

    const auto& destination = Destinations[static_cast<std::size_t>(request->map)];
    const auto position = request->place.value_or(std::array{destination.position[0], destination.position[1], destination.position[2]});
    const auto setup = std::format("local missionCode, position = {}, {{{}, {}, {}}}\n", destination.missionCode, position[0], position[1], position[2]);
    if (!depart(setup + std::string(Script), "Travel", destination.name)) return;

    log("Travelling to " + std::string(destination.name) + ".");
    fxui::toast(fxui::Tone::Ok, std::format("Travelling to {}", destination.name));
}
}
