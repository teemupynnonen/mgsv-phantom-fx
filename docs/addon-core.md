# Building an add-on on the core

`phantom_fx_core` is the part of Phantom FX that more than one add-on uses. It is a static library: each add-on links
it and carries its own copy. There is no shared DLL, no ABI, no version handshake and no load-order coupling. An
add-on that was not built against the core cannot use it, and two add-ons never share state through it.

PhantomFX is the first consumer. A second one is expected; anything the core does not do yet is listed at the end.

## A new add-on in this repository

1. Put ReShade's exports in `addons/<name>.cpp`: `NAME`, `DESCRIPTION`, `AddonInit`, `AddonUninit`.
2. Give it a shared library target whose name ends `.addon64`, and link the core:

```cmake
add_library(my_addon SHARED addons/my_addon.cpp src/...)
target_link_libraries(my_addon PRIVATE phantom_fx_core)
target_compile_options(my_addon PRIVATE /W4 /WX /permissive- /utf-8)
```

3. Nothing else is needed. `phantom_fx_core` publishes:

| What | Value |
| --- | --- |
| Include directories | `src`, `src/core`, `src/core/game`, `src/core/capture` |
| Language | C++20 |
| Definitions | `WIN32_LEAN_AND_MEAN`, `NOMINMAX` |
| Libraries | `d3d11`, `d3dcompiler`, `minhook` |

4. `AddonInit` and `AddonUninit` stay yours. The core registers no ReShade events, draws no overlay and opens no
   log by itself.

Two add-ons replace different things. ReShade delivers events in load order and offers no priorities between add-ons,
so there is no ordering to rely on across DLL boundaries.

## Namespaces

- `fxcore` — the core's own: `Slot`, `Chain`, `DrawClaim`, `FrameLatch`, `fxcore::settings`.
- `phantom_fx` — code that moved out of the add-on unchanged: the log, the clock, `check`, the Direct3D helpers,
  `shader_bytecode`, the game's constants, lights and shaders, `device_check`, `constant_capture`.

The moved files kept their names, so `#include "game_shaders.hpp"` or `#include "log.hpp"` resolves in either add-on.
New core code is included by path: `#include "core/order.hpp"`, `#include "core/settings/ini.hpp"`.

## What is in it

| Header | What it gives |
| --- | --- |
| `core/order.hpp` | `Slot`, the slots a feature can work in; `Chain`, the steps of one slot in priority order; `DrawClaim`, one owner per draw |
| `core/frame_latch.hpp` | `FrameLatch<T>`: the settings a frame started with, so a frame never mixes two settings sets |
| `core/settings/ini.hpp` | The settings file format: `Values`, `read`, `parseLine`, `trim`, `number<T>`, `readFlag`, `readFloat` |
| `core/log.hpp` | `PhantomFX.log` beside the add-on, `log`, `hex`, `logOnce`; one file per process |
| `core/clock.hpp` | `QueryPerformanceCounter` ticks and milliseconds |
| `core/hresult.hpp` | `check`: a failed Direct3D or Windows call as an exception naming the step |
| `core/d3d11_helpers.hpp` | Shader compiles, dynamic constant buffers, compute textures, texture sizes |
| `core/shader_bytecode.hpp` | DXBC checksum, the instruction walk, the output saturation and dither patches |
| `core/game/game_code.hpp` | The game's image, code checks, hooks and patches, on one MinHook instance |
| `core/game/game_constants.hpp` | The game's constant buffers by name, and a light pass's view space |
| `core/game/game_shaders.hpp` | The game's pixel and vertex shaders in one table, and `DrawnShader`, a draw's lookup |
| `core/game/game_lights.hpp` | What a point or spot light shader is: kind, falloff, twin, range |
| `core/game/device_check.hpp` | Lets the game start under ReShade, and the build check |
| `core/capture/constant_capture.hpp` | What the game's threads last wrote to the constant buffers, read by type |

## The order a frame runs in

A feature declares the slot it works in and a priority inside it. Lower priority runs first; equal priorities keep the
order they were added in. The first step that takes the work ends the list, so a draw is drawn in the game's place
exactly once.

```cpp
using DrawOwners = fxcore::Chain<ID3D11DeviceContext*, const ui_redirect::DrawCall&, game_shaders::DrawnShader&>;
DrawOwners owners;

void registerDrawOwners()
{
    owners.add(10, "atmosphere", atmosphere::draw);
    owners.add(20, "hdr", hdr::draw);
    owners.add(30, "upscaler", upscaler::draw);
}

// In the draw event:
const bool drawn = owners.run(context, call, shader) != nullptr;
```

`DrawClaim` covers work claimed outside a chain: `take(owner)` is true for the first owner, false for a second, and
the caller logs the refusal. `clear()` starts the next draw.

The slots: `FrameStart`, `GameDraw` (the game's draws, before lighting), `ShadersBound`, `SceneReady`,
`BeforeUpscale`, `BeforePresent`, `Overlay`. A feature that needs an ordering the slots do not express is a reason to
add a slot, not to invent one locally.

## Settings

The core owns the file format, not the settings. Each add-on keeps its own structs and applies the values itself:

```cpp
std::ifstream input(directory / "MyAddon.ini");
const auto values = fxcore::settings::read(input);   // throws when the stream fails
for (const auto& [key, text] : values)
    apply(key, text);                                 // invalid text leaves the setting as it was
```

`readFlag` takes only `0` and `1`; `readFloat` clamps to the range the caller gives; `number<T>` takes the whole text.
PhantomFX's `SettingVisitor` is PhantomFX's: it describes PhantomFX's effects, and it is not part of the core.

## Threads and lifetime

- `FrameLatch<T>` locks: settings arrive on the overlay's thread, the frame's copy is taken at present, and the
  recording reads that copy.
- `log` is safe to call from any thread.
- There is no other shared state. Two add-ons each have their own copy of everything, including the log's file handle
  and any caches in `game_shaders` or `constant_capture`.

## What the core does not provide yet

- ReShade event dispatch: each add-on registers its own events.
- The overlay and its pages.
- The texture registry (`GameTexture`, `gameTextureOf`, `currentLevelOf`).
- Ordering between add-ons, or a build hash that refuses a mismatch between DLLs.

Each waits for the second add-on to need it. `docs/addon-plan.md` says what was deferred and why.

## Testing

Tests follow the rest of the repository: `expect()` helpers, one executable per suite, CTest, no framework. Link the
library rather than listing its sources:

```cmake
add_executable(my_tests tests/my_tests.cpp)
target_link_libraries(my_tests PRIVATE phantom_fx_core)
target_compile_options(my_tests PRIVATE /W4 /WX /permissive- /utf-8)
add_test(NAME my COMMAND my_tests)
```

Keep tests to what runs without the game: ordering, settings parsing, bytecode, formats. Anything that needs a device
or the game's memory belongs in an in-game case.
