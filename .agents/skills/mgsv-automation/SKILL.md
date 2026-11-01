---
name: mgsv-automation
description: Debug and verify Phantom FX lighting in the running game with the automation tools, without asking the user for screenshots or probe readings. Use this whenever an MGSV lighting, shadow, ray tracing, path tracing or global illumination fault needs diagnosing or a fix needs checking in game - missing or extra shadows, surfaces too bright or dark, geometry missing from the traced scene - and whenever the user mentions F9, a debug case, "go" after pressing F9, replay, scenarios, regression checks, the automation copy, fx.py or case.py, even if they do not name this skill.
---

# MGSV automation

The user steers to a broken spot once and presses **F9**. Everything after that is yours: read what the game recorded, ask it questions while it is frozen, try fixes on the same frame, and verify them yourself. Do not ask the user for screenshots, probe readings or numbers; get them from the game.

The tools exist in **dev builds only** (`build/dev`). Reference: `docs/DEVELOPMENT.md`, "Debug cases". Every command line the game takes: `src/dev/automation.hpp`.

## Rules that protect the user

- **Never run the automation copy beside the player's game.** `launch.ps1` restores the player's save files when the copy exits, so it could overwrite newer saves. `fx.py replay` and `fx.py scenarios` refuse to start then. Check before launching anything by hand: `(Get-Process mgsvtpp).Path`.
- **Never stop the player's game.** If it is in the way, ask the user to close it, or wait for it to close. To stop the copy, stop only a process whose path is under `mgsv-automation`.
- **`--copy` decides which game you talk to.** Without it, `fx.py` commands reach the player's running game. `setting` writes to their `PhantomFX.ini`, so restore what you change.
- **Tell the user when their game is left frozen.** F9 again, or `fx.py run release`, lets it go.
- **Keep game images out of the repository.** Scenario baselines stay in `mgsv-automation/baselines`; only numbers go in `scenarios.json`.
- **Commit, push and install into the player's game only when the user has asked.** Install with a backup of the current add-on and with their game closed.

## Running the tools

Run from `tools/automation`, as `case.py` imports `fx.py` from there:

```
uv run -q --with numpy --with pillow --with scipy python fx.py <command>
uv run -q --with numpy --with pillow --with scipy python case.py [--copy] [<case>]
```

Cases are in `PhantomFX-automation/cases` beside each game:

- Player: `C:\Program Files (x86)\Steam\steamapps\common\MGS_TPP`
- Copy: `%USERPROFILE%\Documents\Development\mgsv-automation\game`

## The loop

1. **Read the case.** After the user presses F9, run `case.py` with no arguments: it reports their latest case. Read `report.md`, then look at `scene-check.png`, `sun-check.png` and `light-ratio.png`, and `traced.png` next to `vanilla.png`. Any screenshot or words from the user are extra context, not the data.
2. **Ask more while it is frozen.** The player's game stays frozen on that frame. Each finding gives a pixel: `fx.py probe <x> <y>` lists what the rays meet there and why. Move the camera with `fx.py run "camera <x> <y> <z> <yaw> <pitch>"` to see a fault from another side.
3. **Prove the cause before changing behaviour.** State it as a measured fact. To test a model, force it into the frozen scene:
   ```
   fx.py run "reveal <hash>" "wait 150" "game_shadow 1" "wait 30" "inspect cases/<case>/fix-scene" "image cases/<case>/fix-traced" "game_shadow 0" "reveal off"
   ```
   If the mismatch disappears, the cause is proven before any code is written.
4. **Fix it, then go back to the same frame.** Build the `dev` preset, then `fx.py replay <case>`. It installs the checkout's build into a fresh copy with the case's settings, goes back to the spot, records the case again as `<case>-replay` and says how nearly the view matches. It takes about 2 minutes. Compare the two reports. It needs the player's game closed.
5. **Guard against regressions.** `fx.py scenarios` runs every saved spot, about a minute each, and reports what got worse. Read each `before-after.png`.
6. **Show the user.** When the look changes, put a picture in chat with an absolute path, such as `![cabin](C:/.../before-after.png)`, before and after where it helps. Report numbers next to it.

## Commands

| Command | What it does |
|---|---|
| `fx.py [--copy] status` | Frame, camera, frozen or running, what the scene holds |
| `fx.py [--copy] probe <x> <y> [...]` | Shadow and lamp probes at render pixels. Needs ray-traced shadows or path tracing running |
| `fx.py [--copy] case [<name>]` | Records a case as F9 does, then reports it |
| `fx.py [--copy] run "<line>" ...` | Any lines of `src/dev/automation.hpp`, as one command file |
| `fx.py replay [<case>]` | Fresh copy, back at the spot, recorded and compared. Latest case of either game by default |
| `fx.py scenario <name> "<why>" [<case>]` | Keeps a case as a scenario, its numbers as what to expect |
| `fx.py scenarios [--accept] [<name> ...]` | Runs them, with the player's settings |
| `fx.py [--copy] boot` | Copy from launch into free roam, 30 to 45 s |
| `fx.py [--copy] travel <map>` | `afghanistan`, `africa` or `motherbase` free roam |
| `fx.py [--copy] acc` | From free roam into the helicopter's cabin (the ACC) |
| `fx.py png <file.dds>`, `fx.py diff <a.dds> <b.dds>` | A texture as a picture; how two captures differ |
| `check_release.py` | Fails if the tools reached a release build |
| `perf.py [--copy] log [<file>]` | A play session's costs by place, then its worst seconds |
| `perf.py [--copy] ab <test> ...` | What each change costs on the frame as it stands, with the drift to judge it by. `--images` also says how far the frame moved |

