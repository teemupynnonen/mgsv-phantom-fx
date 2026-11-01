# Development tools

Everything a dev build has for testing in the game, and a release build does not: the overlay's Developer page and the
debug cases. `docs/DEVELOPMENT.md` describes what each does ("Debug cases", "Travel", "Cutscenes", "Game Lua").

| File | What it is |
| --- | --- |
| `dev.hpp`, `dev.cpp` | The one door from the core, and the Developer page |
| `dev.cmake`, `tests.cmake` | The folder's sources and kernel, and its tests |
| `automation.*` | Commands from files, and F9's debug cases |
| `save_redirect.*` | Keeps the automation copy off the player's files |
| `game_lua.*`, `travel.*`, `cutscenes.*`, `cutscene_list.hpp` | Lua in the game, and the tools that run on it |
| `inspect_trace.*`, `inspect.hlsl`, `inspect_shared.h` | A debug case's record of every pixel, a trace like any other |
| `game_shadow.*` | A debug case's reference: the game's shadow per pixel, drawn at the sun pass |
| `held_camera.*` | A debug case's camera, which photo mode's camera update takes |
| `perf_record.*` | What frames cost, on file: a row a second, and captures for comparing frames |
| `remaster/` | The tool that writes the rules `src/remaster/` ships: marks, and the panel that tunes them |

`tools/automation/` and `.agents/skills/mgsv-automation/` are the other side of the debug cases: the scripts that talk to
the running game and what an agent needs to know to use them.

## How it hangs from the core

The core calls in through `dev.hpp`, and through the few small headers its passes need. Each call stands behind
`if constexpr (DevTools)` (`dev_tools.hpp`), which leaves a release build nothing to link, so the headers compile in
every build and the sources in a dev build alone. A new tool goes in this folder, with its sources in `dev.cmake`.

Searching for `PHANTOM_FX_DEV_TOOLS` and `DevTools` finds every line the core keeps for the tools.

## Detaching it

Delete this folder, `tools/automation/` and `.agents/skills/mgsv-automation/`, then remove:

| File | Lines |
| --- | --- |
| `CMakeLists.txt` | The `include(src/dev/dev.cmake)` and `include(src/dev/tests.cmake)` blocks and `${PHANTOM_FX_DEV_SOURCES}` |
| `src/addon.cpp` | The three `dev::` calls, the include, and the name of `onPresent`'s `swapChain` parameter |
| `src/game_loop.cpp` | `dev::onFrameStart()` and the include |
| `src/menu.cpp` | `dev::drawPage()` and the include |
| `src/menu_budget.cpp`, `src/features/setting_visitor.hpp` | The Developer section |
| `src/remaster/remaster.cpp` | The `tool_hooks.hpp` include and the three calls of it |
| `src/passes/ray_tracing/trace.cpp` | `inspect_trace` in the list and the include |
| `src/passes/sun_light_pass.cpp` | The `game_shadow` block, its `detach` and the include |
| `src/passes/sun_light_pass_shaders.hpp` | The three `GAME_SHADOW` blocks |
| `src/performance.cpp`, `src/features/ray_traced_lights/local_lights.cpp` | The `perf_record` call and the include |
| `src/features/photo_mode/photo_mode.cpp` | The `held_camera` calls, `heldCamera` and the include |
| `src/passes/ray_tracing/ray_tracing.cpp`, `.hpp` | The `PHANTOM_FX_DEV_TOOLS` blocks: the probe's pixel and `saveTextures` |
| `src/passes/ray_tracing/gi_renderer.cpp`, `.hpp` | `waitingWork`, `workWaiting`, `Frame::probePixel`, and the `perf_record` calls with `copyCounters` and `readCounters` |
| `src/phantom_fx_settings.cpp`, `.hpp` | `withLine` |
| `docs/DEVELOPMENT.md` | The `src/dev/` rows, "Debug cases", "Travel", "Cutscenes", "Game Lua", and the mentions under "Build" |

That is 190 lines, and the add-on builds and passes its tests without them (tried 2026-11-01). The rules in
`src/remaster/` go on working from the file built into the add-on; what only the tool calls there stands in
`PHANTOM_FX_DEV_TOOLS` blocks, inert in a release build, and its tests still use it. One thing goes deeper: `reveal`, which puts what the scene leaves out back in, is
threaded through the scene's own filters. In a release build it is always empty, so it can stay. To remove it as well for
a clean tree, take out `Reveal` and `waitingFiles` in `engine_scene`, `keepsDropped` and `Rejection` in `scene_description`, and the
`PHANTOM_FX_DEV_TOOLS` blocks in `scene_contents.cpp`.
