# Development tools

Everything a dev build has for testing in the game, and a release build does not: the overlay's Developer page and the
debug cases. `docs/DEVELOPMENT.md` describes what each does ("Debug cases", "Travel", "Cutscenes", "Game Lua").
Travel, cutscenes and the Lua console are the **DevKit add-on's** page (`../devaddon/README.md`); this page keeps
PhantomFX's own probes.

| File | What it is |
| --- | --- |
| `dev.hpp`, `dev.cpp` | The one door from the core, and the Developer page |
| `dev.cmake`, `tests.cmake` | The folder's sources and kernel, and its tests |
| `../dev_tools.hpp` | `DevTools`, and `ToolsOnly`: the `tools` preset, which keeps these tools and leaves the game's frame as it is |
| `devkit_link.cpp` | PhantomFX's lines in the DevKit add-on's command files, plugged in through `../devkit/extension.hpp`: DevKit owns the runner, the waits and F9 (`../devkit/automation.*`) and knows nothing of these |
| `../devkit/` | What another MGSV add-on can build on: the saves redirection, Lua in the game, travel, cutscenes, its own Developer page in plain ImGui, and the logger and toasts the core sets |
| `inspect_trace.*`, `inspect.hlsl`, `inspect_shared.h` | A debug case's record of every pixel, a trace like any other |
| `game_shadow.*` | A debug case's reference: the game's shadow per pixel, drawn at the sun pass |
| `held_camera.*` | A debug case's camera, which photo mode's camera update takes |
| `perf_record.*` | What frames cost, on file: a row a second, and captures for comparing frames |
| `hot_kernels.*` | The ray tracing's Direct3D 12 kernels, recompiled and swapped in as their sources are saved |
| `remaster/` | The tool that writes the rules `src/remaster/` ships: marks, and the panel that tunes them |

`tools/automation/` and `.agents/skills/mgsv-automation/` are the other side of the debug cases: the scripts that talk to
the running game and what an agent needs to know to use them.

## How it hangs from the core

The core calls in through `dev.hpp`, and through the few small headers its passes need. Each call stands behind
`if constexpr (DevTools)` (`dev_tools.hpp`), which leaves a release build nothing to link, so the headers compile in
every build and the sources in a dev build alone. A new tool goes in this folder, with its sources in `dev.cmake`, or in
`../devkit/` when another add-on can use it.

Searching for `PHANTOM_FX_DEV_TOOLS` and `DevTools` finds every line the core keeps for the tools.

## Detaching it

Delete this folder, `src/devkit/`, `src/devaddon/`, `tools/automation/` and `.agents/skills/mgsv-automation/`, then remove:

| File | Lines |
| --- | --- |
| `CMakeLists.txt` | The `include(src/dev/dev.cmake)` and `include(src/dev/tests.cmake)` blocks, `include(src/devkit/devkit.cmake)`, `include(src/devaddon/devaddon.cmake)`, `target_link_libraries(phantom_fx PRIVATE devkit)` and `${PHANTOM_FX_DEV_SOURCES}` |
| `src/addon.cpp` | The `hot_kernels::stop()` call and its include; the `dev::install` call and its include, and the Enter `photo_mode::teleport()` call |
| `src/game_loop.cpp`, `src/game_loop.hpp` | `dev::onFrameStart()` and the include; the frame-start callbacks (`registerFrameStart`, its array and mutexes, the dispatch block and the `PhantomFXRegisterFrameStart` exports), all behind `#if PHANTOM_FX_DEV_TOOLS` |
| `src/menu.cpp` | `dev::drawPage()` and the include |
| `src/menu_budget.cpp`, `src/features/setting_visitor.hpp` | The Developer section |
| `src/remaster/remaster.cpp` | The `tool_hooks.hpp` include and the four calls of it |
| `src/passes/ray_tracing/trace.cpp` | `inspect_trace` in the list and the include |
| `src/passes/sun_light_pass.cpp` | The `game_shadow` block, its `detach` and the include |
| `src/passes/sun_light_pass_shaders.hpp` | The three `GAME_SHADOW` blocks |
| `src/performance.cpp`, `src/features/ray_traced_lights/local_lights.cpp` | The `perf_record` call and the include; in `local_lights.cpp` also `dev::recordingDirectory()` and `dev/dev.hpp` |
| `src/features/photo_mode/photo_mode.cpp` | The `held_camera` calls, `heldCamera` and the include; the `game_lua` include, `teleportRequested`, the `Warp` state, its capture at `selectorUpdate`'s teleport and the clears `leave` makes, and `onFrameStart`'s teleport warp |
| `src/features/photo_mode/photo_mode.hpp` | The `teleport()` and `onFrameStart()` declarations |
| `src/passes/ray_tracing/ray_tracing.cpp`, `.hpp` | The `PHANTOM_FX_DEV_TOOLS` blocks: the probe's pixel and `saveTextures` |
| `CMakeLists.txt` (kernels) | `GI_KERNEL_SOURCES` in `add_gi_kernels`, and the `hot_kernels` block that writes `gi_hot_kernels.h` |
| `src/passes/ray_tracing/gi_renderer.cpp`, `.hpp` | The hot reload block at the start of `render` (`hot_kernels::start`, `take`), `waitingWork`, `workWaiting`, `Frame::probePixel`, the `perf_record` calls with `countersReadback`, `countersWritten` and `phaseTimes`, and the `dev::recordingDirectory()` block with `dev/dev.hpp` |
| `src/phantom_fx_settings.cpp`, `.hpp` | `withLine` and `iniText` |
| `docs/DEVELOPMENT.md` | The `src/dev/` rows, "Debug cases", "Travel", "Cutscenes", "Game Lua", and the mentions under "Build" |

That is about 220 lines, and the add-on builds and passes its tests without them (tried 2026-11-01, at 190, before DevKit was split
out). The rules in `src/remaster/` go on working from the file built into the add-on; what only the tool calls there stands in
`PHANTOM_FX_DEV_TOOLS` blocks, inert in a release build, and its tests still use it. One thing goes deeper: `reveal`, which puts what the scene leaves out back in, is
threaded through the scene's own filters. In a release build it is always empty, so it can stay. To remove it as well for
a clean tree, take out `Reveal` and `waitingFiles` in `engine_scene`, `keepsDropped` and `Rejection` in `scene_description`, and the
`PHANTOM_FX_DEV_TOOLS` blocks in `scene_contents.cpp`.
