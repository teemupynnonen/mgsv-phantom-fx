#pragma once

#include <filesystem>

struct ID3D11DeviceContext;
struct ID3D11Resource;
struct DevKitApi;

// The automation: commands from files that let an agent look into and drive the running game, and F9's debug cases
// (docs/DEVELOPMENT.md, "Debug cases"). The runner, the waits and the lines that stand on the game alone are here; an
// add-on adds its own lines, its state and what F9 does through extension.hpp, and DevKit knows nothing of which.
//
// Files come through the `DevKit` folder beside the add-ons: each `<name>.cmd` runs, one line at a time, from the main
// loop's frame start, and `<name>.result` holds what each line did once all have run. With `saves` and `local` folders
// in it, the game is the automation copy: its saves are read from and written to there (save_redirect.hpp), or the game
// quits at once. Without them it is the player's game. `skip-logos` there drops the game's startup logos.
//
// Lines of DevKit's own:
//   wait <frames>                 lets that many frames present
//   lua <code>                    runs `code` in the game's Lua, its value or error
//   state [<name>]                what the game is at, and what the add-ons say; with a name, into cases/<name>/state.txt
//   status                        what the add-ons say they are at
//   travel <map> [<x> <y> <z>]    afghanistan, africa or motherbase free roam, at that place
//   cutscene <index>              plays the cutscene at that index of the list
//   key <name> [times]            presses a key at the game's window: the loading screens' space, and the likes.
//   pad <name> [polls]            presses a gamepad button the game reads, with no focus taken: a, b, x, y, start, back.
//                                 A stalled loading screen stops this runner too, so the tools press Space from
//                                 outside the game for that case (`session action='recover'` in the MCP).
//   peek <address|game+rva> [bytes]  the game's memory there, 256 bytes without saying, 4096 at most
//   frame                         records a frame as F10 does, after its thirty warm-up presents (needs
//                                 DevKit/frame-log): draws, shaders, targets
//   shaders [<text>]              the shaders the game created: stage, hash, source name, size; text filters them
//   shader <hash start|name> [save]  a shader's assembly; `save` writes its .dxbc and .asm into DevKit/shaders
//   case_done <name>              keeps what the script's lines said in cases/<name>/log.txt
//   help                          the lines there are, with the add-ons' own
namespace devkit::automation
{
// Reads command files from `<addonDirectory>/DevKit`, and redirects the saves there when it holds some.
void install(const std::filesystem::path& addonDirectory) noexcept;
// What the add-ons plug into (extension.hpp), DevKit.addon64's `DevKitGetApi`.
const DevKitApi* extensionApi() noexcept;
// F9: from any thread; the game thread takes what the add-on that holds the key does, at its next frame start.
void caseKey() noexcept;
// The game thread, at the main loop's frame start.
void onFrameStart() noexcept;
// The render thread, as each frame presents its back buffer.
void onPresent(ID3D11DeviceContext* context, ID3D11Resource* backBuffer) noexcept;
}