To drive the copy by hand: run `install.ps1` (`-Settings @()` for the player's exact settings), start `launch.ps1` in the background, then `fx.py --copy boot`. `replay` and `scenarios` do all of this themselves.

## Costs

A dev build writes what each second cost to `PhantomFX-automation/perf/`, with no one asking. The user plays, you read `perf.py log`. A case also takes `perf.json` on its frozen frame. Judge a change by `perf.py ab` on a frozen frame, and never by a difference smaller than the drift it reports.

## Reading a report

Each check compares the traced frame with what the game itself says about the same frozen frame.

- **The scene against the game's surfaces.** "Lacks" means the game drew a surface and the scene has nothing there. "In front" means the scene has something the game did not draw there. "In the sky" means the scene has something over the game's sky.
- **The sun's rays against the game's shadow map.** "Extra shadow" means the rays are blocked where the map says lit. "Missing shadow" means the rays reach the sun where the map says blocked. Only inside the game's shadow range, so "no reference" is normal indoors or far away.
- **The light against the game's own.** The median of traced over vanilla, with a grid over the frame. Vanilla is taken under the same pause, so its exposure may not be what a player sees: read it as raw light.
- **"Revealed, it is"** names what the scene left out there and why, from a second record with everything put in: `[left out: hidden from every pass]`, `[left out: faded out]`, `[left out: its mesh groups hidden]`, `[dropped by the mesh rules]`.

Read these with care:

- A hidden copy can sit in front of the real mesh, so the first name is not always the one the game drew. A case on the Africa roof names a hidden model at the origin for most of a surface whose real mesh was dropped by the rules. Check with `reveal <hash>`.
- Reveal keeps dropped meshes as solid. Glass then seals windows, so light numbers under `reveal` are not the numbers of the real fix. Judge reveal by the scene and sun checks, and measure light after the fix.
- Differences that are expected: thin rails and floor grates the mesh rules drop, bushes the game's shadow map leaves out (extra shadow), the sea under the ACC's windows, and backdrops far beyond a window.

## Scenarios

They live in `tools/automation/scenarios.json`: each spot's place, why it matters and its expected numbers. When the user marks a spot worth keeping, or a fix closes an issue, add one: `fx.py scenario <name> "<why>" <case>`.

- Runs of one spot differ by a few tenths. The limits are `WORSE_*` in `case.py`; tighten or loosen them with evidence, as a limit that fails at random teaches everyone to ignore it.
- `--accept` makes a run the new expectation. Use it only after a change meant to show, and after looking at the pictures.
- Expected numbers depend on the player's settings, so a run after they change is a reason to accept, not a fault.
- A check nobody has seen fail proves little. When adding a scenario for a fix, take the fix out, run it, and see it fail.
- `via` names a free roam to pass through first, where the way in changes the frame. The ACC needs it: straight from Mother Base a visibility manager can hold the hull, which hides issue #35.

## What cannot be reached

- A story mission or a cutscene comes back only as that place in the map's free roam, without what it put there.
- The player's position is what gets restored, so a case taken in mid-air falls.
- The copy runs on a save from Oct 24, so a player's Mother Base layout or progress may differ from it.

## Dev-only code stays out of release

Everything here is left out of a release build altogether, because hooking system file calls, patching Steam's interfaces and running commands from files trips security software. CMake adds the dev-only files and the `InspectPixels` kernel with `PHANTOM_FX_DEV_TOOLS`, and what they need elsewhere stands in `#if PHANTOM_FX_DEV_TOOLS`. Add new tool code the same way, build the `release` preset, then run `check_release.py`.

## When things go wrong

| What you see | Cause |
|---|---|
| "no record came" | Nothing rendered: a freeze during loading stalls the game, or the scene is off |
| `fx.py` times out | No dev build running there, or the copy failed to start: read `PhantomFX-automation/launch.log` |
| Report says "No record of the frame" | The case's `log.txt` has the error |
| Title screen shows the cabin but a case fails | The title moves on by itself into a load. Use `fx.py --copy acc` |
| A scenario says "did not reach the view" with the eye at 0 m | Compare `before-after.png`, then the numbers, before trusting it |
| Game hangs right after new code in a draw | Read every thread's stack with Frida and `llvm-symbolizer` before guessing. Never take a lock from a present that a draw holds across Direct3D calls |
| A build seems to pass but changed nothing | Check `build/dev/PhantomFX.addon64` is newer than the sources: `if errorlevel 1` misses DXC failures |
