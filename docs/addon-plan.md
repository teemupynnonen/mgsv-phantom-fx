# Add-on core plan

Status: in progress. Branch `feat/core-library`, written 2026-11-21.

Goal: a reusable rendering core, extracted from PhantomFX, that every later add-on links against. A static library,
not an injected DLL: an add-on that wants it must be built against it. No ABI, no version handshake, no load-order
coupling.

The first consumer after PhantomFX is the body-blood add-on. It is **deferred**: nothing in this plan may depend on
it landing. Build the core so it can, then stop.

Done so far: step 1 (`phantom_fx_core`, `core/order`, `core_order_tests`), step 2 (game access and utilities moved
under `src/core/`; their headers keep their names, so the add-on's includes did not change; the suites link the
library), step 3 (`Chain` runs the draw owners, `FrameLatch` lives in core) and step 4 (`core/settings` owns the file
format and its readers; `core_settings_tests` and `settings_tests` cover them). Remaining: step 5.

## Target layout

```
src/core/
  log, clock, hresult, d3d11_helpers, shader_bytecode
  order/     the slots, Chain (priorities, one winner per draw), DrawClaim
  frame_latch.hpp  the frame being recorded keeps the settings it started with
  game/      game_code, game_constants, game_lights, game_shaders, device_check
  capture/   constant_capture
  settings/  generic store, SettingVisitor
  passes/    scissor/fullscreen helpers, state scopes
src/phantomfx/     features, module table, pages
addons/phantomfx.cpp   AddonInit/AddonUninit, fxcore::initialize, registration
```

One CMake target, `phantom_fx_core` (namespace `fxcore`, so it does not collide with PhantomFX's own
`phantom_fx::core`), linked by PhantomFX and by its tests. `dev/` (automation, DevKit) stays PhantomFX-only.

Deferred to the blood add-on: the overlay/pages framework, the texture registry and `engine_scene::GameTexture`,
the blood feature itself, and the core build-hash check between DLLs. Revisit each when the second add-on exists.

## What moves into core

| Group | Lines | Contents |
| --- | --- | --- |
| Game access | 1,445 | `constant_capture` 257, `game_constants` 181, `game_shaders` 469, `shader_bytecode` 388, `game_files` 201, `game_code` 58, `device_check` 64 |
| Utilities | 456 | `log` 84, `clock` 27, `hresult` 17, `d3d11_helpers` 328 |
| Settings | ~150 | `core/settings/ini`: the file format, `key=value`, flags, clamped floats; the visitor and the per-feature structs stay with PhantomFX |
| Ordering | ~150 new | the scheduler below |

`ui_redirect` does not move: it belongs to a page, and pages are deferred.

## Render ordering

Four orderings, made explicit instead of implicit.

1. **Slots.** A feature declares the slot it works in; it cannot invent one: `FrameStart`, `GameDraw`, `ShadersBound`,
   `SceneReady`, `BeforeUpscale`, `BeforePresent`, `Overlay`.
2. **Priority inside a slot.** A small integer, stable-sorted. Replaces today's `||` chains in `addon.cpp:271` and
   `addon.cpp:303`.
3. **Single winner per draw.** A draw is claimed by at most one feature; a second claim is refused and logged.
4. **Settings latch.** `FrameLatch` moves into core; the frame being recorded never mixes two settings sets.

Rules that must survive the port:

1. The pool draws **inside** the game's decal draw, before lighting. A core that only exposes present-time hooks
   breaks it.
2. Ray-traced lights claim light draws **before** the local light pass (`addon.cpp:301`).
3. The upscaler and HDR stay at output size and in their present slot (`addon.cpp:588-591`).
4. `blood::draw` stays ahead of every ray-tracing feature in the draw chain.
5. Present order stays `core` → `upscaler` → `features` → `performance`.

## Settings split

`phantom_fx_settings.hpp` today is one `Settings` struct holding 20 feature structs. What moved, and what did not:

