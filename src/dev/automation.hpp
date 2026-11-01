#pragma once

#include <filesystem>

struct ID3D11DeviceContext;
struct ID3D11Resource;

// Commands from files, so an agent can look into and drive the running game (dev builds only;
// docs/DEVELOPMENT.md, "Debug cases"). They come through the `PhantomFX-automation` folder beside the add-on.
// Each `<name>.cmd` file put there runs, one line at a time, from the main loop's frame start, and
// `<name>.result` holds what each line did once all have run. Files run in name order, one at a time; write
// each under another name first and rename it, so it is never read half written.
//
// With `saves` and `local` folders in it, the game is the automation copy: its saves are read from and
// written to there, and its backups and display settings (save_redirect.hpp), or the game quits at once.
// Without them it is the player's game, on the player's saves.
//
// F9 freezes the game and records a debug case into `cases/<time>`; F9 again lets it go.
//
// Lines:
//   case [<name>]                 records a case (caseLines in automation.cpp)
//   freeze                        photo mode frozen at the camera now
//   release                       leaves the frozen camera, `probe`'s pixel and the debug view it turned on
//   camera <x> <y> <z> <yaw> <pitch>  photo mode frozen at that pose (held_camera.hpp), radians
//   move <x> <y> <z> <yaw> <pitch> <frames>  from the frozen camera there, one step each camera update
//   wait <frames>                 lets that many frames present
//   wait_scene                    until the scene's work to do has stopped changing
//   probe <x> <y>                 the shadow and lamp probes' text at that render pixel
//   perf <file> [<frames>]        what those frames cost, into `<file>.json` (perf_record.hpp), 120 without saying
//   record_costs 0|1              the Developer page's Record the costs
//   inspect <file>                a record of every pixel, into `<file>.bin` and `<file>.tsv` (inspect_trace.hpp)
//   image <file>                  the next presented frame as `<file>.dds`
//   textures <folder>             the ray tracing's textures and the game's shadow, as DDS files
//   game_shadow 0|1               keeps the game's shadow per pixel (game_shadow.hpp)
//   reveal off|all|<hash> ...     restarts the scene, taking what it leaves out: all of it or those models
//   enhancements 0|1              the F1 switch
//   setting <key>=<value>         a PhantomFX.ini line, applied at once (and saved, as the overlay does)
//   diagnostic <name> <value>     debug_view, show_scene, two_sided_shadows or no_jitter
//   lua <code>                    runs `code` in the game's Lua (game_lua.hpp), its value or error
//   travel <map> [<x> <y> <z>]    afghanistan, africa or motherbase free roam (travel.hpp), at that place
//   clock <seconds>               the time of day, its clock stopped there (weather.hpp)
//   weather <type> [<fog>]|off    forces TppDefine.WEATHER's type, with the fog's density for Foggy; or lets go
//   status                        frame counts, the camera, the scene
namespace phantom_fx::automation
{
void install(const std::filesystem::path& addonDirectory) noexcept;
// F9: on the render thread.
void caseKey() noexcept;
// The game thread, at the main loop's frame start.
void onFrameStart() noexcept;
// The render thread, as each frame presents its back buffer.
void onPresent(ID3D11DeviceContext* context, ID3D11Resource* backBuffer) noexcept;
}
