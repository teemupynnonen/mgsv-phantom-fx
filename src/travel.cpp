#include "travel.hpp"

#include "fxui/fxui.hpp"
#include "game_lua.hpp"
#include "log.hpp"

#include <format>
#include <mutex>
#include <string>
#include <string_view>
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

// After the line that sets missionCode and position. It returns nothing when the game leaves, or
// why not. The mission clear is what free roam reserves when the player leaves; elsewhere it would
// end a story mission, hence free roam only. The game puts a free roam arriving from free roam where
// the player stood, which in another map is mid-air or underground; so the next loading position the
// game reserves is replaced, once, with the destination on foot, and the map streams in around it.
// Mother Base on foot with one of its cutscenes waiting can stop loading.
constexpr std::string_view Script = R"(
if not (mvars and svars and TppMission.IsFreeMission(vars.missionCode)) then return "Only from free roam." end
if vars.missionCode == missionCode then return "Already there." end
if not TppMission.CheckMissionState() then return "Not now: the game is busy." end
if missionCode == 30050 and gvars.mbFreeDemoPlayNextIndex ~= 0 then return "A Mother Base cutscene is waiting; go by helicopter." end

local reserve = TppMain.ReservePlayerLoadingPosition
TppMain.ReservePlayerLoadingPosition = function(...)
  TppMain.ReservePlayerLoadingPosition = reserve
  reserve(...)
  if vars.missionCode ~= missionCode then return end

  TppPlayer.SetStartStatus(TppDefine.INITIAL_PLAYER_STATE.ON_FOOT)
  TppHelicopter.ResetMissionStartHelicopterRoute()
  TppPlayer.SetInitialPosition(position, 0)
  TppPlayer.SetMissionStartPosition(position, 0)
  StageBlockCurrentPositionSetter.SetEnable(true)
  StageBlockCurrentPositionSetter.SetPosition(position[1], position[3])
end

if not TppMission.ReserveMissionClear{missionClearType = TppDefine.MISSION_CLEAR_TYPE.FROM_HELISPACE, nextMissionId = missionCode} then
  TppMain.ReservePlayerLoadingPosition = reserve
  return "The game would not leave free roam now."
end
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
    const auto outcome = game_lua::run(setup + std::string(Script), "=PhantomFX travel");
    if (!outcome)
    {
        fxui::toast(fxui::Tone::Warn, "Travel", "The game's scripts are not running yet.");
        return;
    }

    if (!outcome->ran)
    {
        log("Travel to " + std::string(destination.name) + " failed: " + outcome->text);
        fxui::toast(fxui::Tone::Bad, "Travel failed", "See PhantomFX.log.");
        return;
    }

    if (!outcome->text.empty())
    {
        fxui::toast(fxui::Tone::Warn, "Travel", outcome->text);
        return;
    }

    log("Travelling to " + std::string(destination.name) + ".");
    fxui::toast(fxui::Tone::Ok, std::format("Travelling to {}", destination.name));
}
}