- **core:** `core/settings/ini` — `key=value` parsing, the file's reader, flags, clamped floats and full-text numbers.
  Nothing there knows what a setting means.
- **PhantomFX:** `SettingVisitor`, its info structs and the 20 feature structs. They describe PhantomFX's effects, so
  moving them would drag `Effect` into core for a consumer that does not exist yet. `core_settings_tests` covers the
  format; `settings_tests` moves every described setting off its default, saves, loads and compares.

The widest change in the port is therefore the test, not a rewrite: a feature that ignores its slider now fails
`settings_tests` instead of surprising the player.

## Tests

The core is the base for every later add-on, so it gets its own suites before it gets consumers. Same pattern as the
rest of the repo: `expect()` helpers, one executable per suite, CTest, no new framework. Linking the static library
also removes the source lists every test target duplicates today (`upscaler_tests` recompiles the settings and all 20
feature translation units).

1. `core_order_tests` — priority order, equal priorities keeping registration order, the first winner ending the
   list, a step added once, one winner per draw, and `FrameLatch` holding the frame's settings. This encodes the
   ordering rules; it is the check that catches a draw-chain regression without the game running.
2. `core_settings_tests` — the file format: trimming, a line without a key or an `=`, an empty value, a later key
   replacing an earlier one, flags that take only 0 and 1, floats clamped to their range, text that is not a number.
3. `settings_tests` — every setting moved off its default, saved and read back; unknown keys ignored; a missing file
   the first launch; a value outside its range refused at save.
4. Repointed, not rewritten: `shader_bytecode_tests`, `game_shaders_tests`, `kernel_table_tests` link the library and
   change include paths to `core/...`. Pure helpers (`hresult`, log formatting, game constants) get assertions.

Not tested: ReShade plumbing, device and swapchain hooks, pass helpers that need a live context. Those stay in the
in-game cases.

## Sequence

| Step | Work | Estimate |
| --- | --- | --- |
| 1 | **Skeleton core.** CMake static lib, `core/order` (`Schedule`, `DrawClaim`), `core_order_tests`, `phantom_fx` links it. ReShade event dispatch stays in `addon.cpp` until a moved handler needs it. | 0.5 day |
| 2 | **Move game access and utilities.** File moves, include fixes, tests link the lib. PhantomFX's tests stay green. | 1 day |
| 3 | **Ordering.** Slots, priorities, single-winner claims, state scopes, frame latch; convert `addon.cpp`'s chains; `core_order_tests`. | 1 day |
| 4 | **Settings split.** `core/settings/ini`, the add-on's visitors on top, `core_settings_tests` and `settings_tests`. | 1 day |
| 5 | **Regression.** Every case rerun by the player. | 1–2 days, gated on the player |

Coding: about 4 days. Verification adds one to two days, because every in-game case waits on the player.

Step 1 is not done until `ctest` runs at least one assertion through the library. Steps 3 and 4 each land with their
suite.

## Risks

1. **Settings split.** Touches all 20 features; failures look like a setting that does nothing. `core_settings_tests`
   exists for this.
2. **Duplicated hot paths.** Constant capture runs once per loaded add-on. Acceptable for one; measure the frame cost
   when the second arrives.
3. **Draw-chain reordering** changes the pool's decal behaviour. Keep ordering rule 1 and rerun one shot, one pool, one
   body after the port.
4. **Blood is deferred, not cancelled.** The texture registry is still an unresolved unknown; price it when that work
   restarts, not now.

## Definition of done

1. PhantomFX loads and runs from the core, with the same features, settings and screenshots as before the split.
2. One shot, one wound, one pool, one body: the same log lines and the same picture as before the split.
3. The pool draws in the decal pass, not at present.
4. Ray-traced lights still claim their draws before the local light pass.
5. `ctest` runs `core_order_tests` and `core_settings_tests` through `phantom_fx_core`.
6. Settings apply, and the ini still round-trips every key.
