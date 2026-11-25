# DevKit add-on

The developer tools as their own ReShade add-on (`DevKit.addon64`), built in the dev preset beside
`PhantomFX.addon64` and not linked into it. It draws `src/devkit/page.*` in ReShade's default ImGui
style, and gets the game's frame start from PhantomFX when that is loaded (or hooks the loop itself)
for travel, cutscenes, the Lua console, command files and the render diagnostics: the game's shaders (search,
assembly, bytecode) and a frame recorder (`src/devkit/frame_log.*`), for any MGSV add-on, not only PhantomFX. It
also merges a synthetic gamepad button into XInput (`src/devkit/gamepad.*`), so an automated run can press the
screens that wait for one without taking the keyboard or the foreground.

| File | What it is |
| --- | --- |
| `addon.cpp` | ReShade's entry points: the log, the build check, the tools' install, the present event and the overlay page |
| `game_loop.*` | The main loop's frame start (0x7F840) and the devkit calls on it |

## Use

Copy `build/dev/DevKit.addon64` beside `mgsvtpp.exe` with ReShade 6.8. The add-on keeps its files in
`DevKit/` beside itself:

- `DevKit.log` — its log, including the reasons a tool did not run.
- `DevKit/` — the automation (`src/devkit/automation.*`, its lines are listed in the header):
  `<name>.cmd` files, one line at a time, and F9's debug cases. The name is the one the
  `tools/automation` scripts know. When it holds `saves/` and `local/`, the game's Steam Cloud and user
  data folder are redirected there (the automation copy), or the game quits at once.

- `DevKit/frame-log/` — make it to turn the frame recorder on (F10, or the `frame` line): `frame-<n>.txt`
  with every draw, its shaders, targets and states, and `dump.txt` for constants and textures. Without
  it nothing is recorded; the shader list is kept either way.
- `DevKit/shaders/` — what `shader <hash> save` and the page's *Save* write: `<stage>_<hash>.dxbc` and `.asm`.

The page is ReShade's overlay entry **DevKit**: travel, cutscenes, **Shaders** (filter by stage, hash or
source name; click one for its assembly; copy its hash or save it; record a frame) and the Lua console.

## With PhantomFX

DevKit knows no add-on. Another add-on, PhantomFX among them, plugs its own command lines, state and F9
into it through the `DevKitGetApi` export (`src/devkit/extension.hpp`); PhantomFX's are in
`src/dev/devkit_link.cpp`. `help` in a command file lists the lines there are.
When `PhantomFX.addon64` is loaded, DevKit also registers its frame work on PhantomFX's main-loop hook
through the `PhantomFXRegisterFrameStart` export, so both add-ons run together with one hook; otherwise
DevKit hooks the loop itself. The lines that stand on the game alone (`lua`, `travel`, `cutscene`, `state`,
`status`, `peek`) run either way. PhantomFX's own Developer page keeps the remaster panel and the cost
recorder (`src/dev/`).
