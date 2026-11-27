# Devkit

What a dev build of another MGSV add-on can build on: the tools that do not need PhantomFX to mean anything.
The add-on sets its name, its logger and its toasts first, links `devkit` in its dev preset alone, and calls in
from its own hooks. This repository also builds it as the separate `DevKit.addon64` (`../devaddon/README.md`),
which is the add-on to run when PhantomFX's own dev build is not.

| File | What it is |
| --- | --- |
| `host.*` | The add-on's name, in Lua chunk names, and where its toasts go |
| `log.*` | One logger function pointer, so the devkit writes to the add-on's log |
| `commands.*` | Commands from files: the runner, its waits, and the present thread's jobs |
| `save_redirect.*` | An automated run's saves, Steam Cloud and user data folder kept off the player's |
| `game.hpp` | The game's image and code checks, and the build check the add-on answers |
| `game_lua.*` | Lua in the game's own script state: expressions, print(), and the console's queue |
| `travel.*`, `cutscenes.*`, `cutscene_list.hpp` | Free roam in another map, and any cutscene the game's scripts name |
| `keys.*`, `gamepad.*` | Key presses to the game's window, and a synthetic gamepad button merged into XInput |
| `frame_log.*` | The game's shaders (kept, searched, disassembled, saved) and the frame recorder: every draw with its shaders, targets and states, dumps of constants and textures |
| `automation.*`, `extension.hpp` | The runner's lines and the API an add-on plugs its own into |
| `page.*` | The devkit's own Developer page: travel, cutscenes, shaders and the Lua console in plain ImGui |

`devkit.cmake` builds the static library. The add-on answers `devkit::game::isSupportedBuild()` (PhantomFX:
`src/dev/dev.cpp`, with the core's `game/device_check.cpp`). The devkit also needs the core (`src/core`,
`phantom_fx_core`). `src/dev/` stays PhantomFX's own: its Developer page with the debug cases and the probes into its
passes. The devkit's page is plain ImGui, so it looks like ReShade's other windows.

## Using it in another add-on

1. Bring `src/devkit/` and the core into the add-on's tree (or make this repository a submodule and add its `src`
   to the include path), and build `devkit.cmake` as a static library in the dev preset alone. Link it only there.
   `devkit.cmake` links `phantom_fx_core` and MinHook.
2. Answer the host side before installing anything:
   ```cpp
   devkit::setName("MyMod");   // Lua chunk names the game's errors show
   devkit::setLogger(&myLog);  // void(std::string_view)
   devkit::setToast([](devkit::Tone tone, std::string_view text, std::string_view detail) { myToast(tone, text, detail); });
   namespace devkit::game
   {
   bool isSupportedBuild() noexcept { return myBuildCheck(); }
   }
   ```
3. Hook it into the add-on's own frame:
   - `devkit::commands::install(folder, run, waiting, started)` once; `onFrameStart` from the game thread and
     `onPresent` from the render thread.
   - `run` gets each line and dispatches the add-on's commands; `wait <frames>` is the runner's own. `waiting`
     returns true while a line's work (a scene, a capture) is unfinished, and `started` resets that state for a
     new file.
   - Work for the render thread: `devkit::commands::askPresent([](context, backBuffer) { ...; return "saved\n"; })`;
     the runner waits and appends the result.
   - The add-on's own key can call `devkit::commands::start({"line", "line"})`.
4. Use the tools directly: `game_lua::run` and its console, `travel::go`, `cutscenes::play`,
   `save_redirect::install`; the first three each have an `onFrameStart` to call from the game thread. Paths for
   command results are `devkit::commands::folder()`. `frame_log.*` is in the `devkit` library but also needs
   `src/resource_dump.*`, which `devkit.cmake` does not compile: the add-on that links it supplies it.
5. Draw the devkit's page from the overlay callback, inside the window ReShade gives it:

   ```cpp
   reshade::register_overlay("MyMod", [](reshade::api::effect_runtime*) { devkit::page::draw(); });
   ```

   It uses ReShade's ImGui function table (`imgui.h` and `reshade_overlay.hpp`) and no `fxui`, so it looks like
   ReShade's other windows. The host can also call `draw()` inside its own settings window.

6. Or skip steps 3 and 4: run `DevKit.addon64` beside the add-on and plug the add-on's own lines in
   (`devkit/extension.hpp`, nothing to link). Find `DevKitGetApi` in the module, then:

   ```cpp
   const auto* kit = getApi();   // null until DevKit has loaded: try again at the first frames
   kit->addCommand("hello", [](DevKitScript* script, const char*, void* user) { static_cast<const DevKitApi*>(user)->say(script, "hi\n"); }, const_cast<DevKitApi*>(kit));
   kit->addState(&myState, nullptr);   // text for `state` and `status`
   kit->setKey(&myF9, nullptr);        // F9: usually kit->start("line\nline")
   ```

   A line can `say` its output, `queue` more lines to run next, `waitUntil` a function says it is done, or hand the
   render thread a `presentJob`. Plain C types and caller buffers only: each add-on has its own CRT.
   `src/dev/devkit_link.cpp` is PhantomFX doing it.

PhantomFX keeps its own page (`src/dev/dev.cpp`, drawn with `fxui`) for its probes and costs; the DevKit add-on
draws `devkit::page::draw()` instead, and so does another add-on that links the devkit. What stays PhantomFX's:
that page, the debug cases, and the probes into its passes (inspect, perf record, game shadow, held camera,
remaster).

Release: link `devkit` only when the dev tools are on, and keep the dev-only strings listed in
`tools/automation/check_release.py` current; it reads the built add-on and fails if any reached it.
