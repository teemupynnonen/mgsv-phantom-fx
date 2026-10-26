# Phantom FX development

## Build

Requires Visual Studio 2022 (or Build Tools) with the C++ workload and CMake 3.25 or later.
From a x64 developer prompt:

```
cmake --preset release
cmake --build --preset release
```

CMake downloads MinHook, the DLSS SDK v310.9.1 headers, loader and `nvngx_dlss.dll`, and the
ReShade 6.8.0 add-on headers with the Dear ImGui header, each pinned by commit and checked by
SHA-256. Frame generation needs the Streamline SDK 2.14.1, which only its release archive carries:
the first configure downloads it once (about 280 MB, checked by SHA-256) and keeps the headers,
license and x64 runtime files. FSR needs the AMD FSR SDK 2.3.0's API headers and AMD's signed
Direct3D 12 DLLs, pinned by commit and checked by SHA-256 like the rest (about 66 MB). The add-on is
written to `build/release/PhantomFX.addon64` together with `nvngx_dlss.dll`, Streamline's `sl.*.dll`
and `nvngx_dlssg.dll`, and AMD's `amd_fidelityfx_*_dx12.dll`.
It builds with `/W4 /WX`. The version comes from `project()` in `CMakeLists.txt`: it fills the
add-on's version resource (`src/version.rc.in`), which ReShade shows and logs, and the first line
of `PhantomFX.log`.

### The diagnostic log

The `release` preset is what players get: `PhantomFX.log` holds only what happens once (startup,
the game build, sizes, what was identified, failures, and NGX's and Streamline's own messages). The
`dev` preset (`cmake --preset dev`, `cmake --build --preset dev`, into `build/dev`) sets
`PHANTOM_FX_DIAGNOSTIC_LOG`, which adds the statistics logged every few seconds and the work that
only feeds them (`DiagnosticLog` in `src/log.hpp`, tested with `if constexpr`):

- the global illumination's light, rays, largest values and cost (the output and counters copied
  back from the GPU every ten seconds), its lights and exposures, its scene, its characters' bones
  and its CPU time a frame, with the time of every draw hook (`gi_inputs`' `HookTimer`);
- the ray-traced lights' "Lights in ten seconds";
- the physical sky's parameters, copied back from the GPU every ten seconds;
- the upscaler's frame counts every 1800 presents;
- the shadow probe's findings, while its debug view shows;
- Developer, **Log global illumination often**.

Its log's first line says "with the diagnostic log". Builds for in-game checks use this preset.
It also sets `PHANTOM_FX_DEV_TOOLS` (`DevTools` in `src/dev_tools.hpp`), which adds the overlay's
**Developer** page, below Diagnostics: **Travel** (see "Travel"), the **Lua console** (see "Game
Lua") and the log switch above.

`cmake --install build/release --component addon --prefix <dir>` collects the add-on, its DLLs,
player documentation and licenses for distribution. The DLLs are at the install root;
documentation and licenses are in `PhantomFX/`.

## Test

```
ctest --test-dir build/release
```

This runs `tests/upscaler_tests.cpp`, which needs neither the game nor a GPU: settings (defaults,
round trip, clamping, invalid files and values), the mode maths (render scales, texture LOD bias)
and the camera maths (main-view recognition, jitter direction and units, reprojection, camera cuts,
jitter phases and the Halton sequence, the camera frame generation gets). Everything else is checked
in the game.

## Package a release

From the repository root, after building and testing:

```
cpack --config build/release/CPackConfig.cmake -B build/release
```

This produces `build/release/PhantomFX-<version>.zip` and its `.sha256` checksum:

| Path in the ZIP | What |
| --- | --- |
| `PhantomFX.addon64`, `nvngx_dlss.dll` | The add-on and the DLSS model, beside `mgsvtpp.exe` |
| `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `nvngx_dlssg.dll` | NVIDIA Streamline and the frame generation model |
| `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`, `amd_fidelityfx_framegeneration_dx12.dll` | AMD FSR upscaling and frame generation |
| `PhantomFX/README.md`, `CHANGELOG.md`, `LICENSE` | Player documentation and the add-on's license |
| `PhantomFX/*-LICENSE.txt` | NVIDIA DLSS, NVIDIA Streamline, NVIDIA NRD (global illumination's denoiser), AMD FSR SDK, MinHook, ReShade, Dear ImGui, AMD FidelityFX (RCAS), Bend Studio (screen-space shadows), three-gtvbao and Intel XeGTAO (GTVBAO), AMD Capsaicin (global illumination) |

CPack packages only the `addon` component, so SDK headers, static libraries, tests, debug symbols
and local settings are left out. ReShade is installed separately by the player.

## Sources

| File | Purpose |
| --- | --- |
| `src/upscaler_addon.cpp` | ReShade add-on: exports, events, build line, MinHook set-up, NGX's wrapped context |
| `src/upscaler.cpp`, `src/upscaler.hpp` | DLSS or FSR before the grading, in place of FXAA: pass recognition, jitter, frames, render size plan, NGX, composite, the grading at the output size, texture LOD bias |
| `src/render_scale.cpp`, `src/render_scale.hpp` | Hook on the renderer's per-frame step: the engine renders at the requested size |
| `src/ui_redirect.cpp`, `src/ui_redirect.hpp` | The grading, the 2D pass and `GammaControl` on output-size targets while the engine renders below them |
| `src/upscaler_shaders.hpp` | HLSL: the upscaler's inputs and motion vectors, composite with RCAS and debug views, stretch fallback |
| `src/camera.cpp`, `src/camera.hpp` | Scene constants: main-view recognition, jitter, reprojection, camera cuts |
| `src/upscaler_settings.cpp`, `src/upscaler_settings.hpp` | Upscaler, mode, custom scale, preset, texture bias, sharpening, frame rate unlock, frame generation, soft shadows, sun and lamp contact shadows, ambient occlusion, indirect lighting, volumetric fog, draw distance; `PhantomFX.ini` |
| `src/upscaler_menu.cpp` | The "Phantom FX" overlay window: its pages (Upscaling, Shadows, Lighting, Atmosphere, Camera, Display, World, Diagnostics) |
| `src/menu_budget.cpp`, `src/menu_budget.hpp` | The window's sections, and each effect as a budget line: its state, its switch and its timed passes |
| `src/fxui/` | fxui, the reusable frame budget settings window for ReShade add-ons (see its README.md) |
| `src/performance.cpp`, `src/performance.hpp` | The passes' GPU costs: timestamp queries, the averages, the real cost measurement, `PhantomFX-performance.txt` (see "Costs") |
| `src/performance_menu.cpp` | The budget page, the costs over the game, and the costs as text |
| `src/device_check.cpp`, `src/device_check.hpp` | Lets the game start under ReShade (see below); the build check |
| `src/game_code.hpp` | The game's image, checks of its code, hooks (MinHook) and code patches |
| `src/motion_blur.cpp`, `src/motion_blur.hpp` | Hooks on the motion blur plugin: velocity passes every frame, no blur |
| `src/fxaa.cpp`, `src/fxaa.hpp` | Hook on the FXAA plugin: the FXAA pass at any post processing setting |
| `src/frame_rate.cpp`, `src/frame_rate.hpp` | Hook on the frame wait setter: the optional 60 fps unlock |
| `src/presenter.cpp`, `src/presenter.hpp` | Frame generation: the game's stand-in swap chain, presented through Direct3D 12 |
| `src/frame_generation.cpp`, `src/frame_generation.hpp` | DLSS Frame Generation: Streamline, its inputs and camera, Reflex |
| `src/fidelityfx.cpp`, `src/fidelityfx.hpp` | Loads the AMD FSR API |
| `src/fsr_upscaler.cpp`, `src/fsr_upscaler.hpp` | FSR upscaling on Direct3D 12 for the upscaler |
| `src/fsr_frame_generation.cpp`, `src/fsr_frame_generation.hpp` | FSR frame generation: AMD's swap chain and its inputs |
| `src/game_loop.cpp`, `src/game_loop.hpp` | Hooks on the main loop's frame start and end, for Reflex, the weather menu and the draw distance |
| `src/draw_distance.cpp`, `src/draw_distance.hpp` | Draw distance: wider streaming windows at a location's load, the level of detail and the models' and vegetation's cull lengths (see "Draw distance") |
| `src/weather.cpp`, `src/weather.hpp` | The overlay's weather and time control, in Utility (see "Weather and time") |
| `src/sun_movement.cpp`, `src/sun_movement.hpp` | The sun and moon move every frame instead of in the game's steps (see "Sun movement") |
| `src/dev_tools.hpp` | Whether the build has the Developer page (the dev preset) |
| `src/game_lua.cpp`, `src/game_lua.hpp` | Lua in the game's script state, and the Developer page's console (see "Game Lua") |
| `src/travel.cpp`, `src/travel.hpp` | The Developer page's travel: free roam in another map (see "Travel") |
| `src/shadows.cpp`, `src/shadows.hpp` | Soft shadows and Bend Studio's screen-space shadows on the sun and the lamps: the traces and the light passes' replacement draws |
| `src/shadows_shaders.hpp` | HLSL: Bend's trace ported to FXC, the game's sun light shaders rebuilt with soft shadows and the screen-space shadow |
| `third_party/bend_sss/bend_sss_cpu.h` | Bend Studio's dispatch layout, unchanged (Apache License 2.0) |
| `src/gi_scene.cpp`, `src/gi_scene.hpp` | The ray-traced scene on Direct3D 12: the geometry pool, bottom-level structures (a geometry per mesh, compacted), the geometry table, the frame's instances and top level (see "Ray-traced scene") |
| `src/engine_scene.cpp`, `src/engine_scene.hpp` | The game's loaded models from its visibility managers, its characters from its model list, their FMDL meshes and base textures, its texture manager, the terrain's heightmap inputs (see "Scene objects") |
| `src/gi_inputs.cpp`, `src/gi_inputs.hpp` | What the ray tracing takes from the game's draws: the constants it writes (also the screen-space shadows' light pass constants), the main camera of the frame drawn, the terrain's inputs |
| `src/engine_brush.cpp`, `src/engine_brush.hpp` | The game's object brush: its loaded blocks' vegetation and props, read from their placement records (see "Scene objects") |
| `src/game_memory.hpp` | Guarded reads of the game's objects |
| `src/gi_textures.cpp`, `src/gi_textures.hpp` | Shared copies of the game's textures the ray-traced scene's meshes use, from its texture manager (see "Ray-traced scene") |
| `src/gi_light.cpp`, `src/gi_light.hpp` | The global illumination's light on Direct3D 12: its memory and passes (see "Global illumination") |
| `src/gi_denoiser.cpp`, `src/gi_denoiser.hpp` | NVIDIA NRD's RELAX denoiser on Direct3D 12, without NRI: root signature, pipelines, textures, descriptors, states |
| `src/shaders/gi_light.hlsl` | HLSL: the light's rays, their hits' light, the hash grid cache, the denoiser's inputs and the render size |
| `src/gi_shadow.cpp`, `src/gi_shadow.hpp` | Ray-traced shadows on the global illumination's Direct3D 12 device: the denoiser's memory and passes |
| `src/local_lights.cpp`, `src/local_lights.hpp` | Ray-traced lights: the game's point and spot light draws kept for their stencil, their light drawn again at the sun pass's list with specular and the rays' shadow (see "Ray-traced lights") |
| `src/game_lights.hpp` | The game's point and spot light shaders and their constants, for lamp contact shadows and the ray-traced lights: the checksums, a light's range, its sphere on screen |
| `src/shaders/gi_shadow.hlsl`, `gi_shadow_filter.hlsl`, `gi_shadow.hlsli` | HLSL: the sun's shadow rays, and the FidelityFX Shadow Denoiser's data access, temporal pass and filters |
| `third_party/ffx_denoiser_shadows/` | AMD's FidelityFX Shadow Denoiser headers, unchanged (MIT; `licenses/AMD-FidelityFX-LICENSE.txt`) |
| `src/indirect_light.cpp`, `src/indirect_light.hpp` | GTVBAO in place of the game's SSAO, on the indirect light, and SSILVB's bounce light: pass recognition, the compute passes, the multiply, the resolve's white occlusion and diffuse light |
| `src/indirect_light_shaders.hpp` | HLSL: the visibility bitmask march (GTVBAO from three-gtvbao, SSILVB's light as SSRT3 gathers it), XeGTAO's noise and denoiser, the multiply, the diagnostics views |
| `src/atmosphere.cpp`, `src/atmosphere.hpp` | Volumetric fog: the larger fog texture, the game's fog pass over all of it, the shadowed fill before the resolve |
| `src/atmosphere_shaders.hpp` | HLSL: the game's fog formula per froxel with the sun shadowed, the sum into the game's fog texture; Hillaire's atmosphere tables, the physical fog and sky (Epic Games' reference, MIT) |
| `src/photo_mode.cpp`, `src/photo_mode.hpp` | Photo mode (F2): the game's pause, a free camera through the camera selector, the game's input held back; hiding the UI (F3) |
| `src/frame_log.cpp`, `src/frame_log.hpp` | Development diagnostics, off by default: shader log, F10 frame record, constant and texture dumps |
| `src/resource_dump.cpp`, `src/resource_dump.hpp` | Format names, constant rows, DDS saving for the dumps |
| `src/hdr.cpp`, `src/hdr.hpp` | HDR10 output: the swap chain, the float scene targets and their sRGB views, the shader patches, the grading and output passes (see "HDR") |
| `src/hdr_shaders.hpp` | HLSL: the encode and decode of the float scene, bloom's reduction, the grading, the output to PQ |
| `src/shader_bytecode.cpp`, `src/shader_bytecode.hpp` | DXBC checksum (bytes 4 to 20 of the container), the instruction walk, the output saturation patch |
| `src/log.cpp`, `src/log.hpp` | `PhantomFX.log` beside the add-on, one per game session |
| `src/clock.hpp` | `QueryPerformanceCounter` ticks and the milliseconds between them |
| `src/hresult.hpp` | `check`: a failed Direct3D or Windows call as an exception naming the step |
| `src/d3d11_helpers.hpp` | Direct3D 11 helpers for the effects: shader compiles, dynamic constant buffers, compute textures, texture sizes |
| `src/d3d12_helpers.hpp` | Direct3D 12 helpers for the global illumination, FSR and the presenter: barriers, the device on Direct3D 11's GPU behind ReShade's proxy, shared textures and fences opened in Direct3D 12 |
| `src/version.rc.in` | Version resource template: name, author and version from `CMakeLists.txt` |

## Game version

Steam app 287700, `mgsvtpp.exe`, title screen "Ver. 1.15", file version 1.0.15.4,
`version_info.txt` `Tpp_steam_mst_en_day3900Mgo_patch_0707_1632`. The add-on logs the build line:

| Field | Value |
| --- | --- |
| TimeDateStamp | `0x6A4CB898` (2026-07-07) |
| SizeOfImage | `0xA01D000` |
| Image base | `0x140000000`, no ASLR, so the RVAs below are fixed for this build |

Direct3D 11 at feature level 11.0. The game creates 24 deferred contexts and records almost every
draw on them; the immediate context mostly executes command lists.

## Install for testing

Into the game folder (`steamapps\common\MGS_TPP`), beside `mgsvtpp.exe`:

| File | From |
| --- | --- |
| `dxgi.dll` | `ReShade64.dll` from the official `ReShade_Setup_6.8.0_Addon.exe` (6.8.0.2155, add-on build; the setup is a zip) |
| `ReShade.ini` | Minimal: no effects, Generic Depth and Effect Runtime Sync disabled |
| `PhantomFX.addon64` | This build |
| `nvngx_dlss.dll` | This build (the DLSS SDK's, copied beside the add-on) |

ReShade writes `ReShade.log`, the add-on `PhantomFX.log` and `PhantomFX.ini`. Uninstall by deleting
these files.
The game can be started directly while Steam runs (`steam_appid.txt` is present), with the game
folder as working directory.

The graphics settings live in `Steam\userdata\<id>\287700\local\TPP_GRAPHICS_CONFIG` (JSON;
`window_mode`, `width`, `height`, `motion_blur_amount`, ...). Editing it by hand works.

## Start-up device check

**Symptom.** With any Direct3D wrapper (ReShade as `dxgi.dll` or `d3d11.dll`, with or without
add-ons, or RenderDoc) the game exits silently about 1.3 s after start: no window, no error, no
crash report. ReShade's log shows the device and the 24 deferred contexts, no swap chain, then
"Exiting". Without a wrapper the game starts normally.

**Cause.** After creating the device, the renderer start-up (`0x2BA1B0`) calls `0x2B9BA0` at
`0x2BA60B`. That function enumerates the process modules (`K32EnumProcessModules`,
`GetModuleBaseNameA`), finds `d3d11.dll` (string at `0x21178F0`), takes its range
(`GetModuleInformation`) and checks 97 vtable entries, from vtable + 0x18, of a Direct3D object
reached through the global at `0x2B9D0A8` (`[[[g]+8]]`). Wrapper objects have their vtables in
the wrapper's DLL, so it returns false and the caller (`je 0x2BAA37`) quits through
`PostQuitMessage` (`0x7082A`), and `WinMain` returns 0. Traced with Frida: `ExitProcess(0)` from
the CRT, `PostQuitMessage` from `mgsvtpp.exe+0x7082A`, under both ReShade and RenderDoc.
It is a local check; nothing is sent anywhere.

**Fix.** ReShade loads add-ons inside `D3D11CreateDevice`, before the check runs (with Special K,
see "Special K"). `passDeviceCheck()` (`src/device_check.cpp`) verifies the build and the function's
first bytes (`40 53 48 81 EC 60 04 00 00`) and writes `mov al, 1; ret` (`B0 01 C3`) over its start,
in memory only. The game file is never changed. Without the add-on, ReShade alone cannot run the game.

## Special K

Checked with Special K 26.10.10 injected by SKIF, ReShade as `dxgi.dll`. Special K registers itself
as a ReShade add-on and sits below ReShade's DXGI proxies.

**Start-up order.** Special K's hook set-up makes a Direct3D 11 device on its own thread, 2 to 3 ms
after it logs "Installing Deferred DXGI / D3D11 / D3D12 Hooks", before the game makes its own.
ReShade loads add-ons inside that call, so `AddonInit` ran beside the game's start-up: in one run
the frame generation hook and HDR's handlers came after the game's swap chain, and the add-on took
Special K's immediate context for the game's. Special K then makes a swap chain of its own through
DXGI's `CreateSwapChain`, which the frame generation hook sits on. Now a device made on another
thread waits in `create_device` until `AddonInit` is done (at most 10 s), and the immediate context
and the swap chain frame generation replaces are only taken when the game's code is on the stack
(`game_code::calledFromGame`). Without Special K the game's own thread runs `AddonInit` and
nothing waits.

**Tearing flag.** Special K makes the game's swap chain with `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`
(its `AllowTearingInDWM`). The game resizes with flags 0x2; with the game's sRGB format Special K
puts the flag back, with HDR's format it does not, DXGI fails `ResizeBuffers` with `E_INVALIDARG`
and the game crashes releasing the missing back buffer (`0x19F43D0`, from its window procedure).
HDR's `create_swapchain` keeps the flag when the live swap chain has it.

**Frame generation** does not start when a module exports `SK_GetDLLRole`; the menu says so. On the
frame generation swap chain, Special K's present hook makes a Direct3D 11 texture with the game's
device and faults in `d3d12.dll` on the third frame, on the game's window thread, which ends; the
rest of the game waits for it. With Special K's `[API.Hook] d3d12=false`, DLSS Frame Generation
faults in `sl.dlss_g.dll` on the first frame instead.

## Frame rate

**What the game does.** `framerate_control` in `TPP_GRAPHICS_CONFIG` (`videoout_setting`) is
`Auto`, `Limited30` or `Variable`; the game's menu does not offer it. Traced statically, checked in
game:

- The config loader (`0x52BC30`) stores each video-out value as a 48-bit name hash: `window_mode`
  at video out + 0x10, `vsync` + 0x18, `framerate_control` + 0x20. `Auto` is `0x373A4C315329`
  (read in game). The apply code tests `0xC5A0FE4F5128` (`Limited30`) and `0x9E7769C788E4`
  (`Variable`).
- Applying the video-out settings (`0x24BB40`) sets the frame period at `[[0x2B641C8] + 0x60]`
  (1/59.94 s, 1/29.97 s with `Limited30`) and calls `SetGameFrameWaitType` (`0x81090`, through
  `0x80EE0`) with a wait type hash: `0xA5D6397A8E12` for `Auto` and `Limited30`, `0xA29EF1CEFC2`
  for `Variable`. Lua can call it too (`FoxGameFrame.SetGameFrameWaitType`).
- `SetGameFrameWaitType` keeps the hash at `0x29E3D68` and sets the wait callback of the main
  loop's clock `__game_main` (`0x1A46790`). Each main loop frame ends in that wait (`0x7FDB0`,
  `0x1A464D0`, `0x1A46D80`). The `Auto` wait (`0x807E0`) sleeps until the next period tick: the
  60 fps cap. The `Variable` wait (`0x803F0`) does not wait; it returns the measured frame time,
  averaged over up to 30 frames.
- Game time: the frame start (`0x7F840`) steps by the clock's measured frame time unless the frame
  controller's mode (`+0x64`) is 1 (fixed step). In game it is 0, so unlocked frames take shorter
  steps (0.0104 s at 96 fps) and game speed does not change. Lua's `SetFrameRateMode` (`0x17AA70`)
  can switch the mode; the settings do not.

**Unlock.** `src/frame_rate.cpp` verifies the build and the setter's first bytes
(`40 53 48 83 EC 20 48 B8 12 8E 7A 39 D6 A5 00 00`, which hold the `Auto` wait hash) and hooks it
with MinHook at `AddonInit`. While "Unlock frame rate" is on, a request for the `Auto` wait gets the
`Variable` wait; every other type passes through, so `Variable` in the config and Lua's choices are
kept. Switching the option calls the setter at once with the last requested type, so it applies
live. When a check fails nothing is hooked and the config decides. Checked in game: 60 fps off,
about 95 fps at once when turned on (the CPU limit on the test PC), 60 again when turned off; with
the option saved on, the game's first requests at start-up get the `Variable` wait.

## Diagnostics

RenderDoc cannot capture this game (see "Known limitations of the tools"), so the add-on has its
own recorder, `src/frame_log.cpp`. It is off unless a `PhantomFX-frame-log` folder exists beside the
add-on when the game starts; without the folder it registers no event and costs nothing. It only
reads state; the frame renders unchanged. Its files go into that folder:

- Every shader is logged once to `PhantomFX.log` (stage, checksum, size). With a `shaders` folder
  inside, the bytecode is saved there. The game's shaders keep reflection and debug info (cbuffer
  and variable names, `#line` source paths), so `D3DDisassemble` names every pass.
- F10 records a frame to `frame-<n>.txt`, after one warm-up frame, in GPU order: command lists
  recorded on deferred contexts appear, indented, where the immediate context executes them. Each
  call lists shaders, targets, depth, viewport, blend, depth state, shader resources, UAVs and
  constant buffers. `frame-<n>-writers.txt` lists the call stacks of constant buffer maps by size.
  Once both are written, a "Frame captured" toast with the file name shows at the right of the screen.
- `dump.txt` (`<checksum> <count>` per line) makes the first calls with those shaders add their
  constant buffer contents (as the game wrote them on that thread) and save their textures as DDS
  in `frame-<n>/` once their command list has run, so a list's textures are as it left them, not as
  each of its calls saw them. ` constants` after the count leaves the textures out. An indexed call
  also saves `<call>-vertices.txt`: each index's vertex (its first four floats) and the bounds in NDC
  of those in front of the eye; and `<call>-streams.txt`: every other bound vertex buffer slot from
  its offset, an element a line as floats (an instanced draw's per-instance data, such as the clone
  shaders' matrix rows).
- With Ray-traced lights on, a dev build's redraw of the lights saves what it draws from into the same
  folder, for each batch it draws: `lights-<n>-before-diffuse.dds` and `-specular` (the light targets),
  `-after-` the same once drawn, `-share.dds` (the rays' share, R16), `-depth.dds` (64 bits a pixel, the
  depth in the first 32) and `-lights.txt` (each light's shader, world position and way, colour, range,
  whether traced and its volume's index count).

The overlay's Diagnostics section has, for everyone: debug views of the motion vectors, of
where the engine's object velocity was used and of the global illumination denoiser's history (red
where it started a pixel's light over within three frames, yellow under ten), jitter off, the game's FXAA for comparison (DLSS stays
ready), and the frame, fallback and reset counts. All are off by default and not saved. Every 1800
frames with DLSS on, `PhantomFX.log` gets a status line with the same counts.

### Costs

Diagnostics ends with what the add-on's passes cost on the GPU (`src/performance.cpp`): one row
per effect with a bar, its milliseconds and its share of the GPU frame, each opening to its passes,
averaged over about half a second. The GPU frame runs from a timestamp at the frame's first
command list to one at its present. Frames are timed only while the costs are on screen (the
Diagnostics section open, or **Show the costs over the game**) or measured; otherwise no query is
made.

- **Timestamps.** Each pass ends a timestamp query before and after its work (`performance::Timed`),
  on the context it records on, inside one disjoint query per frame on the immediate context. The
  game records a frame on its deferred contexts while the immediate context executes the one
  before, so a deferred context's queries follow it to its command list at `FinishCommandList` and
  count for the frame that executes the list. DLSS's passes run on the immediate context and count
  for the frame at once. The results are
  read back up to four frames later with `DONOTFLUSH`, never waiting; a frame whose clock was
  disjoint is left out.
- **Ray tracing.** Its Direct3D 12 queue ends a timestamp between its phases (`Recorder::mark`,
  each with its pass), and `gi_renderer` hands each frame's times to
  `performance::timedOnDirect3D12` as it reads them back. Direct3D 11 waits while Direct3D 12
  works, so the G-buffer pack and the waits, less all the Direct3D 12 passes, are the hand-over.
  Each ray-traced effect counts its own passes; the scene's build and the hand-over split evenly
  between those working, else count under the first that is on.
- **What a row holds.** The sun light pass is the game's own lighting drawn with the replacement
  shaders, so it includes the game's work. The game's fog pass is timed because the add-on draws it
  over the larger texture. Frame generation runs on Direct3D 12 and is not timed; FSR's upscale
  shows as the Direct3D 11 time around it.
- **Real cost.** The timed passes miss what they change in the game's work: GTVBAO leaves the
  game's SSAO out, the physical look the game's sky. **Measure the real cost** times the GPU frame
  with everything on, then leaves out, one more at a time and only where on: the volumetric fog,
  SSILVB, GTVBAO, the lamp and the sun contact
  shadows, the soft shadows, the sharpening and last the upscaler (the game's FXAA at the same
  render size, as the Diagnostics comparison). 1.5 s to settle and 2.5 s timed per step; the
  difference between steps is what the effect left out costs. The steps run through
  `upscaler::leaveOut`, which the settings, `PhantomFX.ini` and the overlay never see, and the
  effects come back at the end. HDR takes a restart and is not measured. The GPU frame, not the
  frame time, is compared, as the 60 fps cap would hide the difference.
- **Report.** While the costs show over the game or are measured, `PhantomFX-performance.txt`
  beside the log holds them, rewritten every second and once more after, so it keeps a finished
  measurement. `PhantomFX.log` gets each measurement's result.

### Weather and time

The overlay's Utility section has **Weather and time** (`src/weather.cpp`), to see the sky in every condition:
a 24-hour time-of-day slider, a clock stop, and a weather override with fog density for Foggy.
It calls what the game's `WeatherManager` script functions call, on the game thread at the main
loop's frame start (the game loop is hooked with or without frame generation for this), and reads
the state back there for the overlay. Nothing is saved.

- **Manager.** A singleton at `[0x2BD3600]`, made on first use (`0x5348C0`). Its object at `+0x80`
  is the clock: seconds since midnight as a float at `+0xED0`, the stop byte (`PauseClock`) at
  `+0xEC8`, the time scale at `+0xED4`. `SetCurrentClock` (`0x5365C0`) calls `0x536550(manager,
  seconds)`, which also passes the time to `[[[[0x2C3A8A0] + 8] + 0x98] + 0x78] + 8`'s first virtual.
- **Requests.** The object at `+0x58` has one 16-byte slot per priority from `+0x2C`
  (`REQUEST_PRIORITY_NORMAL` 0, `EXTRA` 1, `PLAYER` 2, `FORCE` 3, `DEMO` 4): user (a name hash),
  transition time, weather type byte (`0xFF` none), and for fog its density plus 10000 x the fog
  type (`FOG_TYPE_PARASITE` 0, `NORMAL` 1, `EERIE` 2). The strongest filled slot wins.
  `RequestWeather` (`0x5361D0`) calls `0x551430(requests, priority, user, type, transition)`, then
  for fog `0x551B80` (density) and `0x551BB0` (fog type); `CancelRequestWeather` (`0x534680`)
  calls `0x54F8F0(requests, priority, user)`. A transition of 0 takes the game's default; under
  1e-5 the weather switches at once. Bytes at `+0xC1C` mark the weather types the location has
  settings for; requests for the others are ignored.
- **Types** (`TppDefine.WEATHER`): Sunny 0, Cloudy 1, Rainy 2, Sandstorm 3, Foggy 4.
- The menu forces with priority `FORCE` and its own user id, as `TppWeather.ForceRequestWeather`
  does with "Script". Releasing it sets the transition of the game's strongest remaining request
  (the weather system's: user hash `0x7F387469`, transition 20) to the instant one first; with its
  own transition the forced weather stayed in place. `StoreToSVars` copies every slot into the save, so a checkpoint keeps the
  override; the menu recognises its user id after a restart.

### Game Lua

`src/game_lua.cpp` runs Lua in the game's own script state, on the game thread at the main loop's
frame start, the way the game's own calls into it run. The dev build's Developer page has a **Lua
console** on it: Ctrl+Enter or Run runs the box's code at the next frame start, and the output box
shows what it printed and returned, or its error with the stack.

- **Lua.** Lua 5.1.5 is linked into the exe (its copyright string at `0x24B47A0`). The base
  library's table at `0x24B6A70` gives `loadstring` (`0x1A2E2E0`), which calls `luaL_loadbuffer`
  (`0x1A178E0`), and `pcall` (`0x1A2E6C0`), which calls `lua_pcall` (`0x1A116C0`);
  `luaL_checklstring` calls `lua_tolstring` (`0x1A12150`); `tostring` pushes `"nil"` with
  `lua_pushlstring` (`0x1A118C0`); `pcall` also calls `lua_pushboolean` (`0x1A11750`). Traced for
  5 s in free roam, every `lua_pcall` came from the main loop's thread on one `lua_State`.
- **Script system.** The game calls Lua inside a scope: `0x6A590(scope, restoreTop, runQueued)`
  takes the reader lock at `0x2B64200`, runs the system's queued work when asked (`0x6AD50`; the
  game's per-frame callers ask) and notes the stack's top; `0x6A960(scope)` restores the top when
  asked and releases the lock. The scope holds the system (the pointer at `0x2B641F0`), the top and
  the restore flag; the system's `lua_State` is at `+ 8`. A chunk run this way returned
  `vars.missionCode` and the player's position in game, with the stack balanced after.
- **Harness.** Each run loads a small Lua function and calls it with the code, a chunk name and
  whether to try the code as an expression first (`return ` before it, as an interactive Lua does).
  While the code runs, `print` collects lines in place of the game's; the code runs under `xpcall`
  with `debug.traceback` when the game has the debug library. The lines and the returned values come
  back as one string; an error is raised again with the lines before it.

### Travel

The dev build's Developer page has **Travel** (`src/travel.cpp`): Afghanistan, Africa or Mother
Base free roam from any free roam, after one loading screen, without the helicopter and the sortie
screens. It runs a chunk through Game Lua, and a toast says whether the game left or why not.

- **Leaving.** The chunk calls `TppMission.ReserveMissionClear` with `FROM_HELISPACE` and the
  destination's free roam (30010, 30020, 30050), the call free roam's own scripts make; the free
  roam sequence then ends the mission (`MissionGameEnd`, `MissionFinalize`) and
  `TppMission.ExecuteMissionFinalize` saves and loads the next one. It only runs in free roam
  (elsewhere it would end a story mission), when `TppMission.CheckMissionState()` allows it, and
  not into Mother Base while one of its cutscenes waits (`gvars.mbFreeDemoPlayNextIndex`), which
  can stop loading with the player on foot.
- **Arrival.** Free roam to free roam, the game's `LoadingPositionToFree` puts the player where they
  stood, which in another map is mid-air or underground. The chunk wraps
  `TppMain.ReservePlayerLoadingPosition` once: after the game's own, when `vars.missionCode` is the
  destination, it sets the start on foot, clears the helicopter route, sets the initial and mission
  start positions and points `StageBlockCurrentPositionSetter` there, so the map streams in around
  it. The spots: in Afghanistan (1448.61, 337.787, 1466.4) and Africa (-510.73, 5.09, 1183.02),
  where `TppPlayer.FailSafeInitialPositionForFreePlay` puts a player found off the map; at Mother
  Base (9.12, 8.75, -42.43), the Command platform's first helipad (landing point
  `cl00pl0_mb_fndt_plnt_heli_30050` in `mbdvc_map_mbstage_parameter.lua`, the same in every
  layout).

## How Fox Engine renders

Recorded in Afghanistan free roam at 1920x1080 windowed, all settings Extra High, depth of field
on, motion blur Off and Low. Resource names are the pointers of one session; formats and sizes
are stable. Pass names are the shaders' source files.

1. Clears; volumetric fog volume (`VolFog_TppVolFog`, 512x256 `R16G16B16A16_FLOAT`).
2. G-buffer, reversed-Z: terrain depth prepass (`NewTerrain_Depth`), objects (`fox3ddf_*`, skinned
   `fox3ddf_skin*`), terrain (`NewTerrain_PackedMaterial*`), decals. Three `B8G8R8A8_UNORM`
   targets and depth `D32_FLOAT_S8X24_UINT`, cleared to 0, tested `greater_equal`.
3. With motion blur on only (the add-on forces it on, see "Velocity without motion blur"):
   velocity. Dynamic meshes, including Snake, soldiers and D-Horse, are drawn again with
   `GBufferVelocity` / `GBufferMaskedVelocity` (stencil ref 1) into a velocity target, then
   `MotionBlurCameraVelocity` adds camera motion from depth into a second target.
4. SSAO at half size (960x540) with a separable compute blur (see "Ambient occlusion"), screen-space and SH lighting,
   shadow maps (one 8192x8192 `D24_UNORM_S8_UINT` atlas), sun light into two
   `R16G16B16A16_FLOAT` 1920x1080 targets (see "Sun light and shadows"), then a separable compute
   filter over the lighting that
   reads depth and normals (shaders embedded in the exe, `04ef76054aae7a8f2d30c0386c915839` and
   `960e322bda3a4d1ca1a48bc3569c2840`; they have their own `cCameraCenterOffset` constant).
5. **Tonemap early:** `DR_VolFog_TppTonemap` writes the scene target `B8G8R8A8` 1920x1080 through
   an `_SRGB` view. Everything after is LDR, stored sRGB-encoded. `CopyDepthBuffer` makes the depth
   copy the later passes test against.
6. Sky, forward and transparent objects (`*3dfw*`), screen-space reflections (half size, blurred
   mip chain, `LocalReflectionAddBuffer`), particles (`Prim_Poly_*`).
7. Exposure (`Tonemap_Lum_Lacc`), bloom and lens flare at 480x270 (`TonemapBrightpass`,
   `SSLensFlarePass1/2`, `blur_kawase`), depth of field (`DOF_Scatter*`, composited into the scene
   target every frame with depth of field on; see "Depth of field").
8. A 960x540 layer blended over the scene (`CopyRenderBuffer`, blend ONE / SRC_ALPHA).
9. With motion blur on: `MotionBlurTile`, `MotionBlurTileNeighbour`, `MotionBlurBakeVelocityDepth`,
   two `MotionBlurMcGuire` passes at 960x540, blended over the scene (`CopyRenderBuffer`, SRC_ALPHA
   / INV_SRC_ALPHA).
10. Colour grading: `ColorCorrection_BlndLut_Tex_Gen` builds a 256x16 LUT; the scene target is
    copied to its twin; `Tonemap` applies the LUT from the copy into the scene target.
11. **FXAA:** the scene target is copied to its twin again; `Fxaa` reads the copy and writes the
    scene target.
12. **UI:** `Draw2D*` (HUD, prompts, menus) draws straight into the scene target, alpha-blended.
13. `GammaControl` reads the scene target through an `_SRGB` view and writes the back buffer
    (`R8G8B8A8_UNORM_SRGB`). Present.

Film grain, chromatic aberration and the camera filters exist (`Draw2D_TppNoiseFilter`,
`Draw2D_TppDirDisp_CA`, `Draw2D_TppCameraWaterDrops`, ...) but did not run in the recorded frames;
they are situational. `TemporalAA_CopyVelocity` exists and never ran.

### Resources

| Resource | Format | Size | Role |
| --- | --- | --- | --- |
| Back buffer | `R8G8B8A8_UNORM_SRGB`, 3 buffers, DISCARD | window | Only `GammaControl` writes it |
| Scene target | `B8G8R8A8_TYPELESS` (UNORM and UNORM_SRGB views) | render | LDR scene from step 5 on; FXAA output; UI |
| Scene copy | `B8G8R8A8_TYPELESS` | render | `CopyResource` of the scene target; FXAA and LUT input |
| G-buffer | 3 x `B8G8R8A8_TYPELESS` | render | Albedo, normal, specular/material (content per Courrèges, not checked) |
| Depth | `R32G8X24_TYPELESS`: DSV `D32_FLOAT_S8X24_UINT`, SRV `R32_FLOAT_X8X24_TYPELESS` | render | Reversed-Z, the G-buffer's |
| Depth copy | same | render | Made by `CopyDepthBuffer`; used from step 5 |
| Dynamic velocity | `B8G8R8A8_TYPELESS` | render | R = 255 where a dynamic mesh drew, G = 0, B/A = velocity |
| Velocity | `B8G8R8A8_TYPELESS` | render | R = G = 0, B/A = camera and object velocity |
| Lighting | 2 x `R16G16B16A16_FLOAT` | render | HDR light accumulation |

### Shaders

Checksums are the DXBC header checksum (bytes 4 to 20) as lowercase hex.

| Pass | Vertex shader | Pixel shader |
| --- | --- | --- |
| `Fxaa` (FXAA 3.11, `fox\source\system\Gr\Dg\shader\Fxaa.hlsl`) | `dd1e3d1240e73daa7e53e861845472f6` | `deef1a5e3462ab091b5612a37662d15d` |
| `Tonemap` (applies the LUT) | `3deca0fd1a4b712a99b1419546693ce1` | `422ba739df267d9f92d4d62b0d2b4e09` |
| `ColorCorrection_BlndLut_Tex_Gen` | `d6d688ebb016cfb9133b359f233af9f9` | `82a954d0fe3755bb0a79aaaded5cce74` |
| `GammaControl` | `a3a7925d91d305283bb6e2ccc51504e8` | `12790f061c5f1460aa40c264e4bc9f07` |
| `DR_VolFog_TppTonemap` | `11a3b2f867ec3409c20a2f58c0bb11dd` | `00a609370c2e8b47a645263f91f6d3e8` |
| `GBufferVelocity` | `653764512e1d19ffe60c3db8c1c2f2d2` | `89e3198a5285240f75a44e714406b36c` |
| `GBufferMaskedVelocity` | `b93ed456c7b3d164c14ff09f15d5144d` | `66b808b723708f6cf72b98f690f0aa44` |
| `MotionBlurCameraVelocity` | `ea21bed4add8b4f64839b58145cf3cb4` | `5d0241dd9804f41c780cd36669c71198` |
| `MotionBlurTile` | `c0d86c3bea9821b283fe78493614fa05` | `519f12495c42e4b6a27d56a05514b0c5` |
| `MotionBlurTileNeighbour` | `0d214bed7b73fea12f2cf5e22039a103` | `890e554ba4f340d1d82ddf218d3e339e` |
| `MotionBlurBakeVelocityDepth` | `0f934876edd8b1120384905cd7e3477f` | `d89d8bcef5a286dee3f697d3aae068fa` |
| `MotionBlurMcGuire` | `926f38e58184c050456e4bdb9bfe184a` | `475283f0d10c84d3c1b47e2ba153c916` |
| `CopyRenderBuffer` (generic copy, used many times) | `da1404ad7d2b4c9d6dc7b57828737141` | `9f93a957f1e21656e0d86bf112b71d6d` |
| `CopyDepthBuffer` | `33c68dfbe259d05896722b16de33562d` | `b61f24562a2421162537203cc932a8e4` |
| `Draw2D` | `49fdf6e5a7ec2e0b1461ef43eaed943e` | `ceb72d101a7b403a4e9ad06d5e55f02d` |
| `Draw2D_Ui2_BL` | `7e254e83e180ad51690ed8cb3b9206db` | `670bf1eb7e25589eeec593f59ba2ce69` |
| `NewTerrain_Depth` (first G-buffer pass) | `19db120cc51b6a18587f315dc7135436` | `45bc53d23272dbfcd9108a611174c65a` |
| `fox3ddf_blin` (common object) | `2d242c4792e9edde4e8b12ca2d3810f5` | `3942f4da227e6ac7c19daf284d4d7024` |
| `fox3ddf_skin` (skinned) | `feec72cb73d30137114267d174e4ae14` | `0ab79b77cd13e962000f47e9afe333cc` |
| `ModelForward_ShadowOpaque` | `f241bee6390b176e669aabd0eceada09` | `4b5d963e9de0def40f7aaff71e62ba9a` |
| `SunLight_DL_CS_ES` (sun inside the shadow range) | `c6a508fd91f99886f583eda88a8855c7` | `56ed4db892a7940f038146eeb0df0d30` |
| `SunLight_DL_ES` (sun beyond it) | `d5b3bd0acc405d7a4d115aa9f38a45cb` | `92726e8d231c8ed85f0db90400ef2258` |

### FXAA pass

- Pixel shader: `t0 g_Image` (the scene copy, `B8G8R8A8_UNORM` view, so values are sRGB-encoded),
  sampler `s11 g_samplerLinear_Clamp`, `cb0 cPSSystem`; `fxaaQualityRcpFrame` is
  `cb0[2].zw` = `m_renderBuffer.zw`.
- Writes the scene target (`B8G8R8A8_UNORM` view) with the depth copy bound and depth off.
- The copy's alpha varies (0 to 253 in a dump), consistent with luma in alpha for FXAA.
- It runs in a deferred command list together with the LUT pass, before the UI's list.

### Constant buffers

`cPSSystem`, slot `b0` (64 bytes):

| Register | Field | Value seen |
| --- | --- | --- |
| c0 | `m_param` | 0 |
| c1 | `m_renderInfo` | (1920, 1080, 0, 0) |
| c2 | `m_renderBuffer` | (1920, 1080, 1/1920, 1/1080) |
| c3 | `m_dominantLightDir` | sun direction, w = 1 |

`cVSScene` / `cPSScene`, slot `b2` (480 bytes, dynamic, written with Map `WRITE_DISCARD`). Matrices
are rows, applied as `dp4(row, v)`:

| Registers | Field | Main view, G-buffer and velocity passes |
| --- | --- | --- |
| c0-c3 | `m_projectionView` | P x V |
| c4-c7 | `m_projection` | `(-1.75,0,0,0) (0,3.111,0,0) (0,0,A,B) (0,0,1,0)`: A = -1.725e-5, B = 0.069 |
| c8-c11 | `m_view` | world to view |
| c12-c15 | `m_shadowProjection` | shadow |
| c16-c19 | `m_shadowProjection2` | **previous frame's P x V** |
| c20 | `m_eyepos` | camera position, w = 1 |
| c21 | `m_projectionParam` | (1/P00, 1/P11, B, A) |
| c22 | `m_viewportSize` | (1920, 1080, near 0.069, far 4000) |
| c23 | `m_exposure` | |
| c24-c26 | `m_fogParam[3]` | |
| c27 | `m_fogColor` | |
| c28 | `m_cameraCenterOffset` | (0, 0, 0, 0) |
| c29 | `m_shadowMapResolutions` | (8192, 4096, 8192, 4096) |

- The projection is reversed-Z with a finite far plane: depth = A + B / z, 1 at z = 0.069, 0 at
  z = 4000. Clip w is view z. P00 is negative (the view's x axis is mirrored). P11 = 28/9 here,
  vertical FOV about 35.6 degrees; it changes with the camera.
- The game fills several instances per frame from different buffers: shadow views (`c4-c7`
  orthographic, `c7 = (0,0,0,1)`), the main view, and the post-processing passes. They are told
  apart by content, not by buffer.
- Slots are reused per pass. In `MotionBlurCameraVelocity`, `c8-c11` hold the previous P x V and
  `c16-c19` the inverse view. In post passes `c16-c19` hold fog colours. In shadow passes
  `c16-c19` hold the main view's P x V.
- With motion blur Off, the main view's `c16-c19` equalled `c0-c3` in the one frame recorded. The
  velocity passes the add-on forces on (motion blur Off) do get the previous P x V: with the camera
  following Snake on a sprint, his body's velocity is near zero while the ground's is over 10 px.

`cVSObject` / `cPSObject`, slot `b5` (208 bytes): `m_viewWorld` c0-c3, `m_world` c4-c7,
`m_useWeightCount` c8, `m_localParam[4]` c9-c12. `VSBones`, slot `b6` (3072 bytes):
`g_vsBone` (32 x `float4x3`) and **`g_vsPrevBone`** (32 x `float4x3`).

### Velocity

- Encoding, the same in `GBufferVelocity` and `MotionBlurCameraVelocity`: v = (current NDC -
  previous NDC) x `m_renderInfo.xy` x 0.5 / 64, i.e. pixels / 64; length clamped to 1; y negated;
  stored as v x 0.5 + 0.5 in B (x) and A (y), 8-bit. So: 0.5 pixel steps, at most 64 pixels,
  screen-space x right, y down, pointing from the previous position to the current one.
- `MotionBlurCameraVelocity` reconstructs view position from depth (`t2`) with
  `m_projectionParam` and `clip.xy - m_cameraCenterOffset.xy`, goes to world with `c16-c19` and to
  the previous clip position with `c8-c11`, and keeps the dynamic velocity (`t3`) where its mask is
  set.
- The game runs the velocity passes only with motion blur Low or High (config values `Small` and
  `Large`). They cover skinned characters with per-bone previous positions: dumps show Snake's legs
  and arms with their own motion and the standing horse with camera motion only. Nearby trees are
  drawn too (wind); grass and bushes are not.
- With the add-on's jitter, the current position is projected with the jittered matrices and the
  previous one with the engine's unjittered previous P x V (`c16-c19`, which the add-on leaves
  alone), so the stored velocity includes this frame's jitter.
- The vertex shader projects the current position with `cVSObject.m_viewWorld` and `c4-c7`, and the
  previous one from `g_vsPrevBone` with `c12-c15` (not checked) and then `c16-c19` (previous P x V). The scale
  (pixels / 64) is a literal in the pixel shader, so it does not depend on the blur amount.

### Velocity without motion blur

What decides whether the velocity passes run, traced statically in this build:

- `GrPluginMotionBlur` (class info `0x2B75C20`, 0x80 bytes) has its vtable at `0x20F8830`. Slot 6
  (`0x1E2D80`) runs before the frame's geometry and slot 8 (`0x1E2E20`) draws the blur; they are
  called PreExec and Exec here, after `fox::gr::PluginDeferredGeometry::PostExec`, the one name
  the string table gives. Both take (plugin, pass context, view).
- PreExec: the shutter is the camera's (`[[view+0x570]+0x128]`, or the plugin's own at `+0x6C`
  when `+0x68` is set), at least `[0x2B75AC8]`. If the dword `[0x2B75AC4]` (motion blur on) is
  set and shutter x `[0x2B75ACC]` exceeds the constant at `0x20F732C`, it sets bit 0 of the
  64-bit frame flags at `[[context+0xD0]]` (`or qword [rax], 1` at `0x1E2DDE`).
- `PluginDeferredGeometry::PostExec` (`0x1C54D0`, GPU marker `MakeVelocityBuffer`) tests that bit
  (`test byte [rcx], 1` at `0x1C558F`) and only then draws the two velocity lists (`[plugin+0x70]`
  and `[plugin+0x78]`, created unconditionally at `0x1C5050`) and `MotionBlurCameraVelocity`.
- Exec repeats the test and draws the blur through three renderer calls (vtable `+0x468`, `+0x470`,
  `+0x478`): tiles, neighbours, bake, the McGuire passes and the composite. It does nothing else.
- The flag is cleared every frame: when the add-on stops setting it, the next frame has no
  velocity passes. The three globals have no RIP-relative writers; they are written through
  pointers, presumably from the graphics settings. The add-on does not touch them.

Leads not needed in the end: the graphics settings object is the singleton at `[0x2B74910]`
(0x1E0 bytes; the current settings at `+0x38`, the next at `+0x108`, 0xD0 bytes each: video out
0x38, then quality, each value a 48-bit string hash, `motion_blur_amount` first).
`0x1D38F0` stores new settings and `0x1BCD50` notifies the listeners in the array at `0x2B735E0`
(count at `0x2B735D8`, vtable slot 2). The `GrPluginSettings` entity (reflection at `0x1C55CA0`)
has `isMotionBlur`, `isPatchVelocity` and `isTemporalAA` bits in its `flags`, set from level data.
Some functions jump into the `.xdata` section (`0x2F03000`, 111 MB, entropy 6.7), which holds
virtualised code (for example `0x1D3850`); static analysis stops there.

### FXAA with post processing down

Lower post processing settings turn the FXAA pass off, and with it DLSS, which replaces that pass.
Traced statically in this build:

- `GrPluginFxaa` (class info `0x2B799A0`, name string `0x21032D0`) has its vtable at `0x2103220`.
  Slot 8 (`0x2155A0`, Exec) takes (plugin, pass context, view), like the motion blur plugin's.
- Exec runs the FXAA filter at `[plugin+0x68]` (its vtable `+0x20`, with the view in `rdx` and
  `r8`) only when bit 7 of `[[view+0x570]+0x100]` is set (the view has post effects) and the dword
  `[0x2B79984]` is non-zero. Exec is that dword's only reader; the post processing setting clears
  it. Exec does nothing else.


All screen-space reconstruction shaders read `m_cameraCenterOffset` (`cb2[28]`) and subtract it
from the clip position: lighting (`Lighting_*`, `SSLighting2_*`, `SunLight_*`), SSAO, screen-space
decals, subsurface scattering, the rain filter and `MotionBlurCameraVelocity` (42 pixel shaders,
plus the embedded compute shaders through `cCameraCenterOffset`). The engine therefore supports an
off-centre projection and passes the offset to the reconstruction. Units and sign, from
`MotionBlurCameraVelocity`: `clipSpacePosXY = inBasePos.zw - cb2[28].xy`, then
`viewPos.xy = clipSpacePosXY * (1/P00, 1/P11) * viewZ`, so the offset is the NDC shift the
projection adds, and the shader removes it. In the game's own frames it is 0.

The two embedded compute shaders (`04ef7605...`, `960e322b...`) turned out to be the skin
subsurface-scattering filter: they only process pixels whose light buffer alpha is set (skin) and
read the offset from their own constant buffer, which the add-on does not set.

How the scene constants reach the GPU, from the call stacks of their Map calls:

| RVA | What |
| --- | --- |
| `0x2B9E30` | Per-pass "apply": updates the bound constant buffers (`0x2486E0` for the scene buffer) and binds |
| `0x2486E0` | Uploads the scene buffer object (`[ctx+0x130]`) through `0x2C5A10` |
| `0x2C5A10` | Generic upload: for each slot `Map(WRITE_DISCARD)`, `memcpy` from the CPU copy at `[slot+8]` (`[slot+0x18]` x 16 bytes), `Unmap`, bind |
| `0x2D664F`, `0x2B9E60`, `0x29190D`, `0x291EB0` | Most frequent callers of the upload for 480-byte buffers (157 of the 234 maps in the warm-up and recorded frames came from `0x2D664F`) |

Every scene-constant upload is a Map / memcpy / Unmap, and ReShade's `unmap_buffer_region` event
fires before the real `Unmap` with the data still mapped, on deferred contexts too. The code that
builds the projection itself was not found. Many functions store to offsets `0x150`, `0x160` and
`0x1C0` together (the candidates `0x1EB190`, `0x206630`, `0x20AA00`, `0x272390` and a dozen more);
the ones read fill other structures, so the scene block has no single writer to hook.

### Render size

The renderer keeps the size it renders at apart from the display's. Traced statically, then
confirmed by setting the wanted size in game.

- The renderer object (vtable `0x21086E0`) holds the display size at `+0x180`, the wanted render
  size at `+0x188`, the current one at `+0x190` and flags at `+0x198`.
- A display mode request (`0x2496A0`, slot 10) stores the display size and sets the wanted size to
  it, at least 512 and rounded up to even, with flag 2 when it differs from the current size.
  Applying the video-out settings (`0x24BB40`) sets flag 4 instead.
- The per-frame step (`0x24C220`, slot 6, on the render thread): with flag 4 it first takes the
  wanted size from the display object (`[+0xF0] + 0x30`); with flag 2 and a wanted size that differs
  from the current one it copies it and calls `0x24B2A0`. That rebuilds every size-dependent target:
  five groups through `0x242AC0` (group, width, height), three objects through their virtual
  `+0x10`, `0x27B4E0`, a generation counter at `0x2B9404C`, and `0x1BE4C0`, which lets the plugins
  resize theirs. Call stacks recorded during development show the same path rebuilding the
  targets when the game's window goes from its first 1024x576 to the configured size.
- The swap chain is not part of this: the display object resizes it on its own (`0x24AD80`,
  `ResizeBuffers` through `0x19F59A0`), and the window keeps the display size.
- With the wanted size set to 960x540 for a 1920x1080 window, the whole frame follows: G-buffer,
  lighting, SSAO and the depth-of-field and bloom chains at half size and below, the half-size
  layer (480x270). An F10 record had no 1920x1080 target apart from the back buffer.
  `GammaControl` stretches the scene target over the back buffer: it samples at
  `uv x m_renderInfo.xy x m_renderBuffer.zw` with a linear clamp sampler, the console-style
  sub-rectangle mapping, so any scene size fills the screen. FXAA's `fxaaQualityRcpFrame` and the
  colour grading follow by themselves.
- Except in exclusive fullscreen (`window_mode` `FullScreen`): there `GammaControl` draws with a
  viewport of the render size, centred in the back buffer (2228x1254 at `806,453` for 3840x2160 in
  F10 record frame-13946), so a smaller scene shows 1:1 in a box. No other screen-size draw in that
  record has an offset viewport; the only others draw tiles of two small targets (128x512 and
  128x256). Windowed and borderless draw it over the whole back buffer.
- `cPSSystem`'s `m_renderInfo` and `m_renderBuffer` are set per pass to that pass's target size
  (data breakpoints caught 512 for the fog volume pass): `0x2C586E` and `0x2C5F75` copy them into a
  per-context copy of the buffer with `memcpy`.
- `ChangeMainViewportSize` (`0x1C658E0`) stays an empty stub; nothing else was needed.

### The 2D pass

- The HUD, menus, subtitles and camera filters are drawn with the `Draw2D*` shaders. Their vertex
  shaders (67 in a full shader dump, up to 73 created in a session) are generated as
  `...\Draw2D*_vs.hlsl`, and the game's shaders keep that path in their debug data; many other
  shaders also contain the word `Draw2D` there, so only the path identifies them. In
  the recorded frames they draw into the scene target in the command list after FXAA's, with the
  scene's depth-stencil bound: the stencil is cleared to 1, a mask written with reference 0 and
  later draws tested against 1; depth testing is off. The viewport is the scene size and positions
  arrive in clip space through `cVSWork`'s matrix, so the same draws work at any target size.
- Their pixel shaders read nothing from `cPSSystem` but `m_renderInfo` (`c1`) and `m_renderBuffer`
  (`c2`): the UI shaders for a screen-space mask (`blendScreen`), the filters (radial blur, blood
  and water drops, the video player) for screen coordinates.
- 16 camera filters also read `cPSScene`'s `m_viewportSize.xy` (`c22`, slot b2), the render size.
  `Draw2D_TppNoiseFilter`, the sandstorm's grain and vignette, places the vignette with
  `2 x pixel / m_viewportSize.xy - 1`; at another target size its centre moves off the screen's.
- The only `Draw2D` draws before FXAA in the records are lens flares into the half-size layer.
- `GrPlugin2D`, `GrPlugin2DFrontmost` (vtables `0x20F2EB0`, `0x20F2F50`, Exec `0x1C0AD0`) and
  `GrPlugin2DShrink` (`0x20F2FF0`, Exec `0x1C0C70`) only queue the 2D work: a flag set around their
  Exec saw none of the draws, which the worker threads record later.

### Sun light and shadows

How the game shadows the sun, from the frame records and the shaders' disassembly. There is no
screen-space shadow mask: the sun pass samples the shadow map itself, so the shadow value exists
only inside its pixel shader.

- **Shadow map.** One 8192x8192 `D24_UNORM_S8_UINT` atlas, two by two cascades of 4096x4096,
  drawn right before the sun: `ModelForward_ShadowOpaque` (skinned), `ModelForward_ShadowOpaqueRigid`,
  `ModelForward_Shadow` and `NewTerrain_Shadow`. Characters are in it like everything else.
- **Light targets.** Two `R16G16B16A16_FLOAT` targets at the render size, diffuse and specular,
  cleared, then filled by `BilateralUpscale2x2` (blend off, the half-size ambient), the point and
  spot lights (`SSLighting2_*`, additive) and the sun. After the sun, the skin subsurface filter
  (the two embedded compute shaders) reads them; `DR_VolFog_TppTonemap` tonemaps.
- **Sun pass**, full-screen triangles, additive (ONE / ONE), stencil reference 10:
  `TPPSunLightReflectMap` and `SH_FresnelMap` fill a 128x256 sky reflection texture;
  `SSLighting2_Stencil` draws a box (36 indices) into the stencil to mark the shadow range;
  `SunLight_DL_CS_ES` lights inside it with the shadow map; `SSLighting2_Stencil` flips the stencil
  with a full-screen triangle; `SunLight_DL_ES` lights everything beyond, with no shadow at all.
  Both read the G-buffer (`t0` normal, `t1` specular, `t2` depth), the shadow atlas (`t3`,
  comparison sampler `s15`), the cloud shadow (`t4`), lookup tables (`t8`, `t9`), the material
  table (`t10`), a random texture (`t12`) and the sky reflection (`t15`); `cPSScene` (b2),
  `cPSLight` (b3) and, for the clouds, `cPSObject` (b5). In rain the pass draws `SunLight_DL_CS`
  (`7e5c2fc99c9d84ddd32df4e789e7082a`) and `SunLight_DL` (`853f56a63e5bc66e6ee301c85ebfeaa8`)
  instead: the same without the sky reflection (`t15`), as `DR_TonemapRainFilter*` reflects the sky
  there. `SunLight_ES` exists but did not run in the recorded frames.
- **Shadow** (`SunLight_DL_CS_ES`): the view position through `c12`-`c14` gives cascade 0;
  `c16`-`c19` (`m_shadowProjection2` in this pass) place cascades 1 to 3 (offset in rows 0 to 2,
  scale in row 3, depth scale in `c16.w`). The first cascade that holds the position wins; with
  `c19.w` set, the borders are dithered by the random texture. Four comparison taps half a texel
  apart, with a 2x2 pixel dither, average to `s`, and the shadow is `(1 - s)^2`. It then fades to lit
  towards the end of the range (`cPSLight` `c6.x`, `c8.xy`), is offset by `c6.w`, and multiplied by
  the cloud shadow (`t4` through `cPSObject` `c9`, `c10`, `c12.zw`) and a height fade (`c11`).
- **Lighting**: a disc-shaped sun (the light direction moves towards the reflection by its angular
  size, `cPSLight` `c1.w`), specular from the lookup tables, translucency for foliage, and a sky
  reflection. The shadow multiplies the diffuse light; the specular by `max(shadow, c7.x)`; the sky
  reflection by `max(shadow, c2.x) x c2.y`. The far pass is the same with a shadow of 1.

| `cPSLight` | Sun light |
| --- | --- |
| c0 | Direction towards the sun, view space |
| c1 | Colour (times `m_exposure.z`); w the sun's angular size |
| c2 | Sky reflection: shadow floor, scale, minimum F0 |
| c3 | Sky reflection colour |
| c6 | x shadow range fade, w shadow offset |
| c7 | x specular shadow floor |
| c8 | Shadow range: x centre depth, y start distance |

The normal decode adds `1e-7` inside its `rsqrt`, which the disassembly prints as `0.000000`;
`fxc /dumpbin /Lx` shows every literal in hex. It keeps a normal that faces the camera exactly from
dividing by zero.

### Sun movement

The game moves its sun and moon light in steps, so shadows tick forward about once a second. Found
2026-10-25 at Mother Base from the outside (`ReadProcessMemory`, Frida hardware watchpoints; scripts in
`%TEMP%\mgsv-sunstep`). `src/sun_movement.cpp` patches the step out at start-up.

- **The light's update** (`0x79D330`, every frame) takes the sky's sun or moon direction (`+0x60`,
  `+0x70` of the light's settings; `+0xC8` 1 for the sun, 2 for the moon) and clamps it to its limit
  angle (`+0x88` sun, `+0x8C` moon). With the stepped flag (`+0xCC` bit 3) it keeps the light at the
  last step (`+0xF0`, the one before at `+0xE0`) until the sky's direction is `(90 - limit) / steps`
  degrees away (steps at `+0xD0`), then blends from the old step to the new over `+0xBC` seconds with
  a Hermite ease, marking the light as moving only while it blends. Without the flag it takes the sky's
  direction every frame.
- **Where the values come from.** `TppAtmosphere` has properties for this
  (`divisonNumOfDirLightSteppedMove`, `interpolateTimeInSecondOfDirLightSteppedMove` and the
  `localFlags` bit 11), but every frame `0x6B96E0` overwrites them from the weather's parameters
  (`0x550410`): `0x23A` above 0 turns steps on, `0x238` is the blend time, `0x239` the step count.
  Seen at Mother Base: 1000 steps, limit 29 degrees, blend 0: a jump of 0.061 degrees every 1.2 s
  (the clock runs 20 times real time).
- **Patch.** The flag's test at `0x79D51D` ends in `je` to the unstepped path; the add-on turns it
  into `nop; jmp` to the same place. The light then counts as moving every frame, as it already did
  between steps in the weather seen (`+0x54` bit `0x800` of the light was clear there too).

### Ambient occlusion

How the game's SSAO works, from frame records at 3840x2160 with SSAO Off, Low and Extra High, and
the shaders' disassembly.

- **Methods.** `ssao` in `TPP_GRAPHICS_CONFIG`. The engine has `GrPluginLineIntegralSSAO`,
  `GrPluginAmbientObscuranceSSAO` and `GrPluginScreenSpaceAmbientOcclusion`
  (`AmbientOcclusionSettings` methods `LINE_INTEGRALS` and `HORIZON_BASED`). Low runs the line
  integrals only; Extra High adds the ambient obscurance. Medium and High were not recorded.
- **Line integrals** (Low and up): `SSAOLineIntegrals` at half size into a `B8G8R8A8` target, from
  a half-size depth, the depth and a 4x4 `R16G16_FLOAT` rotation texture;
  `blur_Gaussian1D_Bilateral_X` and `_Y` at half size; `SSAOAlphaToRGB` up to the render size.
- **Ambient obscurance** (Extra High): `CopyDepthBuffer` into a `D24_UNORM_S8_UINT` copy;
  `AmbientObscuranceSSAO` at half size into `R16G16_FLOAT` (occlusion, depth) from that copy and the
  G-buffer normals; a separable compute blur; a compute pass that combines it with the line
  integrals into an `R8_UNORM` texture at the render size. Its `cPSObject` (b5) `c9`-`c11`: normal
  scale 1, radius 0.2 m, bias 0.0144, intensity 1, contrast 2.5, 11 taps, faded out from 50 to
  500 m.
- **Where it applies.** Only the deferred resolve reads the result, as `inOcclusion` (`t13`), and
  multiplies all light by it, the sun included: `occlusion x (diffuse x albedo + specular)`. All ten
  resolve variants bind the same slots (`inAlbedo` `t8`, `inLightDiffuse` `t9`, `inLightSpecular`
  `t10`, `inDepth` `t11`, `inOcclusion` `t13`). With SSAO Off no AO pass runs and `t13` is a 1x1
  white texture.
- **Indirect light.** The probe and sky lighting is lit at half size (`SH_SphereMap`,
  `SSLighting2_SH_MultiBlend` in stencil volumes, the baked occlusion volumes of
  `SSLighting2_SH_Occlusion`, `SSLighting2_SH_SkyLight_Encode`); `BilateralUpscale2x2` then writes
  it into the diffuse light target with blend off. Its pixel shader writes `SV_Target0` only, so the
  specular target, also bound, stays clear. The sun and the local lights add to the diffuse light
  afterwards; the sky reflection is part of the sun pass (see "Sun light and shadows"). In every
  record the sky light pass, which binds the G-buffer normals as `g_NormalTexture` (`t0`), directly
  precedes the upscale in the same command list; the upscale binds the depth as `inDepthBuffer`
  (`t2`) and `cPSScene` (b2).
- **`SH_SphereMap`'s light** (`b7`, `PsWork`): `g_psParamSH`, four 4x4 matrices (`c0` to `c15`: the
  probe's light, a matrix a colour, then the sky's occlusion), and `g_psParamSHSky`, three (`c16` to
  `c27`: the sky's light); the ambient it draws is the probe's light plus the sky's times the occlusion
  (clamped to 0..1). Read at Mother Base 2026-10-23: the sky and the occlusion all zero, the probe's
  light near 800 at `c3.w`: its ambient is all baked into the probes. The global illumination takes
  the sky's light for its rays that reach the sky, and the probe's light where the game gives no sky
  (Mother Base was black-red with the sun's bounce alone).
- **G-buffer normals** (`DecodeViewSpaceNormal`): `z = 2 b^2 - 1`, and `xy` the direction of
  `2 rg - 1` scaled to `sqrt(1 - z^2)`. They are in the view space the passes reconstruct positions
  in, with x mirrored.

| Pass | Pixel shader |
| --- | --- |
| `SSAOLineIntegrals` | `1e716301e9c644c4d59c620e9b84f7de` |
| `AmbientObscuranceSSAO` | `9f963f50539aafc2ee77b888c56d3a0f` |
| `SSLighting2_SH_SkyLight_Encode` | `ac49de83da6ebb80336de04c4ef7d022` |
| `BilateralUpscale2x2` | `1314ac1eb31bb06b4e972fdc2c24ccbf` |
| `DR_VolFog_TppTonemap`, the resolve recorded; nine variants in `src/indirect_light.cpp` | `00a609370c2e8b47a645263f91f6d3e8` |

### Engine addresses

| RVA | What |
| --- | --- |
| `0x2B9BA0` | Start-up device check (returns `al`) |
| `0x2BA60B` | Its call in the renderer start-up `0x2BA1B0`; fail path `0x2BAA37` |
| `0x70810` | Quit helper (`PostQuitMessage` at `0x7082A`) |
| `0x70430` | Window procedure (`WM_DESTROY` posts quit at `0x7051D`) |
| `0x19F4F29` | `D3D11CreateDevice` call |
| `0x2C5A10` | Constant buffer upload (Map at `0x2C5B02`) |
| `0x1E2D80`, `0x1E2E20` | `GrPluginMotionBlur` PreExec and Exec (vtable `0x20F8830`, slots 6 and 8), hooked |
| `0x1C54D0` | `PluginDeferredGeometry::PostExec`, `MakeVelocityBuffer`; velocity test at `0x1C558F` |
| `0x2B75AC4`, `0x2B75AC8`, `0x2B75ACC` | Motion blur on, minimum shutter, amount (read by PreExec and Exec) |
| `0x2155A0` | `GrPluginFxaa` Exec (vtable `0x2103220`, slot 8), hooked |
| `0x2B79984` | FXAA on, cleared by lower post processing settings (read only by the FXAA Exec) |
| `0x1C658E0` | Script binding `ChangeMainViewportSize`: an empty stub (`xor eax, eax; ret`) in this build |
| `0x24BB40` | Applies the video-out settings: frame period and wait type from `framerate_control` |
| `0x81090` | `SetGameFrameWaitType` (wait type by hash), hooked; the current type at `0x29E3D68` |
| `0x807E0`, `0x803F0` | The `Auto` (60 fps) and `Variable` frame waits of the `__game_main` clock |
| `0x7F840`, `0x7FDB0` | Main loop frame start (game time step, frame count) and end (frame wait), hooked |
| `0x2C6C7BC` | The main loop's frame count, copied there from the loop object's `+0x150` |
| `0x24C220` | Renderer per-frame step (vtable `0x21086E0`, slot 6), hooked: applies the wanted render size |
| `0x2496A0` | Renderer display mode request (slot 10): display and wanted render size |
| `0x24B2A0` | Rebuilds the size-dependent targets at the current render size |
| `0x24AD80` | Display object: swap chain resize |
| `0x1C0AD0`, `0x1C0C70` | Exec of the 2D plugins (`GrPlugin2D`, `GrPlugin2DFrontmost`; `GrPlugin2DShrink`) |
| `0x437B60` | Main camera selector's update: the active camera into the render view's `GrCamera`, hooked for photo mode; the selector at `[0x2BA1668]` |
| `0x2B65D40` | Pause manager (vtable `0x20C56C0`, Create `0xF1C80`); game time (`0x816A0`) stops while a request is active |
| `0x1A45540` | Reads the window's raw input for the game's mouse and keyboard, hooked for photo mode |
| `0x2BD3600` | `WeatherManager` (see "Weather and time"); clock at `+0x80`, requests at `+0x58` |
| `0x536550`, `0x551430`, `0x54F8F0` | Set the clock, request and cancel weather, called by the weather menu |
| `0x537DD0` | `WeatherManager` update; its pause bit 8 test at `0x537FBF`, made to run in photo mode |
| `0x22C6F0`, `0x22C8B0` | Model visibility tests (see "Scene objects"); `rcx` the visibility manager, `r8d` its capacity |
| `0x165BD0` | `StageBlockControllerData` setup: copies a location's streaming settings into a description and creates its stream controller (see "Draw distance") |
| `0x15F010` | Creates the stream controller from that description (only the setup calls it), hooked for the draw distance |
| `0x2B73434` | The level of detail's reference resolution, set by `GrTools.SetLodFactorResolution` (`0x1BB2D0`); 0 uses each view's width |
| `0x2B74330`, `0x2B77FB0` | The models' and the vegetation's `RejectionLengthBias` settings; `+0x64` the value the draw jobs read |
| `0x205AF0` | Selects those two settings' value by a graphics quality's name, hooked for the draw distance |
| `0x79D330` | Sun and moon light update (see "Sun movement"); its stepped test at `0x79D51D`, patched |
| `0x6B96E0` | Sets the light's settings from the weather's parameters every frame (`0x550410` reads one) |

### Scene objects

Every loaded model the game may draw, on screen or not, with its world matrix and its model file, found
2026-10-22 from the outside (Frida and `ReadProcessMemory`; scripts in `%TEMP%\mgsv-rtscene`), in a
level at Camp Omega's cliffs. No names in the exe: the class names here are descriptive.

- **Visibility manager** (vtable `0x21065C8`), the one with capacity 6144 (`0xDEF9D00` in that run;
  two more with capacity 256 had no records). The visibility tests `0x22C6F0` and `0x22C8B0` take it in
  `rcx` and the capacity in `r8d`, about 300 calls a second from the job threads (one a view: main
  camera, shadow cascades). Objects are handles: bits 16 to 27 a chunk, bits 8 to 15 a slot. The chunk
  table at `+0x88` has 24-byte entries (chunk pointer, handle tag, element size `0x30` at `+0x10`); 256
  records a chunk, chunks not contiguous. The tag's low 32 bits are the chunk's handle (`0x3` in the top
  nibble, the chunk's number in bits 16 to 23); its high 32 bits hold other data in some chunks (Africa,
  2026-10-23: from chunk 11 of the 6144 manager, from chunk 0 of three smaller ones with 248, 39 and 5
  records). Read as one 64-bit word, the scene's walk stopped at the first such chunk and left about
  60% of the placements out (1,075 records of 2,675), which draw capture then filled in where they were
  drawn. Every manager the tests run with counts, not only the one whose test comes first in a frame.
- **Record** (`0x30` bytes): `+0x10`, `+0x18` the box as signed 16-bit x, y, z in half metres (min, max);
  `+0x20` the manager plus `0x100` while in use; `+0x28` the owner. One record an instance.
- **Instance group** (vtable `0x2106720`, constructor `0x239770`): `+0x40` its model, `+0x4C` the
  instance count, `+0x50` the records, `+0x58` the ids, `+0x60` the matrix holder (vtable `0x210D9A0`),
  whose `+0x20` and `+0x28` begin and end an array of world matrices (4x4, column-major: translation
  at `+0x30`, `1` at `+0x3C`). The props and buildings the game draws one by one with skinning shaders
  and `m_useWeightCount` 0 (see "Ray-traced shadows").
- **Model** (vtable `0x20F4D90`, `0x200` bytes), shared by a group's instances or an instance itself:
  `+0x40` the world matrix (also at `+0xA0`), `+0x80` the local box (min, max as float4), `+0x118` the
  resource handle (vtable `0x21027C0`, `0xA0` bytes: `+0x30` the file's 64-bit path hash, `+0x60` the
  file in memory, `+0x68` its size). Models whose file has bones are animated (their matrix is often
  identity); the rest are rigid. `+0x1A4` the passes it is hidden from, a bit a pass: the draw-list
  builder (`0x25BE00`) leaves it out of a pass whose bit is set; the game's model handles set it to 0
  (shown) or all bits (hidden) (`0x2165F0`). `+0x1A8` the passes it may be drawn in, `+0x1AC` a layer
  the builder matches. Found 2026-10-23 in Afghanistan: 294 of 882 single models hidden, among them
  copies of cliff rocks a few metres from the drawn ones (in the ray-traced scene, rock filling the
  hollows the game shows open). Frame records with the draws' constants: no hidden model was drawn,
  every shown one in view was. The scene leaves hidden models out.
- **The model file** is the whole FMDL (version 2.04), kept in memory: section 0 (meshes, vertex
  formats, buffer offsets, LOD face ranges, boxes) and section 1 (the vertex and index data). The
  layout matches the community's FmdlTool: meshes (block 3, 48 bytes: vertex count, first face vertex,
  first face info, alpha enum), mesh formats (block 10), vertex formats (block 11, position usage 0 as
  three floats), buffer offsets (block 14: positions, other attributes, 16-bit indices), face infos
  (block 17, per LOD). Checked on one model: positions span exactly its box, every LOD's indices fit.
- In that level: 4096 records, 216 groups (3156 instances), 454 single models (285 of them rigid),
  and 7 more owner types not decoded yet (`0x20FAFC0`, `0x2113C20`, `0x210CCB0`, `0x20F6760`,
  `0x21139A0`, `0x20F67E0`, `0x210CC30`). Of 36 rigid G-buffer draws sampled, 29 were group or model
  instances; 5 were meshes near (-1463, -549, -331) (below the world, not identified), 2 unmatched.
- Not in this manager: terrain, the object brush's vegetation and props, characters.
- **Clone arrays** (vtable `0x21566E0`, found 2026-10-23): Fox Engine's `StaticModelArray`, entities
  placing one model many times (rows of rocks and props): `+0x78` the model resource (as a model's),
  `+0x80` the model's path hash, `+0x98` its `.geom`'s, `+0xC8` the matrices' count (32 bits), `+0xD0`
  the world matrices (as an instance group's), `+0xE0` a colour each. Nothing in the visibility manager
  points at them, but each one's placements are an instance group's: all 3,089 placements of the 329
  arrays in Afghanistan had a group of the same model at the same matrix, and the game draws them from
  the groups (frame records: every drawn placement had a group record). The scene took them as well
  until 2026-10-23, every one twice; their constructors (`0x4438E0`, `0x443A20`) and destructor
  (`0x443C50`) are no longer hooked.
- **Object brush** (found 2026-10-23): the vegetation and scattered props over the terrain. The brush
  (`ObjectBrush`, vtable `0x2165200`; constructors `0x483460`, `0x483550`, destructor `0x483A80`) has its
  name at `+0x48` and its plugins at `+0x130` (count) and `+0x138` (links, vtable `0x20BBAA8`, the plugin
  at `+0x18`, null where an index is unused). The plugins: clone (`0x2166630`), bush (`0x21A63F0`) and
  fluttering grass (`0x21AD270`) keep their model resource at `+0x90`, its hash at `+0x98` and their scale
  range at `+0xA8`, `+0xAC`; skeleton models (`0x21A40E0`, destructible objects with their own resource
  type) and static models (`0x2165C60`) are not taken yet. The loaded blocks (`ObjectBrushBlock`, vtable
  `0x2165350`; constructors `0x483650`, `0x483730`, `0x483860`, destructor `0x483B20`), the 5 x 5 around
  the camera, name their brush at `+0xA0` (the brush's name object), keep their id at `+0xB8` (row x 64 +
  column) and their placements at `+0x98`: a range (begin, end), then 16-byte groups from begin + 24
  (the records' address, their count (16 bits), the plugin index (8), 1 (8), a type (32)), each record 24
  bytes: Y (float), X and Z (16-bit, 255 a metre around the block's centre), the rotation (four halves,
  x y z w), the block's id, the plugin index and a scale byte (across the plugin's range), an id. The list
  ends in other data; only records naming their block and their group's plugin count. The grid was
  fitted in Afghanistan: 128 m blocks from (-4096, -4096); with it the placements' heights match across
  block edges (median 0.36 m against 16 m for the rotated fit) and those within 4 m of a known terrain
  point sit 1.6 m from it. Read in game 2026-10-23: 46,359 records in 234 groups of 25 blocks, each
  naming its block and group's plugin, none repeated. The first reading took the groups from begin + 16,
  which paired each count with the group before's records: 8,750 records were read twice and about
  21,500 not at all (the shadow probe listed every bush crossed twice). The 255 to the metre was
  measured 2026-10-25 in Africa against the game's own draws: a frame record with the clone shader's
  draws dumped (their per-instance stream, `<call>-streams.txt`: three matrix rows, then 0 1 0 0) gave
  794 instances, each with a record of the same rotation, scale and height; read as 255 to the metre,
  all 794 sat where the game drew them (within the dump's 6 digits), read as 256, 2 did and the rest
  up to 34 cm nearer the block's centre: the ray-traced scene's rocks missed the drawn ones, and sun
  rays from a drawn rock's top started inside the scene's copy of it.
- **Texture manager** (found 2026-10-23): the global `0x2B79B18` points to it (vtable `0x21046E8`). It
  keeps an array of 0x78-byte texture records, as many as its capacity (`+0x28`, 4881 in that run, `+0x2C`
  the count, 2649); a record's handle (`+0x40`, vtable `0x2104710`, 0x50 bytes) has the texture's path
  hash at `+0x20` (the FMDL's block 21 value), its record's index at `+0x34` and its resource at `+0x10`,
  whose first word points to an entry holding the `ID3D11Texture2D` at `+0x10` (and `+0x10` to one
  holding its shader resource view at `+8`). The manager's streaming list (`+0x10` to `+0x18`, 24-byte
  nodes whose first word points 0x10 into a record) leaves a few loaded textures out. Neither the model
  objects nor their materials hold the texture: the draws bind it by id.
- **Every model** (found 2026-10-23): the object registry (vtable `0x20F08C8`; the global at RVA
  `0x2BA04A8` points to it) keeps a list of each object type's instances: at `+0x1E8` a table of list
  headers by type number (`+0x38` of an object, a word; models are type 3, the global at RVA
  `0x29E51B4`), each header the last object, the first and the count; objects are linked first to last
  through `+0x10` (and back through `+0x18`). In Afghanistan the model list held 882 models: 566 rigid,
  360 skinned. Characters are among them and in no visibility manager.
- **Characters** (found 2026-10-23): a skinned model (its file has bones) is an ordinary model object:
  its world matrix at `+0x40` (identity while unused), the passes it is hidden from at `+0x1A4`, its
  bone count at `+0xF8`, and at `[+0x188] + 0x78` its skinning palette: a 3x4 matrix (rows, translation
  in the fourth column, in the model's space and with the bind pose taken out) for each of its file's
  bones, in the file's bone order. A mesh's palette is that array through its bone group: the draw's
  `b6` slot k is the model's bone `group[k]` (checked at the draw replay `0x2D6310`: equal to the last
  bit, for a soldier's body). The game keeps pools of bodies: of 18 soldier bodies of one file, 4 were
  shown and animated, 14 hidden with identity matrices and the bind pose (palette all identity).
  The palette is current when the frame's draws are replayed. Weapons and props a character holds are
  skinned models of their own (a rifle: 2 bones, one weight a vertex), placed at the hand: their world
  matrix is made from the hand's bone, its bottom row (0 0 0 1) give or take rounding (0.99999988
  measured for the 1), which the scene took for no matrix until 2026-10-23.
- **Skinned model files**: block 0 the bones (48 bytes: name, parent, box, flags; then the local and
  the world (bind) position as float4: positions only, no rotations); block 5 the bone groups (68 bytes:
  a word, the count, 32 bone numbers); a mesh's bone group at `+6`. Its second vertex stream adds the
  bone weights (usage 1, four 8-bit unorms) and bone numbers (usage 7, four 8-bit integers into the
  mesh's bone group) beside the texture coordinates (usage 8, two halves).
- **Mesh flags** (an FMDL mesh's first two bytes, alpha and shadow), measured 2026-10-23 from the
  game's own draws: hooking the draw packet replay `0x2D6310` (`rcx` the replay state, its constant
  buffers at `+0x28`, 32-byte slots with the source at `+8`; `rdx` the packet, an indexed draw's count
  and first index in the low 16 and next 24 bits of `+0x18`) for a second, each draw taken to its
  model by the object constants' translation and to its mesh by its index range, and its pass by the
  scene constants' `c7` (`(0,0,0,1)` for the orthographic shadow views). Per mesh:
  shadow bit 0: drawn in the main view only, never into the shadow maps (alpha 0: 32 meshes, alpha
  32: 11); alpha 80 (bit 6) with shadow 0: main view only (35); alpha 160 with shadow 64 or 65
  (decals): main view only (53); alpha 0 and 32 with shadow 0: both passes. A pylon's lattice: the
  visible mesh (alpha 32, shadow 1) main view only, a mesh with alpha 128 and shadow 66 (bit 1) shadow
  maps only. Sky domes 4 to 7.6 km wide, centred on the camera (scales 2000 to 3800, alpha 80,
  shadow 5), have shadow bit 0. Measured again 2026-10-23 in Afghanistan, every rigid draw for a
  second, with the same hook: alpha 128 with shadow 64 casts (all 16 draws of 7 meshes went into the
  shadow maps through `ModelForward_Shadow`, the alpha-tested shadow shader; a trench's ceiling grating,
  its frame opaque and its grid such a quad); alpha 160 with shadow 64 is mixed, decided by the
  material's technique (the FMDL material type's StrCode64: two meshes of `0x69CCC8871944` cast, one of
  `fox3DDF_Blin_NC` did not), so the flags alone do not tell. The game keeps a pass mask in each mesh's
  draw item (`+0x80`), which its draw-list builders test (`0x25BE00` and five like it); reading those
  is the exact answer, not done yet. A tree's twigs in Africa (model `84a32080686afb21`, mesh 0, alpha
  160 with shadow 64) cast in the game's shadow maps and were missing from the rays' shadows, so until
  the pass masks are read the ray-traced scene takes every decal (2026-10-23): it takes all but shadow
  bit 0 and alpha bit 6, and since 2026-10-25 the opaque meshes with shadow bit 0 as well ("Ray-traced
  scene", "Characters"). Which of them the shadow pass
  alpha-tests was measured per draw (its `cPSSystem` `m_param.w`, 1 testing, 0 not): alpha 32 (bit 5)
  and the shadow-only decal (alpha 128, shadow 66) test; alpha 16 (glass, shadow 4) does not and casts a
  solid shadow. The scene takes alpha bits 5 and 7 as cut out, the rest as opaque. Glass with shadow
  bit 0 (a helicopter's windows, alpha 16 with shadow 5) the game never casts, and the scene leaves it
  out for every model (2026-10-25): taken, it kept the sun out of the helicopter's cabin.

## What the add-on changes

DLSS runs after the game's depth of field and before the rest of its post processing, in the place
of its FXAA, at native resolution (DLAA) or from a lower render size (Quality, Balanced,
Performance, Ultra Performance, Custom): the lens flare layer, bloom's composite and the grading then
work on its image, as NVIDIA's guide asks. The depth of field stays before it, as in Unreal Engine's
order: its blur radius comes from the jittered depth, and drawn after DLSS its focus edges flickered
with the jitter (2026-10-25: gone with **Jitter off**, still there at DLAA, where DLSS's image was
its colour). Bloom and exposure are still measured from the scene before DLSS, as the game measured
them before FXAA.
Everything runs from ReShade events, apart from the start-up device check, two hooks on the motion
blur plugin, the hook on the renderer's per-frame step, the optional frame rate unlock (see "Frame
rate"), the photo mode hooks (see "Photo mode"), the game loop hooks (see "Frame generation" and
"Weather and time") and, with frame generation, the swap chain detour.

- **Shaders.** `init_pipeline` identifies pixel shaders by DXBC checksum: `Fxaa`,
  `MotionBlurCameraVelocity`, `GammaControl` and the grading's `CopyRenderBuffer`,
  `ColorCorrection_BlndLut_Tex_Gen` and `Tonemap`. All are drawn as one full-screen triangle, so
  `draw` only looks at the bound pixel shader for 3-vertex draws. The 2D pass's vertex shaders are
  known by their source path (see "The 2D pass").
- **Command lists.** The game records a whole frame on its worker threads, finishes about 44
  command lists, and only then executes them on the immediate context in one burst, followed by
  Present (measured over 200 frames: never a finish after the first execute of a frame). What a
  deferred context records that matters (the grading, the FXAA pass, the velocity texture) is
  kept by context, moved to the command list at `FinishCommandList` (ReShade's
  `execute_secondary_command_list` with the new list and the deferred context) and taken at its
  `ExecuteCommandList`. That event fires before the real call.
- **Where DLSS runs.** Post processing starts its own command list: in all 126 F10 records (SDR and
  HDR, DLAA and upscaling, render sizes 1280x720 to 3840x2160) the list before it ends with a draw
  into the scene (particles, forward geometry), and it opens with exposure (a `CopyRenderBuffer` from
  the light target to a quarter size), then `CopyReductionX4` and the depth of field. Lens flares
  into the half-size layer may follow in lists of their own; the next list holds the half-size
  layer's composite, the LUT build, `Tonemap` and `Fxaa`; then come the UI's list and
  `GammaControl`'s. A list that records the LUT build, `Tonemap` or `Fxaa` holds the grading, and
  DLSS runs at the frame's first such execute, before the list. The FXAA hook always keeps that
  list (below).
- **FXAA skipped.** The `Fxaa` draw is recorded for its bindings (the scene target and the depth
  copy, which DLSS reads) and then skipped: FXAA would only blur DLSS's image. The frame is recorded
  before it executes, so the frame's own FXAA pass is known when its grading starts. The
  skip is decided while recording, so a frame DLSS cannot handle (a mode change) shows without
  anti-aliasing.
- **Frames and jitter.** A frame starts with the first main-view scene upload after the immediate
  context executed anything; all of its main-view uploads get that frame's jitter. The FXAA pass
  carries the frame it was recorded in, and DLSS runs only if that is still the latest frame and
  directly follows the last evaluated one (otherwise it falls back or resets).
- **Jitter.** At `unmap_buffer_region`, with the data still mapped, 480-byte constant buffers are
  checked by content (`camera.cpp`): a perspective `c4-c7` (`c7 = 0,0,1,0`), aspect matching
  `c22.xy`, `c28 = 0`. Those uploads get `c4.z += jx`, `c5.z += jy`, and `c28.xy = (jx, jy)` in NDC.
  Where `c0-c3` is P x (something affine), `c0 += jx·c3` and `c1 += jy·c3` as well (motion blur's
  pass keeps other matrices in `c8-c11` and is still jittered). Shadow views are orthographic and
  untouched. Halton(2, 3), 8 phases at DLAA; DLSS gets (jx, jy) in render pixels, x right, y down,
  confirmed with 4x jitter (only that sign is sharp). The first upload whose `c0-c3` is `P x V`
  with `V` in `c8-c11` is the frame's camera, captured before the jitter. A frame DLSS did not run
  on leaves the next unjittered, so menus and 3D views without FXAA never shake.
- **Motion vectors.** A compute shader before DLSS: camera motion from the depth copy (the FXAA
  pass's depth target, `R32_FLOAT_X8X24_TYPELESS` view; the G-buffer depth plus forward geometry)
  and one clip-to-previous-clip matrix, `prev P x V x inverse(cur P x V)`, built on the CPU in
  double, so the camera's absolute position cancels. The pixel's NDC has the jitter removed, so
  the vectors carry none. Where the engine's velocity mask is set, the motion is clamped to within
  half a quantisation step (64/255 px) of the engine's decoded velocity less this frame's jitter,
  unless the engine's value is saturated at 64 px: object motion at 8-bit precision, static parts
  of masked objects at full precision. Checked in game:
  zero on every frame with a still camera; the previous DLSS output warped by the vectors aligns
  with the current one at offset 0 (searched in 1/8 px) on a pan; within 0.25 px of
  `MotionBlurCameraVelocity`'s output.
- **Camera cuts.** Reset when the camera moves more than 2 m, turns more than 25 degrees or
  changes P11 by more than 10% in one frame (gameplay stays far below: 2 m is 120 m/s at 60 fps).
  The title screen alternates between two cameras 1197.7 m apart with identical orientation
  every 2 to 5 frames, so it resets that often there; gameplay showed none. In three launches
  this happened only in the title's first 15 seconds (about 40 resets), never later while the
  menu showed.
- **Evaluation.** In the add-on's own `ID3DDeviceContextState`: the inputs pass copies the colour
  (the scene target with the depth of field, through a view of FXAA's render target format:
  sRGB-encoded LDR, as the resolve tonemapped it, into `R8G8B8A8_UNORM`), depth (`R32_FLOAT`) and
  motion (`R16G16_FLOAT`); NGX evaluates through ReShade's wrapped immediate context with
  `DepthInverted | MVLowRes`, no `IsHDR`, MV scale 1 and a 1x1 exposure texture holding 1. NGX is
  only ever called on the immediate context. With HDR the colour is the scene's linear light before
  any tonemap or grading, in `R16G16B16A16_FLOAT`, and the feature has `IsHDR` (see "HDR").
- **Composite.** A full-screen draw writes the DLSS output with RCAS (AMD FidelityFX, MIT; 33% by
  default) and alpha 0: at DLAA over the scene target, through FXAA's view; when upscaling into the
  UI target (below). The grading reads only the colour and writes its own alpha (luminance, for
  FXAA). RCAS and the debug views therefore come before the grading.
- **Frame generation's hudless colour** is copied from the same target at the immediate context's
  next execute after the `Fxaa` list, once the grading has drawn and before the UI does.
- **Render size.** `render_scale.cpp` checks the build and the per-frame step's first bytes
  (`40 53 48 83 EC 20 48 8B D9 8B 89 98 01 00 00`) and detours it with MinHook. Before the step runs
  it writes the requested size to the wanted size and sets flag 2, so the engine rebuilds its targets
  itself, on its own thread, between frames. A wanted size the hook did not write is the engine's own
  choice (start-up, a display change); it is restored when no size is requested (Off, DLAA). A
  request only applies while the display size matches the one it was planned for, and with flag 4
  set the step is left alone and corrected on the next frame. A mode change therefore costs one
  rebuild, like a resolution change, and Off renders at native resolution again from the next frame.
- **Plan.** At each present, for the upscaling modes: NGX's optimal render size for the swap chain
  size and the mode (Custom: output x scale), rounded up to even as the engine rounds its own. Only
  the mode, the custom scale and the output size make a new plan; the overlay applies the custom
  scale and the texture bias when their sliders are released, so a drag does not rebuild the
  targets every frame. The feature is created for render -> output with the mode's quality value.
  Jitter phases follow the ratio, `ceil(8 x (output / render)^2)` (32 at Performance).
- **The UI at the output size** (`ui_redirect.cpp`). While the engine's targets are smaller than
  the swap chain, the add-on keeps a target in the scene's format (`B8G8R8A8_TYPELESS`, float with
  HDR) at the output size, a second one for copies, and a `D24_UNORM_S8_UINT` depth-stencil:
  - Before the first command list that draws on it runs, it is filled: DLSS composites into it, or
    when DLSS did not run this frame (menus without a 3D view, loading screens, fallbacks) the
    render-size scene target is stretched into it bilinearly, as `GammaControl` would have. The
    grading and the UI never draw over a stale frame.
  - The grading's full-size draws go to it too, whoever draws them (the game, or HDR's grading).
    Their render target is swapped, without the depth-stencil; viewports and scissors are scaled by
    output / render; slot b0 gets a copy of the game's `cPSSystem`, made on the context before each
    draw, with `m_renderInfo` and `m_renderBuffer` at the output size (partial copies, as for
    `cPSScene` below); the game's bindings come back after. The shaders address their inputs by
    texture coordinate (`CopyRenderBuffer` from the clip position through `cPSObject` c12, `Tonemap`
    at `SV_Position x m_renderBuffer.zw`), so smaller inputs still line up.
  - In every F10 record the only `CopyRenderBuffer` draws into a render-size target are the depth of
    field's copy of the scene (its input has the render size; it stays, before DLSS) and the
    half-size layer's composite over the scene (lens flares): that one goes to the output target.
  - `Tonemap` (the grading): the output target is copied into the second target on the context, and
    the grading reads it (slot 0) and writes the output target. The graded image is then stretched
    back into the render-size scene target, which the game copies next for its 2D filters
    (chromatic aberration, radial blur, water drops).
  - A draw with a `Draw2D` vertex shader bound (tracked per context from `bind_pipeline`) into the
    render-size scene target is made by the add-on on the output target instead, with viewports and
    scissors scaled by output / render and its own `cPSSystem` (`c1` and `c2` at the output size)
    in slot b0, then the game's bindings are restored and the game's draw is skipped. Slot b2 gets
    a copy of the game's `cPSScene`, made on the context before each such draw, with
    `m_viewportSize.xy` copied over from a buffer holding the output size (a partial copy into a
    constant buffer, which Direct3D 11.1 drivers allow). A clear of the scene target from such a
    context goes to the output target.
  - Every clear of a render-size depth-stencil also clears the output-size one, so the 2D pass's
    stencil masks work.
  - `GammaControl` reads the output target's `_SRGB` view in place of the scene target.
  - When `GammaControl`'s viewport has its scene's size and the back buffer is larger (the exclusive
    fullscreen box), every `GammaControl` draw gets the viewport and scissor over the whole back
    buffer: the redirected one, frame generation's hudless one, the HDR one, and the game's own when
    the redirect declines, which then stretches the scene as it does windowed.
  - The window, swap chain and mouse are untouched; the engine never learns the output size differs.

  Checked in game: at 1920x1080 windowed, Performance (960x540) on the title menu and in free roam
  with the menu text and HUD at full resolution; at 3840x2160 borderless, Quality 2560x1440,
  Balanced 2228x1254, Performance 1920x1080 and Ultra Performance 1280x720, the HUD pixel for pixel
  the same in Performance and Ultra Performance.
  Modes switched at runtime many times: over 22,000 frames had 4 fallbacks, each the frame right
  after a mode change. With the 60 fps cap lifted, every mode ran at 93 to 96 fps at 4K: the game is
  CPU-limited there on an RTX 5080.
- **Texture LOD bias.** Material samplers get `log2(render / output)` plus the overlay's offset
  (-1 at Performance), through biased copies bound after the game's (`push_descriptors`), on every
  context, except for the 2D pass. The game creates 16 distinct samplers (logged in development):
  point, bilinear, trilinear and anisotropic in wrap and clamp variants, and two comparison
  samplers. The rule takes the mipmapped, non-point wrap or mirror samplers; screen-space passes
  use the clamped and point ones, and a bias would also move their explicit mip reads.
- **Motion blur.** DLSS needs the engine's velocity for characters, which the game draws only
  with its motion blur on, but not the blur, which would only smear its output. `motion_blur.cpp`
  checks the build and the first bytes of PreExec (`80 79 68 00 49 8B 80 70 05 00 00`) and Exec
  (`48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20`) and detours both with MinHook at `AddonInit`.
  While DLSS replaces FXAA (any DLSS mode, not the FXAA comparison), PreExec runs the game's code
  and then sets the velocity bit, and Exec returns without drawing. So the velocity passes run
  every frame whatever the game's setting, and the blur passes do not run at all. Off and the
  comparison give both back to the game's setting from the next frame. When a check fails nothing
  is hooked and the game's setting stays in charge. `AddonUninit` removes all engine hooks with
  `MH_Uninitialize`. Checked with F10 records:

  | Motion blur | Add-on | Velocity passes | Blur passes |
  | --- | --- | --- | --- |
  | Off | DLAA | yes | none |
  | Off | Off | none | none |
  | Low | DLAA | yes | none |
  | Low | Off, or the FXAA comparison | yes | tiles and both McGuire passes |

  With motion blur Off, the object motion debug view covers Snake standing and sprinting and the
  nearby trees; the ghosting of limbs moving toward the camera, seen without the hooks, is gone.
  The still-camera motion vectors stay zero, still-camera stability and a pan match the build
  without the hooks, and 10,000 frames had no fallback.
- **FXAA with post processing down.** `fxaa.cpp` checks the build and the first bytes of the FXAA
  plugin's Exec (`49 8B 80 70 05 00 00 8B 90 00 01 00 00`) and detours it with MinHook at
  `AddonInit`. While DLSS replaces FXAA, the detour keeps the view test and runs the filter without
  testing the post processing dword, so DLSS has its pass at every setting. Off and the comparison
  give the pass back to the game's setting from the next frame. Checked on the title menu with post
  processing Low and DLAA: DLSS ran on every frame (before DLSS moved ahead of post processing).
- **Fallbacks.** Missing or mismatched FXAA bindings, sizes that differ, a frame recorded out of
  step, no FXAA pass, no plan yet or a render size the engine has not reached yet after a mode
  change leave the frame without DLSS and without FXAA (stretched into the UI target when
  upscaling, where the grading still draws), and log the reason once. An exception or NGX error
  stops DLSS until a mode is chosen again; the engine then renders at native resolution. A size
  change recreates the feature and textures.

Not handled:

- The skin subsurface-scattering filter gets no jitter offset (see "Jitter"). Its shaders only use
  the offset for the view direction of a pixel, which the jitter (at most half a pixel) turns by
  about 0.01 degrees, so it is left alone.
- Heat haze (`Primitive_TppHeatHaze`, `hazeMirage*`), particles, and grass and bushes moving in the
  wind have no motion vectors. None was seen to ghost in the tested scenes; a DLSS
  bias-current-colour mask drawn with the same vertex shaders would be the fix if one does.
- The title screen's resets (see "Camera cuts"): only in its first 15 seconds, so left alone.
- 2D filters that redraw the whole screen from the scene copy (chromatic aberration, radial blur)
  read the render-size copy, so while one is on, the image under it has the render size.
- Bloom and the lens flares start from the scene before DLSS, jitter included; at a quarter of its
  size it averages out. The game's motion blur stays off while DLSS runs.

## Frame generation

DLSS Frame Generation runs through NVIDIA Streamline, which supports Direct3D 12 and Vulkan only,
so the game presents through Direct3D 12 by the route of the Days Gone add-on (1.7.1). With
`frame_generation` set to `2x` to `6x` or `dynamic` in `PhantomFX.ini`, it starts with the game;
changing the multiplier, or turning generation off, applies at once. With it off at start-up none
of this is installed and the game presents as in 1.0.0.

- **Swap chain.** The game makes its swap chain on its render thread with
  `IDXGIFactory::CreateSwapChain` (`R8G8B8A8_UNORM_SRGB`, `DISCARD`, 3 buffers, `ALLOW_MODE_SWITCH`,
  first 1024x576, then `SetFullscreenState(FALSE)` and `ResizeBuffers` to the configured size).
  ReShade loads the add-on inside `D3D11CreateDevice`, before that; the add-on then detours DXGI's
  own `CreateSwapChain` (factory slot 10) behind ReShade's factory proxy. The function is read
  from the table entry in `dxgi.dll`'s file and relocated, since some setups replace the entry in
  memory. The game's swap chain becomes a stand-in (`src/presenter.cpp`) whose back buffer is a
  texture in the game's format that Direct3D 11 shares with Direct3D 12. A native Direct3D 12 device
  on the same GPU makes its queue and the real flip-model swap chain through Streamline's proxies,
  so Streamline owns presentation. Flip model takes no sRGB formats, so the real swap chain is
  `R8G8B8A8_UNORM`; the copy moves the same bits. Each present signals a shared fence after the
  game's frame; Direct3D 12 copies the back buffer into the swap chain's current one, and Direct3D
  11 waits for the copy before the next frame draws into the same texture. Calls that do not
  concern the back buffer pass on to the real swap chain, and the stand-in reports the game's own
  present count. When Streamline does not start (an unsupported GPU, missing files), the game keeps
  its own swap chain. The add-on module is pinned while the stand-in can exist.
- **ReShade.** ReShade wraps the stand-in as the game's swap chain: its overlay and effects draw
  into the back buffer before the copy, and add-ons get Direct3D 11 events as without frame
  generation. RenoDX's DLSS 5 add-on kept finding the game's DLSS with frame generation on.
- **Hudless colour.** Frame generation finds the HUD where its hudless colour differs from the
  back buffer, and `GammaControl` changes every pixel on its way to the back buffer (it applies the
  brightness setting), so the hudless colour goes through `GammaControl` too. After DLSS's composite,
  its image (the scene target at DLAA, the UI target when upscaling; both `B8G8R8A8_TYPELESS` at
  the output size) is copied into a source texture before the UI draws over it. Where a context
  records `GammaControl`, the add-on records the same draw once more, with the game's constants and
  sampler, from the source's sRGB view into a shared `R8G8B8A8_TYPELESS` texture through an
  `R8G8B8A8_UNORM_SRGB` view, the back buffer's bits. Streamline is told it is `R8G8B8A8_UNORM`
  (`hudLessBufferFormat`), as the real swap chain holds them. The hudless colour counts only when
  the command list that drew it executed after DLSS ran in the same frame. The 2D camera filters
  (blood and water on the screen, radial blur) draw with the HUD, so they count as HUD.
- **Inputs.** DLSS's render-size depth (`R32_FLOAT`, reversed-Z) and motion (`R16G16_FLOAT`, render
  pixels, without the jitter) are created shared while the game presents through Direct3D 12. Each
  present tags them and the hudless colour through Direct3D 12 as `eOnlyValidNow`, so Streamline
  copies them in the command list that copies the back buffer, before Direct3D 11 reuses them. A
  frame without DLSS (Off, menus without a 3D view, loading screens, a fallback) turns generation
  off until DLSS runs again; the history resets with DLSS's.
- **Camera.** From the main view captured before the jitter (`camera::streamlineCamera`).
  Streamline takes row vectors and a view space with x right, y up and z forward; Fox's view
  mirrors x (P00 < 0), so the mirror moves into the view: `cameraViewToClip` is P with its first
  column negated, transposed; right is the view's row 0 negated, up row 1, forward row 2; the
  position is `m_eyepos`, near and far `m_viewportSize.zw`, the field of view from P11.
  `clipToPrevClip` is the motion pass's `prev P x V x inverse(cur P x V)`, transposed.
- **Reflex.** Frame generation paces its frames with Reflex, whose markers must carry one frame
  number on every thread. The main loop runs on the game's main thread: its frame start
  (`0x7F840`) steps game time and increments the frame count at the loop object's `+0x150` (copied
  to `0x2C6C7BC`), the update follows, and its frame end (`0x7FDB0`) ends in the frame wait. The
  loop thread itself makes the frame's first main-view upload, about 4 ms before the frame end; a
  worker thread executes the frame, DLSS included, during the next loop frame, and the render thread
  presents it right after. Traced in game: the frame recorded during loop frame N was evaluated and
  presented as N, every frame. `src/game_loop.cpp` checks both functions' first bytes
  (`40 53 56 57 48 83 EC 60 48 C7 44 24 20 FE FF FF FF` and
  `48 83 EC 28 C7 81 A8 00 00 00 00 00 00 00`) and detours them with MinHook: before the frame
  start, Reflex sleeps and marks the simulation start of count + 1; before the frame end, the
  simulation end and the render submit start. The frame's first main-view upload takes the loop's
  count, which the evaluation, the tags and the present markers carry. When a check fails nothing
  is hooked and the present marks the next frame's sleep and simulation.
- **Frame rate.** The Reflex sleep at the frame start is separate from the game's frame wait at the
  end: with `framerate_control` `Auto` the wait still holds 60 fps, and Reflex sleeps only while the
  GPU is behind. **Unlock frame
  rate** works as without frame generation. Dynamic chooses the multiplier for the display's
  refresh rate and Reflex then holds the rendered frame rate below it: on a 144 Hz display it
  rendered 47 fps at 3x, 141 presented.

Checked in game at 3840x2160 borderless on an RTX 5080, with RenoDX's DLSS 5 add-on loaded:
the stand-in made the Direct3D 12 swap chain and followed the game's resize; DLSS ran with frame
generation in DLAA and Quality, through mode and preset changes (each rebuild one "frame rate over
100 ms" warning in `sl.log`), at 2x and Dynamic (3x); the overlay and ReShade's frame counter drew
into the frames; the game released the swap chain cleanly at exit.

## FSR upscaling

With `upscaler=fsr` in `PhantomFX.ini` AMD FSR takes DLSS's place after the FXAA pass, with the
same inputs, composite and sharpening. AMD's runtime picks the version: FSR 4 on Radeon RX 7000 and
9000, FSR 3.1 on other GPUs; the overlay's status line shows it. The port follows the Days Gone
add-on (1.7.1). FSR runs on Direct3D 12 only.

- **Interop.** The colour, depth, motion and output textures are created shared while FSR is chosen.
  A native Direct3D 12 device on the game's GPU opens them (`src/fsr_upscaler.cpp`); Direct3D 11
  signals a shared fence and flushes after the inputs pass, Direct3D 12 runs FSR on its own queue,
  and Direct3D 11 waits on the GPU before the composite reads the output.
- **Colour.** FSR is made for linear colour, so the inputs pass decodes the sRGB-encoded LDR image
  into `R16G16B16A16_FLOAT` in SDR too, and the composite encodes FSR's output again, as for DLSS in
  HDR. With HDR FSR runs in its HDR mode.
- **Camera.** Reversed-Z with a finite far plane (`FFX_UPSCALE_ENABLE_DEPTH_INVERTED`, not
  infinite); near and far from `m_viewportSize.zw`, the vertical field of view from P11, metres.
  Motion vectors and jitter are DLSS's: render pixels, motion toward the previous frame without the
  jitter, jitter with y down. There is no reactive mask, as DLSS gets no bias mask.
- **Modes.** AMD's ratios: Quality 1.5x, Balanced 1.7x, Performance 2x, Ultra Performance 3x, and
  Native AA in place of DLAA. The FSR context is made for the render and output sizes and HDR; a new
  one starts a new history. The upscaler not chosen frees its memory, and both do with upscaling off.

## FSR frame generation

With `frame_generation=fsr` the presenter makes AMD's frame generation swap chain in place of
Streamline's proxy, on the same stand-in route; Streamline is not loaded and there is no Reflex. It
works with either upscaler, and switching between DLSS and FSR frame generation takes a restart.

- **Queue.** AMD's swap chain paces the frames and generates on the queue it presents from, so the
  presenter gives it the copy queue: the copy, the hudless copy and FSR's preparation of depth and
  motion come first on that queue, and Direct3D 11's next frame still waits only for the copy.
- **Inputs.** The same as DLSS Frame Generation's: the upscaler's depth and motion, the hudless
  colour drawn through `GammaControl`, and the camera from `camera::streamlineCamera` (position and
  axes in the world, near and far, field of view). FSR generates at present, after Direct3D 11 may
  have drawn the next frame's hudless colour, so it reads a Direct3D 12 copy, made
  `R8G8B8A8_UNORM` from the typeless SDR texture (`R10G10B10A2_UNORM` with HDR, where the back buffer
  is PQ).
- **Frames.** Each frame configures generation with the game loop's frame number, which must rise by
  one; a gap resets it. Frames without upscaling turn generation off. The generation context follows
  the back buffer's size, format and HDR, and is made again after a resize.

## HDR

With `hdr=1` in `PhantomFX.ini` (a restart applies it) the game outputs HDR10 (`src/hdr.*`). The
game tonemaps early, in the deferred resolve (`TppTonemap`, `cPSMaterial` c0 = a, b, c, usually
4.87, 0.6, 0.453: x up to b, then b + c - 1 / (a (x - b + c)), which an 8-bit target clamps at 1,
reached at x = 4) and inline in 23 forward shaders (sky, clouds, oceans, wormholes, sand wind), and
everything after worked on that image in 8 bits. With HDR the scene keeps its light up to the
grading, as RenoDX's mods do, and the grading and output follow RenoDX's and Luma's defaults.

- **Swap chain.** `create_swapchain`: `R10G10B10A2_UNORM`, flip model (`FLIP_DISCARD`, at least 2
  buffers). At `init_swapchain` the colour space is set on the native swap chain (ReShade's proxy
  skips `SetColorSpace1` once an add-on changed the format): `G2084_NONE_P2020` when the output's
  `DXGI_OUTPUT_DESC1` says Windows shows HDR, otherwise `G22_NONE_P709` and SDR output. ReShade's
  runtime is told the same with `set_color_space`. The game's `R8G8B8A8_UNORM_SRGB` views of the back
  buffer become `R10G10B10A2_UNORM`. With frame generation the presenter makes its swap chain from the
  changed description, and the hudless texture is `R10G10B10A2_UNORM`.
- **Scene targets.** The scene target and its copy are the renderer's first two size-dependent
  targets (renderer `+0x1E8`, `+0x1F4`), made through `0x242620` at start-up (return addresses
  `0x2BA676`, `0x2BA6DF`) and `0x242AC0` in each rebuild (`0x24B2C1`, `0x24B2D9`). `create_resource`
  recognises them by those return addresses on the stack and makes them `R16G16B16A16_FLOAT` with
  unordered access; their views take that format. They keep sRGB-encoded values, now beyond 1.
- **sRGB views.** An 8-bit target's `_SRGB` view encodes on writing and decodes on reading; a float
  one cannot. The resolve draws through such a view, blend off, so after it (whoever draws it) a
  compute pass encodes the target in place; a clear through one gets its colour encoded. The
  reflections read the copy through such views: each `CopyResource` into the copy is followed by a
  decode into a linear twin, which those views are swapped for when bound (`push_descriptors`).
  `CopyDepthBuffer` binds the scene's sRGB view with no colour writes and is left alone.
- **TppTonemap out.** At `create_pipeline`, every pixel shader with TppTonemap's block, as the game
  compiled it unoptimised (14 instructions: `ge, movc, mul, mov, add, add, mov, add, mul, div, add,
  add, mul, add`, the movc's literals 1/0 and 0, the div's -1), is patched: the movc's second literal
  takes the first's, so the linear part is always kept, and the curve's multiply becomes an `and`
  with the zero beside it (a multiply would keep the curve's pole as NaN). The three sky draws'
  clamp to 1 within 8 instructions after it (`min` with a literal of ones) becomes 65504. 27 shaders:
  the 4 resolves and the 23 forward ones. The patched code gets its checksum again (`resign`), and
  Direct3D must take it before the game does, or the game keeps its own. `init_pipeline` hands every
  part of the add-on the game's own code, so shaders are still known by their checksum.
- **What expects TppTonemap's image.** `CopyReductionX4` from the scene, the quarter-size copy bloom
  and the lens flares start from, draws with a shader that puts each of its four taps through
  TppTonemap and the clamp to 1, so bloom is as the game had it. The depth of field's
  `CopyRenderBuffer` from the scene into its 8-bit copy is followed by a copy of the scene into a
  float texture, which `DOF_ScatterCompositeFinal` reads in its place.
  The reflections' twin is not clamped: `LocalReflectionAddBuffer` rewrites the whole scene from it.
  The game's depth of field takes its highlights' luminance from the resolve's alpha (`a^2 x 32`),
  which is unchanged.
- **The 2D pass.** Its pixel shaders (`\Draw2D*_ps.hlsl` in their debug data, 67) write alpha above 1,
  which the 8-bit target clamped and a float one does not (black boxes behind subtitles): every write
  to `o0` gets the saturate modifier.
- **Grading.** `Tonemap` (the LUT pass) draws with the add-on's shader: the scene's light through
  TppTonemap (the resolve's curve, captured into a buffer at each resolve) and the clamp to 1, then the
  game's grading as its shader does it (bloom screened in gamma 2, the 16x16x16 LUT), then RenoDX's
  `UpgradeToneMap`: the luminance the tonemap took is added to the graded luminance, and the graded
  colour scaled to it. Below the knee (0.6) the image is the game's. Alpha keeps the game's luma.
- **DLSS** runs with `IsHDR` on the linear light, after the depth of field and before the grading;
  RCAS sharpens through x / (1 + x).
- **Output.** `GammaControl` draws with the add-on's shader: the game's brightness setting as it
  applies it, the 0 to 1 range decoded with gamma 2.2 (the SDR look on 2.2 displays, Luma's and
  RenoDX's default) and above it with sRGB, then RenoDX's Neutwo on the brightest channel, which puts
  100 times the game's white at the display's peak (`hdr_peak_nits`, 0 for the peak the display
  reports) and leaves values well below it nearly unchanged, BT.709 to BT.2020, the game's white at
  `hdr_paper_white_nits` (203), and PQ. Without HDR on the display: sRGB, clipped at 1.

Not handled:

- Particles or glass that blend with alpha above 1 would darken their background, as the 2D pass did;
  none were seen.
- The reflections' own targets are 8-bit, so reflected highlights stop at the game's white.
- Motion blur (only without DLSS) blurs through an 8-bit copy, so moving highlights stop at white.
- The HUD has the scene's paper white; it is not separate.

## Soft shadows

With **Soft shadows (PCSS)** on, the sun pass inside the shadow range filters the shadow map with
contact-hardening soft shadows (Randima Fernando, "Percentage-Closer Soft Shadows", 2005) in place
of the game's four comparisons. It runs in the replacement shaders of "Screen-space shadows", with
the options in `b6`. Beyond the shadow range there is no shadow map, so the game's far shader
draws there unless sun contact shadows are on too.

- **Cascade units.** A cascade's x, y and z share one scale: the rows of `m_shadowProjection` have
  the same length (0.152 per metre for the 13 m first cascade; `m_shadowProjection2` row 3 scales
  it for cascades 1 to 3), and row 2 is the direction towards the sun at that scale. So the search,
  the penumbra and the receiver's slope are all in cascade units: an atlas uv is a quarter unit, and
  depth is `shadowDepth x (z + 1)`, larger towards the sun, with `shadowDepth` in `c16.w`.
- **Receiver plane.** The receiver's tangents come from the depth of its four neighbours, each axis
  taking the neighbour nearer in depth so a pixel at an edge keeps its side. Through the cascade
  matrix they give the plane's rise towards the sun per unit across, clamped to 4. Each sample
  compares against the plane at its offset plus a bias: (1 + |slope.x| + |slope.y|) texels, as the
  four texels a gather or a comparison reads lie up to a texel from the plane's point, and 0.1 per
  unit of offset for curved receivers.
- **Blocker search.** 16 `Gather`s (64 depths) on a Vogel disc whose radius is 20 m of occluder
  height at the sun's angular radius (`tan(sun size / 2)`), from 1 to 128 texels. No blocker
  returns lit; 64 blockers return shadow.
- **Filter.** 24 bilinear comparisons on a Vogel disc whose radius is the blockers' mean height
  above the plane times that tangent, from 1 texel up to the search radius. The result is the
  share of the sun that reaches the receiver, not squared like the game's `(1 - s)^2` for its fixed
  two-texel filter. The distance fade, the offset, the clouds and the height fade follow as in the
  game.
- **Noise.** Each pixel turns the discs by interleaved gradient noise. While DLSS replaces FXAA the
  turn also moves with the frame, repeating after 64 frames, so DLSS averages the pattern; without
  DLSS it stays still.
- **Sun size.** The angular diameter in the overlay, from 0.1 to 4 degrees, 0.53 (the real sun) by
  default. The game's specular sun disc is far larger (`cPSLight` `c1.w`, about 2.5 degrees of
  radius).

Checked on WARP with a synthetic scene, a tilted sun over flat ground and plates 1 m and 8 m above
it: no self-shadowing with the sun at 36 and 72 degrees from the ground's normal, a sharp shadow
under the 1 m plate and a wide penumbra under the 8 m one. With both options off the replacement
still matches the game bit for bit.

Not handled:

- Occluders more than 20 m above the receiver: the penumbra stops widening at that height.
- Penumbrae narrower than a texel stay a texel wide: at the real sun size, occluders closer than
  about 5 m above the receiver in the last cascade (2.4 cm texels), 0.7 m in the first (3 mm).

## Screen-space shadows

With **Sun contact shadows** on, the sun's shadow is multiplied by Bend Studio's screen-space
shadows (`bend_sss_gpu.h` and `bend_sss_cpu.h` from "Inside Bend: Screen Space Shadows", Apache
License 2.0): contact shadows traced through the depth buffer, which the shadow map is too coarse
to give. The game has no shadow mask to multiply (see "Sun light and shadows"), so the sun pass
draws with replacement shaders. With soft shadows off too, the game's own shaders draw; the add-on
then only checks a flag on each full-screen draw. **Lamp contact shadows** trace the same way for
every point and spot light (`drawLocalLight` in `src/shadows.cpp`), one trace per light, so they
are a toggle of their own: the sun's is one trace a frame, the lamps' grows with the lights on
screen.

- **Replacement shaders** (`src/shadows_shaders.hpp`). `SunLight_DL_CS_ES` and
  `SunLight_DL_ES`, and their rain pair without the sky reflection (`SKY_REFLECTION 0`),
  rebuilt in HLSL from their disassembly, with the same bindings, plus the
  options in `b6` (soft shadows, the sun's tangent, the noise frame, screen-space shadows) and the
  screen-space shadow in `t7`: inside the shadow range it multiplies the shadow map's shadow before
  the specular floors; beyond it, it is the shadow. With both options off all four give the game's
  output bit for bit, checked on WARP over 65,536 pixels of random G-buffer, shadow map and lookup
  texture data, with cascade blend on and off. They are identified by checksum at
  `init_pipeline`; `SunLight_ES` keeps the game's shadows.
- **Trace.** At the first sun draw on a deferred context since its last `FinishCommandList` (or
  present), Bend's compute shader traces from the depth the pass binds (`t2`) into an `R8_UNORM`
  texture at the depth's size, cleared to 1 first (the trace skips the sky). Bend's GPU code is
  ported to FXC: the early-out votes through groupshared memory, as shader model 5 has no wave
  intrinsics. `Bend::BuildDispatchList` (`third_party/bend_sss`, unchanged) lays out its 1 to 8
  dispatches from the light's position: `(direction, 0)` through the pass's own projection, built
  from `cPSScene` `c21` (`1 / P00`, `1 / P11`, B, A) and `c28` (the jitter, while DLSS jitters), so
  it matches the depth. The direction is `cPSLight` `c0`. Both buffers are kept per thread as the
  game writes them (Map, memcpy, Unmap on the recording thread, after the upscaler has added the
  jitter; `gi_inputs.cpp` keeps them, as for the ray tracing) and looked up by the buffers the draw
  binds. Depth is reversed (near 1, far 0, the border colour). Bend's starting values: 60 samples,
  surface thickness 0.5%, bilinear threshold 2%, contrast 4, early-out on.
- **Foliage casts no shadow.** The trace reads the material from the pass's specular target
  (`t1`, `z` x 255; foliage is 43, as in "GTVBAO") and skips foliage samples as Bend skips edge
  pixels. Taken as solid surfaces 0.5% deep, a clump's blades shaded each other: in a sunlit
  scene at 11:27, the trace darkened bushes and grass clumps by 7 to 24% and bare ground by
  nothing. The sun's shadow map already holds the blades (cascade 0 has 3 mm texels), and
  foliage still receives the trace from everything else. Without a specular target the trace
  reads 0 and treats nothing as foliage.
- **Draw.** The game's compute bindings in the slots the trace uses are saved and restored around
  it; the replacement pixel shader, `t7` and `b6` are bound for the game's draw, made by the
  add-on, and the game's shader, `t7` and `b6` restored. The options apply from the frame after
  the next present, so both parts of a frame's sun pass see the same. A sun pass without its depth or constants, or an error, keeps
  the game's draw and logs the reason once.
- **Lamps trace only what they reach.** A light's range comes from its falloff in `cPSLight` `c6`
  (from the disassembly, `d` the distance plus `c6.x` for points): `SSLighting2_*` light
  `1 / d^2 - d^2 c6.w`, zero at `c6.w^(-1/4)`; `Lighting_*_NA` light `(1 - d^16 c6.z) / d^2`, zero
  at `c6.z^(-1/16)`. The range's sphere around `c3` gives a screen rectangle (the planes through the
  eye that touch it, per axis) for `BuildDispatchList`'s bounds, and a depth range for Bend's
  early-out, so a lamp costs its share of the screen, not all of it. The lamp's trace is not
  cleared; its shadow draw reads the trace's constants (`b6`) and the depth (`t2`) and takes pixels
  outside both bounds as lit. A sphere behind the camera or off screen keeps the game's draws; one
  through the camera plane, or an unknown range, traces the whole screen.
- **Diagnostics.** "Sun contact shadows only" draws both parts with the far shader: the sun is then
  shadowed by the trace alone.

Checked in game at 3840x2160 with DLAA and frame generation, in Afghanistan at dusk and in
daylight: the shaders compiled and both sun shaders were identified at start-up, the trace ran
every frame (one dispatch with the sun off screen), the lit image looked as before, and contact
shadows showed under Snake's boots, in grass and around rocks. With the trace alone, characters
close to the camera keep only the shadow near their feet.

Not handled:

- The trace reaches 60 pixels at the render size: near characters fill much of the screen, so only
  the start of their shadow is found, and at 4K it reaches half as far in the world as at 1080p.
  The shadow map gives the rest.
- Forward and transparent objects, drawn after the sun, neither cast nor receive the trace.

## Global illumination

The indirect light that replaces the game's baked probe light (`src/gi_light.cpp`,
`src/shaders/gi_light.hlsl`, `src/gi_denoiser.cpp`), rebuilt on 2026-10-24 the way path tracers with a
radiance cache work it out. Until then it was AMD's GI-1.2 (screen probes into the same hash grid, and
Capsaicin's denoiser); a review that day found its lag and blotches built in:

- **Three slow histories in a row.** A cache cell takes 1/16 of a frame's light; three in four probes
  traced no rays a frame and kept theirs, and the blurred probes became the next frame's history; the
  denoiser kept up to 64 frames and its change check moved 1/8 a frame. From the code, a light 100 times
  dimmer took the denoiser alone 32 frames to come within 2x, cache and denoiser 90 (1.5 s), the probes
  more; the logs of the flashlight tests that morning agreed (3 s, then 0.85 s with the flashlights lit
  afresh). The anti-lag tried in 1.5.0 compared one noisy frame with the history, so noise tripped it.
- **Brightening damped too.** A probe cell more than twice as bright kept up to 90% of its old light,
  and three rays in four went where a probe had been bright, so a dark cell was looked at about every
  16 frames.
- **Blotches when moving.** A cell's size follows its distance from the camera, so moving made new,
  empty cells, which read their tile's coarse average until they had 16 samples; one probe tile in four
  was traced a frame, and new ones took 64 rays once or borrowed far probes, while the denoiser blurred
  8 pixels over probe-sized noise; the reprojection followed only the camera, so moving characters lost
  their history every frame.

Now, each frame:

- **Rays** (`TraceLight`). 1, 2 or 4 (the global illumination's ray quality; Medium, 2, by default) from
  each pixel of a grid half the render size each way, cosine-weighted from the depth buffer's surface,
  past copies of it. A ray that gets away takes the sky's light (`SkyRadiance`). A light pixel samples one
  render pixel of its 2 x 2, by depth (`RenderPixelOf`): the nearest on one checkerboard colour, the
  farthest on the other, as half-resolution effects downsample depth. With the top left, as first built,
  a wire, a fence or a grass blade under a pixel wide had it one frame and what lay behind the next as the
  jitter moved them, and the denoiser started their light over every frame with the camera still. The
  first ray's bounce feeds the cache and reads it after its update; the others read their cell as it
  stands, a frame old (the cache's buffers hold a hit a light pixel). Their light is averaged; their
  directions, weighed by it, go to the denoiser's spherical harmonics, shorter than 1 where they spread.
  At 1 ray a light pixel, shaded surfaces lit by a few bright openings crawled with leftover noise after
  a full history ("underwater"); 4 rays cleared most of it (2026-10-25), at about four times the rays' cost.
  Foliage (material 1, below) takes twice the rays: swaying and thin, it restarts its history every few
  frames, so its light is mostly the frame's rays, and shaded grass boiled as the player walked by it.
  Twice the rays wherever the history was under ten frames, in place of on foliage, made no difference
  seen (2026-10-25).
  Rays guided by the last frame's light (half of them in a cone around its denoised direction, weighed by
  one-sample MIS, as GI-1.2 guided its probes' rays) were tried the same day and left out: no steadier in
  the end, and the light looked darker.
- **The hit, lit afresh.** A shadow ray to a point of the sun's disc (of the **Sun size**, as the
  shadows' rays; a leaf lets half through from its far side) and one to a point or spot light picked in
  proportion to its light there (weighted reservoir sampling over the frame's lights, flashlights among
  them), times the hit's albedo (`HitAlbedo`).
  Nothing of the hit's own light comes from a history.
- **Its bounce.** A cosine-weighted ray from the hit (a leaf's to its far side a third of the time,
  counting 1.5): the sky's light where it gets away, else the hash grid cell's where it lands, direct and
  indirect (`ResolveLight`, after the cache's update). The cache only ends the path, as NVIDIA's SHaRC
  does in a path tracer: its lag and coarseness come after a bounce and an albedo.
- **The cache** (AMD's two-level hash grid of GI-1.2, unchanged): the bounce hits' cells take up to
  four of their hits' light a frame (`PopulateCells`: sun, sky and the lights, each through a shadow ray),
  blended 1/16; 30% of them trace on to a third hit whose cell's light gives them one bounce more
  (`PopulateMultibounceCells`, `UpdateMultibounceCells`); tiles nothing touched for 50 frames go.
- **Foliage's light by world position** (`RememberFoliageLight`, `UpdateFoliageMemory`). Grass and
  leaves sway and the denoiser starts their light over every few frames, so a cell in the world keeps it
  instead: cells about 16 render pixels across where they lie (power-of-two sizes, 6.25 cm at the least)
  in a hash table of 2^19 slots in buckets of 8. Each frame a foliage light pixel adds its light to its
  cell, found at a point jittered by up to half a cell each way (blending neighbours over the frames), and
  takes 80% of its light from the cell as the last frame left it once the cell has 8 frames; the cell
  blends each frame's light in over half a second, and goes after 2 seconds without foliage. With it,
  shaded foliage no longer boiled (2026-10-25). The rays log counts foliage light pixels without room.
- **Denoiser.** NVIDIA's NRD 4.18 (`RELAX_DIFFUSE_SH`), at the rays' grid, integrated natively
  (`gi_denoiser.cpp`): one root signature for its pipelines (the constants at the root, its textures and
  storage textures in one table, its two samplers static), its own shader-visible heap, a texture per
  pool entry, each texture's state tracked between its dispatches. Its guides (`PrepareGuides`): the view
  depth (the sky's beyond the denoising range), the normal (`NRD_FrontEnd_PackNormalAndRoughness`,
  R10G10B10A2) with material 1 on the game's foliage (the specular target's material 43, which the
  G-buffer pack puts in the normals' alpha), and 2.5D motion: the camera's, or a moving mesh's from the
  game's velocity where it drew one, and the view depth's change: the camera's, and a moving mesh's from
  the last frame's view depth where it was when that lies within 10% of its own (as if it stood still,
  the change was off by the camera's movement where the camera follows it, and the player's light
  started over every frame). The camera goes in with the engine's x
  mirror moved from the projection into the view (as for Streamline), and, as NRD requires, without the
  upscaler's jitter (c28 taken out of the projections, the motion without the jitter's change, the jitter
  apart as `cameraJitter`): with it in, grass and fine detail shimmered as the jitter moved them, most
  visibly with DLSS preset M. Foliage is NRD's strands (`strandMaterialID`, 1 cm
  thick), whose history survives far more depth change: the game sways its grass and bushes without
  velocity. History: half a second at the real frame rate (smoothed as NRD asks), a fast history of a
  tenth of a second that the long one is clamped to, three frames rebuilt after a reset, anti-firefly on;
  a camera cut (`camera::cameraCut`) restarts it. Tried on 2026-10-25 and left out: REBLUR in place of
  RELAX (blurrier), a wider disocclusion threshold, and the render pixels' light without their own normal
  (neither steadier).
- **The render size** (`CompositeLight`). The four light pixels around each render pixel, weighed as a
  bilinear filter would times how alike their view depth (within 5%) and normal (cosine to the 8th) are,
  else the one nearest in depth; each resolved against the render pixel's normal
  (`NRD_SH_ResolveDiffuse`), scaled to keep the light it has at its own normal. Irradiance over pi times
  the exposure, as before.
- **Albedo learning** (`LearnAlbedo`): one light pixel of each 16 x 16 a frame.

ReSTIR GI's spatial reuse was tried on 2026-10-24 (each light pixel resampling its own ray's light and
six neighbours' on its surface, with the Jacobian and a visibility ray for a chosen neighbour's sample,
this frame's samples only): in game the light was far less stable than without it. Picking one of seven
samples makes the noise come in clumps that move every frame, which the denoiser smooths far worse than
noise a pixel at a time (Epic's MegaLights left ReSTIR out for the same reason). Removed.

Next step planned, once measured as needed: a history confidence for the denoiser from samples traced
again with the last frame's random numbers (NRD's `IN_DIFF_CONFIDENCE`).

## Ray-traced scene

What the global illumination and the ray-traced shadows trace (`src/gi_scene.cpp`, its use in
`src/gi_renderer.cpp`), laid out as DXR scenes are. Rebuilt 2026-10-23 from one structure per
captured draw or model part with every triangle expanded; since stage 5 of that rewrite (the same day)
it is the game's own scene throughout (see "Draw capture (removed)" below).

- **Geometry pool.** One 384 MB buffer: vertices of 16 bytes (position, then the texture coordinates
  as two halves where a mesh has them) and 16-bit indices, from 16-byte boundaries. A model's meshes
  keep the game's own index buffers (the FMDL's most detailed face range), so its vertices are stored
  once. The terrain rings (unindexed) and the characters' poses are written there by kernels
  (`TerrainGrid`, `Skin`).
- **Bottom levels.** A model's structure has one geometry per mesh the scene takes; opaque meshes
  carry `D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE`, so the hardware runs no candidate code for them, and
  only cut-out meshes reach the alpha test. Static structures are built `PREFER_FAST_TRACE |
  ALLOW_COMPACTION` within a budget a frame (128 builds, 300k triangles, 64 MB of scratch), emit their
  compacted size with the build, and are copied into that room when the frame slot comes round
  (`COPY_MODE_COMPACT`); the old room returns once the GPU is past that frame. Offline they compact to
  34 to 40% of their size. The terrain rings and the characters are built in place or refitted
  (`ALLOW_UPDATE`).
- **Geometry table** (`GIGeometry`, `t16`). A row per geometry, a structure's rows contiguous and in its
  order: the vertices' and indices' offsets, `GI_GEOMETRY_CUT_OUT`, `GI_GEOMETRY_UV`, its base texture
  (1 + its index in `g_SceneTextures`) and, for cut-out geometry with one (`GI_GEOMETRY_ALPHA_TEXTURE`),
  the alpha below which light passes. The CPU keeps the table and copies the rows changed each frame.
  The learnt mean albedo (`LearnAlbedo`) is kept by row, for geometry without a texture, and starts
  over for each new row (`ClearNewGeometries`).
- **Textures** (added 2026-10-23; draws no longer involved). A model mesh's base texture is its material
  instance's texture parameter named `Base_Tex_SRGB` (FMDL: mesh `+4` material instance; block 4 texture
  count `+6`, first parameter `+8`; block 7 parameters, name and texture; block 6 textures, path index;
  block 21 path hashes; block 22 name hashes); a mesh without one (a shadow-only decal) has its
  `Mask_Tex_LIN` (`0x3ED9B75CD480`), cut by its alpha as the game's shadow pass does (`ModelForward_Shadow`'s
  pixel shader samples `t0.w`; checked 2026-10-24: a billboard tree's and a roof's stand-ins have the visible
  mesh's own colour texture as their mask, whose red let the rays through). The names are Fox Engine's StrCode64: CityHash64 version
  1.0.3 of the name and its terminating zero, seeds `0x9AE16A3B2F90404F` and (first character << 16) +
  length, 48 bits kept (`Base_Tex_SRGB` = `0x41EA7BE81B61`). The game's loaded textures are in its
  texture manager ("Scene objects"); `gi_textures.cpp` copies each wanted one's levels of detail from the
  first no larger than 512 pixels (a cut-out mesh's) or 128 (the others') into a shared texture of the
  typeless format, viewed as sRGB on Direct3D 12, and looks again every 300 frames for the game's
  replacement as finer levels stream in. Checked in game against the G-buffer's `t0`: 370 of 377 meshes'
  base textures were the very texture the game drew with; 6 were missing from the manager's streaming
  list (the array is walked since), 1 mesh's shader samples another texture at `t0`. On Direct3D 12 each
  texture has a descriptor in a range after the frame tables (`t0` on in `space2`, 4096), written when it
  arrives, a new one for a replacement, the old retired once the GPU is past the frame; a texture no model
  uses is let go. They stay in the common state: the shaders' reads promote them. The copies are made
  before the frame's inputs are signalled to Direct3D 12. Before, the slice copies and the terrain's came
  after the signal, so a terrain ring made again could read the heightmap's copy while Direct3D 11 still
  wrote it: in game the shadow probe showed one pixel's sun ray blocked by the terrain, then by nothing,
  flipping every two seconds or so with the camera still, the rings' refresh.
- **Alpha test.** The game's shadow pass (`ModelForward_Shadow`) discards where the base texture's alpha is
  below `m_param.w x 0.5`, `m_param` being the pixel shaders' per-draw system constants (`cPSSystem`, `b0`,
  mapped per draw); the G-buffer below `m_param.w` times a dither averaging a half. Measured at the draws
  (the 64-byte buffer's Map): `m_param` is (0, 0, 0, 1) on cut-out meshes' shadow draws and (0, 0, 0, 0)
  where nothing is tested. The rays test a cut-out geometry's alpha
  against `CutOutThreshold` (0.5) at the level of detail its ray cone meets (`TextureLod`: Akenine-Moller
  et al., Ray Tracing Gems chapter 20; the cone a pixel wide at the receiver, not spreading: rays to the
  sun run parallel, and its disc comes from the rays' spread over it). Spreading the cone as the sun's
  disc as well, as first built, read coarse levels of detail far from the receiver: at the default
  0.53 degrees a pylon's thin bars 20 to 40 m up fell under the threshold and its shadow broke into the
  bars' joints (2026-10-23).
  Reading `b0` back at the kick, as the first alpha copies did, gave the last draw's constants (0, so every
  texel blocked: the pylon's solid shadow).
- **Instances.** The frame's `D3D12_RAYTRACING_INSTANCE_DESC`s (up to 131,072) are both the top
  level's input and what the kernels read (`t5`): one per placement of a model and per character, with
  its transform; the terrain rings with identity. `InstanceID` holds the kind (`GI_INSTANCE_*`, low 8
  bits); `InstanceContributionToHitGroupIndex`
  holds the first row of its structure, so a ray's geometry is `CandidateInstanceContributionToHitGroupIndex()
  + CandidateGeometryIndex()`. Kernels reach a hit's triangle through `LoadTriangle` (`gi_common.hlsli`):
  instance, row, indices, vertices, transform. A hit the global illumination keeps packs its instance (20
  bits) and its geometry within the structure (12 bits) into one word.
- **Recording order.** The pool's uploads and the table's rows; `ClearNewGeometries`, `TerrainGrid`,
  `Skin`; the bottom levels and compactions; the instances' copy; the top level. Between frames the pool, the table and the instances are in the non-pixel shader resource state.
- **Characters** (2026-10-23). The snapshot also walks the object registry's model list (see "Scene
  objects", "Every model") for the skinned models the game shows, each with its world matrix, its
  skinning palette and its mesh groups' states, and the files' parse takes skinned models too: each
  vertex's four bones (through its mesh's bone group to the model's bone numbers) and weights, and each
  mesh's group. In the pool a skinned model lays out its bind pose, indices and skin (16 bytes a
  vertex: four 16-bit bones, four 8-bit weights) and has no structure of its own; each character gets
  its own room for its posed vertices and a refittable structure of the meshes it shows (a mesh group
  drawn while none of bits 0x33 of its state is set; a character showing other meshes is made again).
  Each frame with a new snapshot the palettes go up with the frame's other uploads and `Skin`
  (`gi_scene.hlsl`) poses every character's meshes into their rooms, a thread a vertex, as the game's
  skinning shaders do (each weight's bone's 3x4 matrix, the weights scaled to add up to one); the
  structures are refitted, and built again every 30 frames. The palettes are not the snapshot's: it reads them at the
  frame's first visibility test, before the game animates the bones for the frame it draws (2026-10-24:
  in the helicopter a lamp's shadow of Snake trailed his moving arms and face; at the kick 10 to 15% of
  the palettes had moved on, every kick in the next game frame). Each main-camera draw of a skinned mesh
  (`m_useWeightCount`, `b5` `c8.x`, set) gives its bones instead (`VSBones` `g_vsBone`, `b6`: 32 3x4
  rows, a mesh's palette through its bone group), its model found by its place (the view's inverse
  times `m_viewWorld`'s translation, within 2 cm of the character's matrix) and its mesh by its index
  range (the FMDL's most detailed faces: first face plus the level's first, and its count); bones no
  draw had are read again at the kick; the log counts the draws placed. A character whose refit finds no scratch
  is left out of that frame. Its instances carry `GI_INSTANCE_MODEL | GI_INSTANCE_CHARACTER`. The props
  and weapons a character holds are skinned models of their own; the player's rifle and pistol were
  missing until 2026-10-23, left out for their rounded matrices (see "Characters"): found by logging the
  skinned draws no character showed, while draw capture still ran. Marked soldiers' silhouettes
  (layer bit 0x20, see "Characters") are left out: the game draws them in the main view only.
  A character's meshes are taken as the main view shows them, not as its shadow maps do (2026-10-24):
  its meshes with shadow bit 1 (drawn into the shadow maps only: stand-ins) are left out, those with
  shadow bit 0 (main view only) and see-through ones (alpha bit 6) taken, the see-through ones cut out
  by their alpha; its main-view glass (alpha bit 4 with shadow bit 0) stays out. The helicopter's
  windows (model `84a12883d81ea5e3`, meshes 12, 13, 17, 21, 22 and 25) were taken until 2026-10-25 and
  shaded its cabin: the shadow probe found the cabin's sunward surfaces blocked by mesh 13 0.2 to 1.8 m
  away, and the camera's ray meeting it at the window, 2.7 m before the game's surface. Before, the shadow probe on a woman in the sun found her hair missing (every mesh of
  its model left out: the rays saw her bald, 1,270-triangle head, and its outline crossed her hair as a
  jagged shadow) and a 40-triangle stand-in of her outfit lying on her skin, blocking her neck's and
  chest's sun rays within a centimetre. Rigid models keep the shadow maps' meshes (a pylon's lattice is
  a stand-in, its visible mesh is not cast), and since 2026-10-25 also take the opaque meshes the main
  view alone shows (shadow bit 0, no alpha bits 4, 5 or 7): a building casts its shadows with an outer
  shell, and a room's inner walls, floor and ceiling have shadow bit 0. Without them a shelf's shadow
  hung below it, a lit line between, and a dark line crossed the wall above a door frame: the debug view
  "Shadow ray starts" showed the room's walls farther than the game's by 3 cm or more, its floor and
  ceiling with no surface near, and the shadow probe found the start on the back of the shell (model
  `84a120f9301becbe`, mesh 21) 7 cm behind the wall, with 61 of the wall's 64 global illumination rays on
  back faces. Main-view cut-out meshes and decals stay out: taken, crossed cards (model
  `84a1da1c1308e887`, mesh 3) 44 cm above a road, where the game showed nothing, cast a moon shadow.
  Open: a tarp on a fence (model `84a1bc5b4702c16b`) is a
  main-view cut-out (mesh 2) cast in the game by a flat 4-triangle stand-in (mesh 3, shadow bit 1) up
  to 25 cm off it, so its shadow rays start on the stand-in or meet it, in patches that change with the
  distance; taking a rigid model as its main view shows it, as a character is, would fix it, untried
  as it may drop a building's shell. The shadow probe lists every model at a character's place
  with all its meshes: each one's group and state, alpha and shadow bytes, whether it is shown and drawn
  (posed by a draw this frame), and the meshes left out.
- **Model files read once.** A model the renderer has not placed for 600 frames is let go and its file
  read again when the game places it again. The 600 frames count from the model's arrival: counted from
  frame 0, a model the frame's lists did not place went as it came, and the walk of the model list
  queued every listed model's file, rigid ones too, before its other tests. In one session that read
  105,181 files for about 1,600 models.
- **Vegetation.** The object brush's placements join the game's models in the snapshot
  (`engine_brush.cpp` reads a block once, when first seen). The bushes' and grass's meshes are flagged alpha 0xA0 (a
  cut-out decal) and shadow 0x41 or 0x40 (bit 0: no shadow; the game draws none for its vegetation), which
  the models' rule leaves out: models from the brush's plugins take them as cut out. In the first session
  with the brush (Afghanistan, 25 blocks) the captured meshes fell from 1,800 to 167 and the geometry pool
  from 91% to 16%. Placements come grouped by model, so the renderer looks a
  model up once a run.
- **Leaves let light through** (2026-10-24). The brush's cut-out meshes are leaves (`GI_GEOMETRY_LEAF`;
  its props stay opaque): a thin surface that reflects and lets through light diffusely, as pbrt's
  `DiffuseTransmissionMaterial`. Where the global illumination's rays hit one (`HitDirectLight`), it adds
  half of what its albedo would reflect from its far side (`LeafTransmittance`; green leaves let through
  a half to two thirds as much light as they reflect, by leaf optics measurements such as PROSPECT): the
  sun, through a shadow ray from that side when the sun is behind it, and the sky, whose one ray goes to
  the far side in proportion to what it gives there (a chance of 1/3), each side's ray counting 1.5.
  Before, a bush was black inside with the global illumination on: the shadow probe found every one of
  a bush pixel's 64 rays meeting leaves within a metre, each lit on its outer side alone. The sun's
  shadow rays still stop at a leaf: the light it lets through is scattered, and its glow carries it to
  what is around. Trees placed as models are not flagged yet.
- **Draw capture (removed).** Until stage 5 (2026-10-23) what the scene lacked was captured from the
  game's draws: each draw made again through stream output into a ring, turned into world positions at
  the kick, scattered into the pool by entry, kept while the game drew it and culled against a depth
  pyramid when it stopped, with the frame's skinned meshes in one structure built every frame. Once the
  visibility managers were read in full (see "Scene objects", the chunk tags), a second of every draw in
  Africa found the main view's and the shadow passes' rigid draws all in the scene but the sky dome and
  spare soldiers' gear parked about 200 m under the map, and the helicopter's interior added nothing.
  The capture went, with its kernels (`Scatter`, `ClearBounds`, `BuildDepthPyramid`, `CullCaptured`), the
  transfer texture, the per-frame structure, the captured instances' masks and the coverage checks. The
  camera and the terrain's inputs still come from the draws (`gi_inputs.cpp`).
- **Offline checks** (`%TEMP%\phantom-rts\harness`): every earlier mode gives the same numbers, with the
  D3D12 debug layer quiet; mode `multipart` puts the post (eight corners shared through indices, opaque)
  and the alpha card (cut-out) in one model, one structure, and gives the same shadows as `alphacard`.

## Ray-traced shadows

With **Ray-traced shadows** on, the sun's shadow comes from rays traced through the ray-traced scene
(see "Ray-traced scene"), in place of the game's
shadow map, soft shadows and sun contact shadows. The scene runs for them with the global
illumination's light on or off; `global_illumination::Options::shadows` turns them on, and the
light passes run only with `enabled`. Lamp contact shadows stay as they are: lamps are not ray
traced here (see "Ray-traced lights").

- **Rays** (`TraceSunShadows` in `src/shaders/gi_shadow.hlsl`). One ray a pixel at the render
  size, from the depth's world position towards a point on the sun's disc (uniform over the disc of
  the sun size's angular radius, a new point each frame), up to 5 km. The sun's direction is the sun
  pass's (`cPSLight` `c0`), noted the frame before, as the scene is traced before that frame's sun
  pass is recorded.
- **Where a ray leaves** (`ShadowSurfaceAt`, for the sun's rays, the lights' and the debug view's). A
  ray from the eye through the pixel looks for the scene's own surface near the depth's point (within
  3 cm plus 2% of the distance; cut-out geometry by its alpha), searching that stretch alone: the game
  hides what comes between the camera and the player (a bush the camera backs into) and the scene keeps
  it. Searched from the eye, the ray stopped on its leaves and those pixels left from the depth's point,
  inside the coarser rock behind for some, so the hidden bush's leaves showed as shadows on what lay
  behind (the shadow probe, 2026-10-24: the camera's ray met leaves 0.1 to 1.4 m away, the game's
  surface 2.4 to 5.9 m). Found, the shadow ray leaves its exact
  point on the triangle, from the side facing the light, lifted off it along the normal by 16 float
  steps of the point's largest coordinate (`LiftOffTriangle`; coordinates under 128 m count as 128),
  and nothing near it is skipped. That is about 2 mm at 1,200 m from the map's origin, where the game's
  maps reach. The point comes from the triangle's corners and the ray is traced in the instance's own
  space, each a few steps of the largest coordinate off in any direction, as a character's transform
  turns the error of one large coordinate into the others. Wachter and Binder's offset
  (`OffsetFromTriangle`, Ray Tracing Gems chapter 6: 256 steps of each coordinate on its own, which the
  global illumination's rays keep) failed both ways, found by the shadow probe on 2026-10-24. In the
  field it lifted the start about 3 cm: Snake's chest was shadowed in strips along the folds of his
  suit, the start inside the next fold, 5 to 6 mm under its surface, the ray blocked at 0.00 m leaving
  it. In the helicopter, whose floor lies near the map's origin across but 1,200 m up, it lifted next
  to nothing off a face turned sideways (0.0 mm): the ray hit its own triangle from inside in every
  other frame, black blotches flickering on his face. Where the game
  drew something the scene lacks, or holds at another level of detail too far off, it leaves from
  the depth's point along the G-buffer's normal, offset by 2 cm plus 0.2% of the distance, skipping
  copies of the surface just above it ("Copies of a surface" in `gi_common.hlsli`). Until 2026-10-24
  every ray left that way: a lamp's hard shadow showed the skip taking a character pressed to a wall
  for a copy of the wall, light through his back in patches ragged along the wall's bumpy normals.
- **Cut-out meshes.** A model mesh the game alpha-tests (FMDL alpha bits 5 and 7, see "Scene objects",
  mesh flags) is cut-out geometry: non-opaque in its structure, so only it reaches the rays' candidate
  code. In the captured-mesh era the shadow rays' alpha test on the captured grass and bushes' cards (5
  to 8 million triangles) took 8 to 47 ms; the brush's vegetation meshes with their textures cost 1.5 to
  2.6 ms.
- **The game's alpha on cut-out meshes.** A loaded model's cut-out mesh (its alpha bits other than
  translucent, or a shadow-only lattice) is cut-out geometry with its texture coordinates (the FMDL's
  UV0, two halves) in the fourth float of each pool vertex and its base texture (see "Ray-traced
  scene", textures and alpha test). The sun's shadow rays and the shadow debug rays sample its alpha
  where they cross a triangle (`CutOutBlocks`) and pass where it is below the threshold, inside the
  shadow map's range and beyond it. The global illumination's rays test it too (2026-10-23), with a cone
  a pixel wide where they start that spreads as a pixel's angle does (`LightCone`), and let cut-out
  geometry without its texture through: taking every triangle as opaque, as they did since the
  captured-mesh era (when testing alpha made them about five times as costly, 8 to 43 ms), a bank of
  grass was a heap of solid cards to them and went dark. Before, in the same spot, the global
  illumination's ray passes took 0.85 ms (probe rays 0.41, second bounces 0.19, hit lighting 0.24) of
  5.1 ms. Cut-out meshes without their alpha yet are left to the shadow map by the sun's rays.
  Offline harness mode `alphacard`: a card clear on one half shadows exactly with the other.
- **The game's terrain from its heightmap.** The terrain shaders (`NewTerrain_Depth`, `_Shadow`)
  read a virtual heightmap: `virtualUv = v0.xy x c9.xy + c9.zw` (b5, per patch), the page table at
  `t0` (`B8G8R8A8`, 128 x 128: tile x, y and level of detail as texel x 255.4, floored), the height
  atlas at `t1` (`R16_UNORM`, 1024 x 2048), `physicalUv = (frac(virtualUv x b4.c0.xy x 2^-level) +
  tile.xy) x b4.c0.zw + b4.c1.xy`, and world = `(u, height, v) x c10.xyz + c11.xyz`. The draw hooks
  (`gi_inputs.cpp`) note these at the main view's first terrain draw a frame (`engine_scene::noteTerrain`); the kick copies
  both textures' most detailed level into shared textures (`t14`, `t15`) and reads `b4` back now and
  then. `TerrainGrid` (gi_scene.hlsl) makes four rings of 256 x 256 cells around the camera, 1, 4,
  16 and 64 m cells (256 m to 16 km), two triangles a cell sampled as the game's shader samples, the
  cells an inner ring covers inactive (NaN). A ring's corner moves in steps of eight of its cells; a
  ring that moves, or whose inner ring moves, is made again and its structure built again
  (`ALLOW_UPDATE`); every 120 frames, staggered, it is made again and refitted as finer pages stream
  in. A ring whose build finds no scratch left that frame is left out until it is built. Their
  instances carry `GI_INSTANCE_TERRAIN` (brown in the shadow debug view). Harness modes `terrain` (flat heightmap at
  the ground: the same shadows as `beyond`) and `terrainhigh` (a metre up: everything shadowed).
- **Inside and beyond the shadow map.** Each ray works out how far its pixel lies past the game's
  shadow map range, as `SunLight_DL_CS_ES` fades its shadow to lit there (`cPSLight` `c8.x`, `c8.y`
  and `c6.x`, noted from the shadow-mapped sun shaders): 0 inside, 1 past the fade. Cut-out geometry
  with the game's alpha is tested by it everywhere. Cut-out geometry without it (its texture not copied
  yet, or a mesh without texture coordinates) lets the rays through inside, where the game's shadow map
  has its shadow; beyond, each such triangle a ray passes blocks it with chance 0.5 times that share, so
  it casts a half-transparent shadow that the denoiser averages. With the rays alone (the default, see
  "Sun pass") there is no range: such geometry blocks with chance 0.5 everywhere.
- **Denoiser.** AMD's FidelityFX Shadow Denoiser (FidelityFX Denoiser 1.0, MIT,
  `third_party/ffx_denoiser_shadows`, unchanged), with the passes and memory of FidelityFX SDK
  1.1.4's integration: 8x4 tile masks of the ray results, the tile classification's temporal pass
  (moments, a local-neighbourhood clamp, tiles that are all lit or all in shadow skipped), then
  three edge-avoiding à-trous filters 1, 2 and 4 pixels apart, the first writing the next frame's
  history; the last restores contrast (`pow(mean, max(1.2 - variance, 1))`). Deviations: the
  trace writes the tile masks itself through groupshared memory (any wave size); the motion is
  the camera's, reprojected from the depth, and where the game's velocity pass drew a moving mesh
  (characters, the horse, trees in the wind) the game's velocity (see "Velocity"): its
  `MotionBlurCameraVelocity` pass gives the texture (`t3`), copied at the kick when a list that drew
  it ran since the last present, before the kick's list. Until 2026-10-23 the camera's motion alone
  served, which gave moving characters' shadows a lag; the velocity passes run while an upscaler
  replaces FXAA or with the game's motion blur on, and without them the camera's motion serves; no 16-bit
  types; the depth similarity's sigma is 1 m plus 5% of the depth instead of 1 m, as sloping ground
  kilometres away is metres deep per pixel.
- **Sun pass** (`src/shadows.cpp`, `shadows_shaders.hpp`). The shared R16_FLOAT shadow goes to
  `t6` of the replacement sun shaders, with `g_rayTraced` in `b6`. By default (2026-10-23) the rays
  alone shadow the sun: the far shader lights everything, with the rays' shadow. With **Show vanilla
  shadows with RT** (`vanilla_shadows_with_ray_tracing`), inside the shadow map's range the shadow is
  the darker of the shadow map's soft shadow (PCSS, whatever its checkbox) and the rays', and it fades
  to the rays' alone where the game faded to lit; the shadow offset, clouds and height fade follow as
  before. Beyond the range the rays' shadow is the shadow. Until 2026-10-23 that mix was the only way,
  and the rays alone a diagnostic ("Ray-traced shadows only"). With every option
  off the shaders still match the game bit for bit on WARP (cascade blend on and off).
- **Timing.** The shadow is traced in the global illumination's Direct3D 12 frame, before its
  light passes. The sun pass and the resolve were in the same command list in every frame record
  checked (`frame-5289`, `frame-236525`), and that list already waits for the frame's signal; the
  sun pass's list is marked to wait as well, in case it is not. Until Direct3D 12 has traced a
  shadow at the render size, the sun pass keeps the game's shadows.
- **Diagnostics.** The cost table's
  measurement leaves them out first, so with the global illumination on that step is their own
  cost; the Direct3D 12 times are in the cost table, the log ("shadow rays", "shadow denoiser") and the scene
  status (Diagnostics, "Global illumination scene").
- **Shadow rays debug view and probe** (Diagnostics, Debug view "Ray-traced shadow rays"; needs
  the ray-traced shadows and an upscaler, as the upscaler's composite blends it over the image).
  `DebugShadows` traces each pixel's ray as the shadow rays do but towards the sun's centre, without
  chance, once as the shadow rays see the scene and once over the whole ray with cut-out geometry
  without its texture letting it through, and colours what blocks it: yellow cut-out geometry,
  lavender the game's models and characters ("Scene objects"), brown the terrain, olive cut-out geometry
  without its texture the rays let through inside the shadow map, magenta anything else only the whole
  ray finds; clear where the sun reaches. `ProbeShadows` does the same for the screen's centre pixel
  (the cross), with its global illumination rays, and the CPU adds the game's models whose bounds the
  ray crosses with each one's state and meshes. The probe is read back when the frame's slot comes round
  again, shown in the overlay with the denoised shadow, this frame's ray, the geometry pool and the
  structures waiting to be built, and logged ("Shadow probe:") whenever what blocks its ray
  changes, at most five times a second. When the terrain blocks it, the probe adds the hit triangle's
  highest corner as its ring was made, the heightmap's height there now and the level of detail resident
  there; every probe has its frame and the frames each terrain ring was last made. Checked offline on the
  four harness scenes, before the captured meshes went: each scene's shadow takes the expected colour.
  The probe also shows where the rays start: the scene's triangle they leave (or the depth's point), its
  normal and the sun's cosine to it and to the G-buffer's normal, which side they leave, the lift off it
  (Wächter and Binder's offset grows with the world coordinates: about 3 cm at 1,200 m) and whatever that
  lift passes through, and whether the blocker is the start's own triangle or mesh. Added 2026-10-24 when
  Snake's chest was shadowed in strips along its folds, each ray blocked at 0.00 m by his own suit's mesh
  from inside. Each model it lists shows its meshes' alpha and shadow bytes and the meshes left out.
- **Shadow ray starts debug view** (Diagnostics, Debug view "Shadow ray starts"; needs the ray-traced
  shadows and an upscaler, with the probe). `ShadowStartColor` colours where each pixel's shadow rays
  leave (`ShadowSurfaceAt`) against the depth's point and the G-buffer's normal: grey from the depth's
  point (no scene surface near), blue on a scene surface facing over 60 degrees away from the game's
  normal, red where the scene's surface lies nearer the eye than the game's, cyan where farther (clear
  up to 5 mm, full from 3 cm); clear on the game's surface. Built 2026-10-25 for the detached shelf
  shadow ("Ray-traced scene", "Characters").
- **Open: terrain blocking for one refresh** (2026-10-23, after the copy race was fixed). With the camera
  still, the probe showed a pixel's sun ray blocked by the terrain 140 m away (66 m above the pixel) for
  about 2.3 s, twice, lit otherwise: one ring made with a wrong height, the next refresh right again. The
  game's terrain vertex shader samples as `TerrainHeight` does, so either the game's page table and
  atlas disagree at the kick (streaming between the terrain draw and the copy) or a coarser level is
  resident there for a while. The probe's terrain fields tell the two apart.

Not handled:

- Lamps: they keep their shadow maps or none. Their draws come right after the kick, so a ray-traced
  lamp shadow would stall the game's GPU work for the trace. "Ray-traced lights" avoids that by drawing
  them again later.
- What the scene lacks: translucent meshes (alpha bit 6), the sky dome, and anything the game places
  outside its visibility managers, the object brush and the model list.
- Decals all cast as cut-out geometry until the game's own per-mesh pass masks are read (see "Scene
  objects", mesh flags): some decals the game never casts may shadow the surface under them.

## Ray-traced lights

With **Ray-traced lights** on, the game's point and spot lights (lamps, floodlights, the player's and
the soldiers' flashlights) cast shadows from rays traced through the ray-traced scene, in place of
their shadow maps or of none, and lights without specular get the game's. The scene runs for them on
its own if neither the light nor the sun's shadows run. Lamp contact shadows are off while it is on.

- **The game's lights** (24 pixel shaders, `SSLighting2_*` and `Lighting_*_NA`, point or spot, with or
  without a mask, specular and a shadow map; their checksums in `game_lights.hpp`). Two draws a light
  in the light pass: the light's volume marks the stencil (colour writes off), then lights where it
  marked (ONE, ONE into both light targets). Their vertex shaders are one and the same: the volume
  comes in clip space. Every one reads `cPSLight` (`b3`, the light passes' view space) alike: `c3` the
  light, `c4` its colour and size (which only widens the specular), `c5` the way a spot points, `c6`
  the falloff (see "Screen-space shadows": `SSLighting2_*` `1 / d^2 - d^2 c6.w`, `Lighting_*_NA`
  `(1 - d^16 c6.z) / d^2`, a point's distance plus `c6.x`), `c7.x` the cosine past which a spot's cone
  cuts its light off, `c8` the specular's scale, the diffuse's and the shadow's strength. A mask (`t4`)
  is projected by `cPSScene` `c16`, `c17` and `c19`.
- **Flashlights**, read from frame dumps at night (2026-10-23): `Lighting_Spot_NA_Mask_NSpec_NShdw`
  (`fd570742642f09e699d193ffb02bf0df`), a 13 degree cone (`c7.x` 0.976) reaching about 30 m, colour
  (2101, 2254, 1537), size 0, `c8` (0, 1, 0): no shadow and no specular. The shadowed spot lights in the
  same frames (`Lighting_Spot_NA`, a 4096 x 4096 shadow map each, in command lists of their own after
  their shadow maps') were the bases' floodlights (60 degrees, size 0.24 m).
- **Flashlight brightness** (`flashlight_brightness`, 1 to 16, 8 by default). The game's flashlight gives
  2254 at 1 m (green, before exposure), 22 at 10 m, against moonlight's direct light of about 4 (the
  global illumination's log, `sun`, times 1024), and cuts off at about 30 m: barely brighter than the
  moon at 20 m, where a real 1980s incandescent weapon light (about 1,000 to 2,000 cd) is many times
  full moonlight at 30 m. The game's night is brighter than a real one, so its units are no measure of a
  real light; 8x is about such a light against the game's night. A flashlight's colour is multiplied by
  the brightness and its falloff's `c6.z` divided by its 8th power, which moves the end of its reach
  (`c6.z^(-1/16)`) the brightness's square root further, where the brighter light falls to the same
  dimness. Set when the light is noted, so its redrawn light, its shadow rays and its bounce all have
  it. Only the look: the game's guards see as before.
- **Drawn with.** Each kind of light (point or spot, mask or not, `SSLighting2` or `NA`) is drawn again
  with the game's own shader for it with specular and without a shadow map, which reads `c8.x` and the
  specular tables at `t8` and `t9`; a light whose own shader has no specular gets a scale of 1, as the
  game's other lights have. A kind whose shader the game has not loaded keeps the game's draws.
- **Recording** (`local_lights.cpp`). A light's marking draw is made as the game makes it; its lighting
  draw is made without a pixel shader, so the stencil is left as the game leaves it, and noted:
  `cPSScene`, `cPSLight`, `cPSMaterial` (as `gi_inputs.cpp` keeps them), `t0` to `t10`, `s8` to `s11`,
  the light targets and the viewport. Both draws are taken before the lamp contact shadows see them.
- **Which lights.** At the kick, the lights of the lists recorded and not run yet whose `cPSScene` eye
  is the kick camera's (within a millimetre) go to Direct3D 12 in world space (`cPSScene` `c8` to `c10`
  take the light passes' view space there), up to 128, those reaching the most pixels on screen first,
  so that past 128 the smallest go without, the same ones frame after frame; those reaching none are
  never drawn again. (On 2026-10-23, with a bit a light, they were taken in the order of a hash map of
  command lists, which changes every frame: in a base at night with about 100 lights and 64 bits, a
  different 45 went without each frame and a spotlight flickered between its shadow and none.)
- **Soft shadows** (`TraceLocalLightShadows` in `src/shaders/gi_shadow.hlsl`, 2026-10-24). One value a
  pixel for all the lights together, the share of their light that reaches it: the ratio estimator of
  Heitz, Hill and McGuire ("Combining Analytic Direct Illumination and Stochastic Shadows", I3D 2018).
  Each light's diffuse light there without shadows (`LocalLightAt`, the game's own formula, with the
  G-buffer's normal) weighs it; **Ray quality** rays a pixel (1, 2, 4 or 8) go to the lights in
  proportion (systematic sampling), each to a point on a disc of the light's radius facing the pixel,
  leaving the surface as the sun's do (see "Ray-traced shadows", where a ray leaves; cut-out geometry by
  its alpha, cut-out geometry without its texture letting light through) and ending short of the light
  by its size or 10 cm, whichever is more, as it sits inside its lamp's model. The share is the rays
  that get through over those traced. The game gives many of its lights no size (`c4.w` 0: every
  flashlight, the hall's spots; the floodlights 0.24 m, most lamps indoors 0.42 to 0.45 m); those take a
  bulb's 3 cm, as do the lights the game shadows with a shadow map (2026-10-24): their shader uses the
  size only to widen the specular (`AdjustLightSizeFromDistance`, size over distance), and its shadow is
  a fixed four-sample filter of the map, sharp whatever the size.
  Every traced light's light times the share is each light's own shadow where lights do
  not overlap, and the right diffuse light in total where they do; specular takes the same share. Each
  blocked ray also gives the penumbra its blocker casts from the light, `2 r t / (d - t)`, in pixels.
- **Filter** (`FilterLocalLightShadows`). Three edge-avoiding a-trous passes (Dammertz et al., 2010),
  1, 2 and 4 pixels apart, a 3 x 3 B-spline weighted by how near the neighbours' depth (2% plus 1 cm)
  and normal (cosine to the 8th) are; a pass blurs a pixel where the widest penumbra among its
  neighbours on the same surface is at least its step, so contact shadows stay as sharp as the rays
  found them while wide penumbrae are smoothed. Until 2026-10-24 the gate was the pixel's own penumbra:
  a pixel inside a soft shadow whose rays all got through found no blocker, so no penumbra, and kept its
  full light amid its smoothed neighbours (19% of a wall's soft shadow in `frame-159975`: speckles new
  every frame). The rays' starts on the lights' total and on each light's disc come from interleaved
  gradient noise (Jimenez 2014) and the R2 sequence from there, so neighbours sample unlike points and a
  pixel's rays cover a disc evenly; they were white noise. No history: nothing trails behind moving
  casters. Before 2026-10-24 each light took one ray to its centre
  and a bit, and its hard edge showed every pixel's difference.
- **Bounce.** With the global illumination on, the same lights light the surfaces its rays hit
  (`SampledLocalLight` in `src/shaders/gi_light.hlsl`), first and later bounces, besides the sun and the
  sky: every light's diffuse light there without shadows, as the game's shaders work it out
  (`LocalLightAt`: the falloff, a point's least and added distance, a spot's cap and cone, the colour
  in the sun's units), picks one in proportion (weighted reservoir sampling, a candidate a light), and
  a shadow ray to it, ending short of the light as the shadow rays do, adds its light over the chance
  it was picked. Since 2026-10-24 (see "Global illumination") a ray's first hit is lit afresh every
  frame, so a flashlight's bounce follows the beam; only the light past a bounce comes from the hash
  grid's cells, which average the picks over frames. Before, the first hits read their light from the
  cells too, and a cell takes 1/16 of a frame's light: a flashlight's bounce took one to three seconds
  to go after the beam moved on (logged four times a second: the brightest lit hit fell from 1.03 to
  0.036 within a quarter second, what the probes read from the cache took 1.3 s and the final light
  3 s). Indoors the game's probe light, which the global illumination replaces, held the lamps' baked
  bounce; without this an indoor hall lit by 47 lamps was dark but for their direct light (2026-10-24).
- **Drawn again.** At the first list with a draw point, which waits for Direct3D 12, the immediate
  context draws the frame's lights before the list runs: each its volume as the game drew it (below), cut
  by a scissor to the light's sphere (a spot's: the bounding sphere of its cone) on screen, in the light
  vertex shaders' form (the clip-space position twice, then where it samples the light targets). On the
  volume's far faces the share (1 for a light not traced) goes into both light targets' alpha, 0 where
  the surface lies past them; on its near faces 0 where the surface lies short of them; then on the far
  faces its shader with specular, with the noted bindings and constants, adds its light scaled by that
  alpha (`DEST_ALPHA`, `ONE`), as the lamp contact shadows do. A light in a list run after it is drawn
  at once. A draw point is a pass after the game's lights and before anything
  reads them: the sun pass; indoors, where no sun shines, the sky's specular alone in its place
  (`SunLight_ES`, `367e8f6128befd48f2840ab3d6ed19b4`: `cPSLight` `c2.x` and `c3`, the sky reflection at
  `t15`; its list waits as the sun pass's does, `global_illumination::noteSkySpecular`); or the
  resolve, whose list also runs the skin filter, after the list starts. Outdoors the sun pass and the
  resolve share one list; some scenes draw no sun pass at all (three captures on 2026-10-24, lamps lit
  through everything), and only the resolve is there. Until then the sun pass was the only draw point:
  in such a scene the first frame's lights were lost and the game's drew from then on. Only after a
  frame with a draw point are the game's lights taken over, so menus and loading screens keep theirs.
- **Within the game's volume** (2026-10-24). The game lights only inside each light's volume, however
  far its falloff reaches: a box along the world's axes for a point light, for a spot its cone within
  one, as if each light were bounded to its room. In a capture of a room (`frame-17555`), 96% of two red
  lamps' 8 m spheres lay outside their 3.7 m high boxes, and a 100 m fill light's box lay wholly behind
  the camera, so the game drew none of it. Until then the lights were drawn again over all their reach
  and lit wherever no ray was stopped: a padded cell's upper walls in red, a lit wedge on a hall's
  floor. As a lighting draw is noted, its indices and vertices (at most 138 and 46 in the captures) are
  copied on its list (`CopySubresourceRegion`, into a 4 KB buffer used again once its light is drawn),
  and a vertex shader reads them back. Their faces face out clockwise, Direct3D's front faces, so the
  far faces are the back faces. A surface is inside where it lies short of the far faces and not short
  of the near ones (ties inside), the test the game's two stencil draws make; the pixel shaders compare
  against the light shaders' depth (`t2`, reversed). A volume that does not fit leaves its light drawn
  over its reach as before; the log counts those. A spot's volume is not always along the world's axes:
  the cells' moonlight has its cone within a box turned along the beam, which meets the floor only where
  the light through the vent falls.
- **Beams** (2026-10-24). A spot with a shadow map and a hard-edged cone (`c7.y`, 1 / its penumbra, at
  least 1000) stays the game's, drawn with its shadow map, and is not traced: the padded cells'
  moonlight through the vent (colour (16712, 23442, 41553), a 7 degree cone, `c7.y` 100000; every other
  light in 13 captures had at most 66). Its light stands 3 m beyond the wall and 5.5 m up; in a capture
  (`frame-125493`, the redraw's inputs saved, below) the share on the floor where the light through the
  vent falls was 0.02, the same as where the wall stops it: something past the vent that the game's
  shadow map leaves out blocked every ray. And at 20 to 40 times the room's lamps it drew nearly all of
  the floor's rays across its 3.5 m wide cone, so the share there stayed near 0 and the lamps' light
  went with it: a dark disc round a patch without slats, where the game draws the slats in a lit room.
- **Developer.** "Log global illumination often" logs the light and each step's largest value every
  few frames (as each readback returns), to see which step holds light after a light goes away.
  Every ten seconds the log counts the lights drawn with their shadow and without it
  (recorded after the kick, or not traced), those left at the kicks for another frame's camera or over
  the 128 bits, and those lost. The cost table times the drawing and the Direct3D 12 rays ("Ray-traced
  lights") and measures the whole by leaving it out; the log has the rays too ("shadow rays for lights"). The flashlights alone took 0.18 ms of rays at 2560 x 1440 in their first session,
  every one traced at the kick.

Not handled:

- Lights recorded after the kick are drawn without their shadow; the log tells how often.
- The rays know only each light's reach, not its volume (that is on the GPU alone): the bounce lights
  surfaces outside the volumes, a light whose volume is off screen can still take one of the 128, and a
  light weighs in the share where its volume leaves it undrawn.
- One share for all the lights: where a far brighter light is blocked, it draws most of a pixel's rays
  and the share falls for the lights that reach it too (the beams above, until they were left out).
- A light's specular takes the share its diffuse light gives; where lights overlap with different
  shadows, the specular's split between them is the diffuse light's.
- Leaves lit from behind (the game's translucency) weigh nothing, so where only that light falls the
  share is 1: no shadow.
- The game still draws the shadow maps of the lights that have them.
- A spot's mask (a flashlight's pattern) is left out of its bounce, which is a little brighter for it.
- The bounce comes with the ray-traced lights: with them off, the global illumination has the sun and
  the sky only.

## GTVBAO

With **Ambient occlusion (GTVBAO)** on, ground-truth visibility bitmask ambient occlusion replaces
the game's SSAO, whatever the game's SSAO setting, and darkens only the indirect light.
The occlusion is ported from three-gtvbao (norio, MIT), which follows SSRT3 (Therrien, Levesque and
Gilet, "Screen Space Indirect Lighting with Visibility Bitmask") with Mirko Salm's GT-VBAO
corrections; the noise and the denoiser are XeGTAO's (Intel, MIT). Off, the add-on only checks a
flag on each full-screen draw.

- **Recognition.** By checksum at `init_pipeline`: the sky light, the upscale, the ten resolves, the
  two SSAO passes (see "Ambient occlusion") and the two sun passes (`SunLight_DL_CS_ES`,
  `SunLight_DL_ES`). At the sky light's draw the add-on keeps the normals it binds for that deferred
  context. At a sun pass's draw it keeps the G-buffer's specular target (`t1`,
  `g_SpecularTexture`), whose `z` is the material. The sun comes after the upscale, so the march
  reads an earlier frame's binding: the same texture while the render size stays. `onDraw` calls
  GTVBAO before the sun shadows, which draw the sun pass themselves.
- **Depth prefilter** (`shaders::PrefilterDepths`, XeGTAO's depth MIPs), first: the view distance
  at the render size and four smaller copies in one `R32_FLOAT` texture, each 8x8 group covering
  16x16 pixels. Each copy is XeGTAO's `DepthMIPFilter` of the 2x2 below: the weighted mean, fading
  out distances more than `0.29 x radius` in front of the farthest, to zero at `0.75 x radius`. The
  sign holds the foliage material (negative), taken by weight at the smaller sizes; the sky is
  `1e6` m. The march reads each sample from the copy `round(log2(offset in pixels) - 3.3)`, so
  samples from 14 pixels away read smaller copies, at the full-size pixel's centre. Reading the
  `R32G8X24` depth and the specular target at full size, a close-up wall and floor (every pixel
  marching ~200 pixels) cost 3.3 ms at 2560 x 1440 against 1.3 ms in a scene with sky and distant
  buildings: the march was bound by cache misses.
- **Occlusion** (`shaders::March` with `LIGHT` 0), recorded at the upscale's draw on its deferred
  context, with the game's compute bindings saved and restored: one pass at the render size, from
  the upscale's depth, the kept normals and the game's `cPSScene` as bound, so `c21` and `c28`
  reconstruct positions as the engine's passes do, jitter included. A buffer whose `c7` is not the
  main view's perspective leaves everything unoccluded. Fox's view space looks down +z with x
  mirrored; the shader flips both into the usual view space the GT-VBAO maths assume. Two slices
  spread around the view vector and projected (GT-VBAO); 8 steps each way, spaced quadratically over
  the radius (1 m by default, at most 15% of the render height on screen, at least a pixel a step);
  32 sectors a slice, the horizons through the cosine-weighted CDF; occluders taken as
  `clamp(0.12 d, 0.05 m, radius)` deep, foliage as `clamp(0.02 d, 0.02 m, radius)`; samples
  farther than the radius ignored. Foliage is material 43 in the specular target (`z` x 255): grass,
  bushes and tree leaves, nothing else in two Afghanistan frames. Its `w`, the translucency, covers
  leaves and skin but not grass. As deep as solid occluders, a grass clump read as a solid block and
  its base went nearly black at night, when the sky light is most of the light. The visibility is
  raised to the strength. XeGTAO's Hilbert curve R2 noise is fixed per pixel, so nothing relies on accumulation
  over frames and the result is the same with DLSS off. The pass also writes XeGTAO's packed depth
  edges.
- **Denoise** (`shaders::Denoise`): XeGTAO's edge-aware 3x3 filter, two passes (beta 1.2 / 5, then
  1.2).
- **Apply.** The add-on draws the game's upscale, then draws it again with `shaders::Apply` in place
  of its pixel shader (the visibility in its `t0`) and a blend that multiplies the diffuse light's
  RGB by it, leaving alpha and the specular target alone; the game's shader, `t0` and blend state are
  restored.
- **Resolve.** Drawn by the add-on with a 1x1 white texture in `t13`, which is then restored. The
  game's `SSAOLineIntegrals` and `AmbientObscuranceSSAO` draws are skipped; their blur, upsample and
  combine passes still run, on cleared targets nothing reads.
- **Options** apply from the frame recorded after the next present, so all passes of a frame agree.
  An upscale without the normals, with sizes that differ or without `cPSScene` keeps the game's draw
  (no occlusion that frame) and logs the reason once. Without a specular target of the render size
  (the first frame, or after a resize) nothing counts as foliage.
- **Diagnostics.** "GTVBAO only" draws the resolve with `shaders::Show`: the visibility alone,
  linearised for the resolve's sRGB view.

Checked in game at 3840x2160 with DLAA and frame generation, in Afghanistan in daylight: all 14
shaders were identified at start-up and GTVBAO ran every frame. The diffuse light divided by the
game's own showed contact occlusion under the jeep, around Snake's legs, in the tyre treads, at the
base of bushes and in rock crevices, with the sky at 1, and the sunlit image kept its brightness.
The frame rate did not change in that CPU-limited scene; the GPU cost was not measured.

Not handled:

- The sky reflection (indirect specular) is added in the sun pass, so GTVBAO leaves it bright in
  creases the game's SSAO darkened.
- Point and spot lights are taken to add to the diffuse light after the upscale, which overwrites
  it; no night or interior scene was recorded.
- A terrain tile seam with a 1 cm step in the depth buffer gets a one-pixel line of occlusion at its
  base. It did not show in the final image at 1:1.
- Forward and transparent objects, drawn after the lighting, get no occlusion, as with the game's
  SSAO.

## SSILVB

With **Indirect lighting (SSILVB)** on, screen-space indirect lighting with visibility bitmask
(Therrien, Levesque and Gilet) adds one bounce of light between the surfaces on screen. It runs
GTVBAO's march (`shaders::March` with `LIGHT` 1) and gathers light as SSRT3 does (CDRIN, MIT). Off,
the add-on only checks a flag on each full-screen draw.

- **Where.** At the resolve, which already has the frame's finished diffuse light: the probes and
  sky (occluded by GTVBAO when it is on), the sun with its shadows, the local lights and the skin
  filter. The bounce is lit by this frame's light, so it needs no reprojection or history. The
  resolve and the sky light pass are in different command lists, so the march takes the normals
  any sky light pass bound last (the G-buffer's, one texture for the session).
- **March**, recorded at the resolve's draw on its deferred context, with the game's compute
  bindings saved and restored: from the resolve's depth (`t11`), diffuse light (`t9`), albedo
  (`t8`, its sRGB view, so linear) and `cPSScene`, with GTVBAO's slices, steps, thickness and noise
  and its own radius (2 m by default). It runs the depth prefilter first, with its radius, into the
  same texture GTVBAO uses. A sample lights the sectors it covers that no nearer sample
  covered, with the diffuse light its surface reflects (albedo x diffuse light) times the cosine
  between its normal and the direction to the receiver, which drops back faces (SSRT3). Equal
  sectors carry equal cosine weight (GT-VBAO's CDF), so the receiver's cosine needs no extra term
  and the sum is the irradiance over pi, the unit the resolve multiplies by the albedo. Specular
  light is left out, being view-dependent. At half the render size, rounded up (`Scale` 2): each
  texel marches from the top-left pixel of its 2 x 2, with the same steps in render pixels. At full
  size the march took 2.3 ms at 2560 x 1440, nearly all of SSILVB's cost, for a soft result. Into an
  `R11G11B10_FLOAT` target. A slice whose normal
  faces away from the camera (normal maps on surfaces seen edge on) drops out, in both effects: its
  negative weight could cancel the other slice's, and the light divided by the near-zero sum showed
  as white specks.
- **Denoise** (`shaders::Denoise` with `LIGHT` 1): XeGTAO's filter on the colour, two passes at
  the march's size, with depth edges the march wrote at that size (GTVBAO keeps its own).
- **Upsample** (`shaders::Upsample`): each render pixel blends the four nearest half-size texels
  bilinearly, each weighed by 1 / (relative depth difference + 0.001), so light stays on its side of
  an edge and sky texels drop out. It writes `diffuse + strength x light` into an
  `R16G16B16A16_FLOAT` copy of the diffuse light (alpha kept).
- **Resolve.** Drawn by the add-on with that copy in `t9` (and white in `t13` with GTVBAO on); the
  game's `t9` is restored. The game's occlusion, with GTVBAO off, multiplies the bounce like the
  rest of the light.
- **Options** apply from the frame after the next present. A resolve without normals, with sizes
  that differ or without `cPSScene` keeps the game's diffuse light and logs the reason once.
- **Diagnostics.** "SSILVB only" writes the light without the game's diffuse light and draws the
  resolve with `shaders::Show`: the bounce alone, as light before the albedo. Both views write the
  resolve's alpha, `sqrt(luminance / 32)`, which exposure, bloom and depth of field read from the
  scene target (`CopyReductionX4`, `DOF_ScatterBakeFirst`); with 1 there, they blurred the view.

Not handled:

- One bounce, from what is on screen: light from off-screen or hidden surfaces is missing, and the
  bounce fades towards the screen's edges.
- The probe lighting may already hold some bounce from static scenery, which SSILVB adds again
  beyond GTVBAO's radius.
- Forward and transparent objects neither give nor receive the bounce.

## Volumetric fog

The game's fog has no shadows, so it has no light shafts. The add-on keeps its formula and its
weather, and shadows the sun in it.

- **The game's fog.** `VolFog_TppVolFog` (the frame's first draw, 768 vertices, two triangles per
  slice) fills a 512x256 `R16G16B16A16_FLOAT` texture: 128 slices of view depth in 32 x 4 tiles of
  16x64 texels. Slice `s` holds the fog from the camera to view depth `2^(s / 127 / m_fogParam[1].x)`
  (1 m to about 46 km), rgb divided by `m_fogParam[1].y` and alpha the transmittance, both
  saturated. Its formula (Hoffman and Preetham's analytic scattering): optical depth
  `od = max(d - near, 0)^power` at the distance `d` from its camera (`cPSObject` `c9`), colour
  `(Mie + Rayleigh phase x sun + skyLum x betaU) / (betaM + betaR + betaU) x (1 - e^(-(betaM + betaR + betaU) od))`
  plus `(1 - alpha) x selfLum`, alpha `e^(-representativeDensity x od)`. `cPSMaterial`: `c0`
  representativeDensity, mieRatio, near, sunIntensity; `c1` skyLum, power; `c2` betaM, mieK (a
  Schlick phase); `c3` betaR, betaU; `c7` selfLum; `cPSObject` `c10` is towards the sun. A top-down
  rain cover (`t0`, sampled at the camera) scales mieK. No shadow map is read and the density does
  not change with height.
- **Readers.** 136 game shaders declare `g_tex_fog` (`t12`): the resolves, the sky, the clouds,
  glass, particles. All sample it with `uv = clip x (0.014648, 0.123047) + (0.015625, 0.125)` plus
  the slice's tile, normalised, so a larger texture in the same layout is read the same way.
- **Larger texture.** At `create_resource` the 512x256 `R16G16B16A16` render target becomes
  7680x544 (tiles of 240x136, a froxel about 16 pixels wide at 4K) with
  unordered access. At the fog pass the add-on draws the game's own
  draw with a viewport over the whole texture: its vertices place each slice's tile in clip space,
  and its froxel positions interpolate across the tile, so the game's fog fills the larger tiles
  exactly as the readers sample them. This runs with the volumetric fog on or off.
- **Shadowed fill.** With the volumetric fog on, the fog pass's `cPSSystem`, `cPSScene`,
  `cPSMaterial` and `cPSObject` are copied on the GPU in its command list, and the sun pass's
  (`SunLight_DL_CS_ES`, or `SunLight_DL_CS` in rain) `cPSScene` and `cPSLight` with its shadow
  atlas. Before the first resolve (`DR_VolFog_TppTonemap*`, `DR_TonemapRainFilter*`) three compute
  passes run on its context, one thread per froxel (240x136x128) in the first two.
  1. The sun's share in the froxel this frame: the mean of two comparisons spread through the slice
     in the cascade the sun pass would pick, faded to lit at the end of the shadow range as the sun
     pass fades. The comparisons move to a new place in the froxel each frame, the same along the
     whole view ray (the Halton sequence in bases 2 and 3 across the view, 5 through the slice, 64
     frames long).
  2. The share over frames: 95 % of the last one, reprojected through the last sun pass's `m_view`
     (view to world, the camera's position in `w`; so in the sun pass and the fog pass) from an `R16_UNORM` volume, held within the lowest and highest share of the
     3x3x3 froxels around it this frame (neighbourhood clamping, Karis 2014), so it follows a moving
     shadow at once and holds the share over the whole froxel where nothing moves: smooth shaft
     edges. Then what the slice adds to the game's colour,
     `(sun x visibility + ambient) / extinction x (e^(-extinction od(near)) - e^(-extinction od(far)))`,
     and its alpha. Without a shadow the slices add up to the game's formula.
  3. One thread per column adds the slices front to back into the game's texture.

  This follows Frostbite's volumetric fog (Hillaire, "Physically Based and Unified Volumetric
  Rendering in Frostbite", SIGGRAPH 2015: Halton jitter shared along the ray, 5 % of the current
  frame, history skipped outside the view, the same energy-conserving integration), with two
  comparisons per froxel and frame where it takes one, and the neighbourhood clamping it names as
  the fix for trails. The resolve's `t12` is unbound for the fill and bound again. The sun pass and
  the resolve are recorded in the same command list, so the fill takes the shadow map from its own
  context's sun pass; without one it has no shadow. (A frame counter by presents does not work: the
  game records the next frame while it presents the last.)

### Physical look

With **Look** set to Physical the fill builds Earth's atmosphere as Hillaire's reference for "A
Scalable and Production Ready Sky and Atmosphere Rendering Technique" (Epic Games, 2020, MIT; the
same as Unreal Engine's sky atmosphere) does, and draws the sky from it. In its frame z is up and the
planet's centre is the origin, in km; the game's world has y up in metres.

- **Tables.** Once: transmittance to the atmosphere's top (256x64) and multiple scattering (32x32),
  with Earth's Rayleigh, Mie and ozone as the reference sets them. Each fill: the sky parameters (one
  thread, `R32G32B32A32_FLOAT` 6x1), the sky-view table (192x108) for the camera's height and the sun,
  and the sky's mean light over the upper half (64 directions).
- **Sunlight.** The sun pass's `cPSLight` `c0` (towards the sun, view space) through `m_view` gives
  the sun in the world; `c1` times `m_exposure.z` is the game's sunlight at the camera (in Afghanistan
  at noon `c1` about 25000, 22000, 17000 and `m_exposure.z` 7.1e-5: about 1.6). The game lights a
  surface with albedo times that, without the 1 / pi of a physical diffuse surface. The game's sky
  is lit by a sun of its own, about four times brighter: the sky draws' `cPSMaterial` `c3.w` times
  `m_exposure.z`, at most 1000, which multiplies its scattering table as Hillaire's multiplies his.
  Physical takes that, white, as the sunlight above the air, from a GPU copy of the last
  sky draw's `cPSMaterial` (the sky in a weather with the sun's sprite: `Sky_Draw_TppBaked`,
  `Sky_Draw_Tpp`). It takes the sun's direction from there too, `c3.xyz` in the world: the sun pass
  lights the ground with the sun at least about 26 degrees up (`y` 0.436), while the sky's sun sets.
  The game raises `c3.w` as the sun sets (157080 at noon, 283554 one degree up) to make up for the
  longer way through the air, so with the sun pass's direction the evening sky came out about five
  times brighter than at noon and at the HDR peak. The fog is lit as the game lights the ground: its
  phase and shadow take the sun pass's sun (with the sky's, a low sun's glow showed on the sunlit
  ground in front of the hills that hid it), its light the sky's sun through the air, which the
  planet hides below the horizon. After a sky draw
  without the sun (`Sky_Draw_TppNoSky`) or before any, it takes the ground's: pi times the game's
  sunlight over the air's transmittance to the sun pass's sun at the camera (luminance); the game's
  sky came out about five times brighter than that (75 to 125 nits against 15 to 30 with HDR, paper
  white 203). The camera's height is `m_eyepos.y`. Every ten seconds of the physical look the sky's
  values go to the log ("Physical sky: ..."), read back a frame late at present.
- **Fog.** Per froxel: the air at its height, lit by the sun through the air above it (transmittance
  table) and the air's multiple scattering, and a height fog: albedo 0.9 tinted by the fog pass's
  `betaM`, Cornette-Shanks phase with the `g` the fog pass's Schlick `mieK` stands for (`k = 1.55 g -
  0.55 g^3`: 0.96 is 0.8; taken as `g` it drew the low sun's glow as a disc over the hills), lit by the sun and the mean light from
  all around (the sky above; below, the ground at albedo 0.3, lit by the sun and the sky; the
  multiple scattering table also takes that ground), falling off with height over 300 m (up to 1 km with Haze; at most 4x
  below the camera). Its extinction at the camera comes from the game's fog over a kilometre,
  `e = representativeDensity x od(1 km) / 1000` (about 1.5e-3 / m on a clear day in Afghanistan:
  `representativeDensity` 0.003, `power` 0.9, `near` 0): `e x min(e / 0.015, 1)` less the air's,
  a tenth of it on a clear day (about 26 km visibility) up to all of it in dense weather. As a
  medium the game's own density greys the whole sky: the game uses it to fade distant ground, and
  its sky leaves most of it out. **Haze** (`atmosphere_haze`, 0 to 1, the `Fog` constants' `Haze`)
  raises that share towards all of it in any weather and deepens the layer from 300 m to 1 km, as
  deep as dust: at 1 the distance fades about as the game's does (on a thin-fog day in Afghanistan
  6.3e-4 / m, about 6 km of view against the game's 8; at 0 about 120 km), and the sky pales with it. The height fog
  also dims the sunlight that reaches the air and itself: `e^(-density x scale height / sin(elevation))`,
  the sine at least 0.01 (a 1 km layer is about 100 km long at the horizon); without it, 100 % haze at
  sunset glowed at the display's peak towards the sun (frame 8938: 700 nits). Both shadowed and integrated over the slice as in the game's look; the slices are added
  front to back with their transmittance. The last slice holds the height fog alone, from a second
  volume: the sky and the game's clouds read it, and the sky's light already holds the air.
- **Overcast.** The game greys its sky by weather: `Sky_Draw_TppBaked` and `Sky_Draw_Tpp` swap
  their Rayleigh light for a flat "cloudy" term by `cPSObject` `c10.x` (0 on a clear day, 1 in
  rain; in rain also `betaR` grey 0.04, Mie `g` 0.08, and the fog's furthest slice laid over most of
  the sky: `influenceOfFogVar` 1 against 0.3). Its rain sky is a flat grey of about 0.18 (40 to 60
  nits with HDR at dusk), where the physical one stayed a clear, darker blue. Physical takes that
  share from a GPU copy of the last sprite sky draw's `cPSObject` (0 after `Sky_Draw_TppNoSky`) as a
  cloud deck: it takes that share of the sun from the sky, the air and the fog, its light replaces
  it in the sky, the fog's mean light and the air's, the moon fades behind it, and the height fog
  takes all of the game's density (rain and mist, not haze the air already makes). The deck's light
  is what the game's fog adds up to far away, `(sunIntensity x mean phase x betaM + skyLuminance x
  betaU) / (betaM + betaR + betaU) + selfLuminance` with the mean phase `(1 - mieRatio) / 4 pi +
  mieRatio`: the game's own light for that weather (frame-59279, rain at dusk: 0.175, 0.184, 0.196).
  The fill reads the last frame's sky draw, a frame late.
- **Sky.** `Sky_Draw_TppBaked`, `Sky_Draw_Tpp` (with the sun's or moon's sprite) and
  `Sky_Draw_TppNoSky` (fog and sandstorms) draw with the add-on's pixel shader and their own vertex
  shader (all three pass the world-space view ray in `TEXCOORD0`, the sprite's place in `TEXCOORD1`
  and colour in `TEXCOORD2`): the sky-view table times the sunlight (below the horizon, its horizon
  row, as the game's sky does: the sea at Mother Base ends short of the planet's horizon, and the
  dark air in front of the planet showed between them as a grey band), the game's sprite over it, the
  last slice's fog in front, the game's dither (`t8`), then the game's TppTonemap (`cPSMaterial`
  `c1`) and sRGB encoding, or the encoding alone with HDR, which takes TppTonemap out of the scene.
  Its two textures go to `t6` and `t7` for the draw; the game's are bound again after it.
- Checked on WARP with made-up constants: blue sky with a pale horizon at noon, orange towards the sun
  at 1 degree, the pink band and the Earth's shadow at -2 degrees.

## Depth of field

How Fox Engine blurs, read from the shaders' debug names and the camera plugin's code. Every frame
with the game's Depth of Field option on runs the `DOF_Scatter*` passes; `DOF_0_*` and
`DOF_OnlyNear_*` are in the shader set, but no recorded frame ran them.

- **Camera.** The depth of field plugin (`0x1DD1F0`) reads the render view's `GrCamera`
  (`[view + 0x570]`): focal length `+0x110` (millimetres), f-number `+0x124`, focus distance
  `+0x108`. f/0 and f/100 and up skip it (no passes that frame); unless the camera allows any
  aperture, the f-number is raised to a minimum for the focal length. It hands its renderer the
  thin-lens blur radius `0.5 x 53333.3 x f^2 / (N (focus - f))` per unit of `(z - focus) / z`:
  53333.3 is 1920 / 0.036, so a 36 mm-wide sensor in pixels of a 1920-wide image.
- **`DOF_ScatterBakeFirst`** (half size, three targets) reads the scene target at four points per
  pixel through an sRGB decode, raises bright pixels to the HDR luminance in alpha (`a^2 x 32`,
  capped at `cPSObject` c11.y), and takes each pixel's blur radius from depth as
  `c12.z x (z - c12.w) / z` (`ReconstructViewZ`), clamped to c11.w in front and c11.z behind. Near
  and far split at device depth c11.x.
- **Scatter.** `DOF_ScatterBakeSecond` makes 1/4, 1/8 and 1/16 size levels. `DOF_ScatterSprite`
  (indexed, 2048 quads a draw) draws a quad per pixel of a level, sized by its blur, shaped by a
  64x64 bokeh texture and divided by its area, with additive blending; big blurs go to the small
  levels. Near, then far. Small blurs are gathered with a few taps (`DOF_ScatterGather1`,
  `CompositeNear`, `CompositeFar`).
- **`DOF_ScatterCompositeFinal`** writes `blur + (1 - coverage) x scene` from the half-size result
  and a copy of the scene (`CopyRenderBuffer`) into the scene target, sRGB-encoded, alpha kept.
- Weak points: everything is half size or less, so slight blur turns blocky and shimmers at focus
  edges; one depth splits near from far; the HDR lights are not tonemapped back, so they clip.

The add-on draws the game's depth of field as it is, before DLSS (see "What the add-on changes").
Photo mode blurs with it: it sets the active camera's focus distance (`+0x38`), f-number (`+0x44`,
the overlay's **f-stop**, or f/64 with the blur off) and the byte that allows any aperture
(`+0xEA`) for the selector's update, which copies them into `GrCamera` (see "Photo mode").

## Draw distance

How far the game draws and loads its world, measured 2026-10-23 in Afghanistan from the outside
(`ReadProcessMemory`, and Frida hooks for the limits; scripts in `%TEMP%\mgsv-lod`).

- **Streaming.** A location's `StageBlockControllerData` entity (vtable `0x20E2F90`; `afgh` in
  Afghanistan) loads the world in square windows of blocks around the camera: full detail in 5 x 5
  blocks of 128 m (`blockSizeX`, `blockSizeZ` at `+0xAC`, `+0xB0`; `countX`, `countZ` at `+0xB4`,
  `+0xB8`; 577,746 bytes a block at `+0xC4`), and two coarser copies of the world, `small_lod0` and
  `small_lod1` (`/Assets/tpp/pack/environ/stagelow/afgh/`), in 5 x 5 blocks of 2 and 4 full-detail
  blocks (sizes and counts as bytes at `+0x134` to `+0x137` and `+0x150` to `+0x153`). One large
  block, a base, loads at a time (`largeBlockCount0` at `+0xE8`). A window of n blocks of size s ends
  between (n - 1) / 2 and (n + 1) / 2 times s from the camera, by where the camera is in its block: full
  detail 256 to 384 m, `small_lod0` 512 to 768 m, `small_lod1` 1 to 1.5 km. Past them only the
  terrain.
- **What full detail holds.** The props, the object brush's vegetation (exactly 25 `ObjectBrushBlock`s
  loaded, all within 412 m) and the `StaticModelArray`s, with their collision. The coarser copies'
  content is not in the visibility managers; it is not identified yet.
- **The setup.** `0x165BD0` runs when a location loads (Continue from the title screen; restarting
  from a checkpoint did not run it). It copies the entity's fields into a description on its stack and
  passes it to `0x15F010` (nothing else calls it), which builds the controller (constructor `0x15CCC0`). In the
  description: full detail's block size at `+0x64`, `+0x68` and count at `+0xB0`, `+0xB4`;
  `small_lod0`'s at `+0x16C`, `+0x170` and `+0x174`, `+0x178`; `small_lod1`'s at `+0x1E4`, `+0x1E8` and
  `+0x1EC`, `+0x1F0`; all 32-bit.
- **Limits.** Widened by a hook on the setup: 7 x 7 for all three loads and plays (49 brush blocks; the
  ray-traced scene held 66,000 instances in 8% of its geometry pool), and so does full detail at 7 x 7
  with the coarser copies at 9 x 9. 9 x 9 for all three crashes while loading, in a `StaticModelArray`'s
  set-up (`0x44429F`, `0x4458A7`, `0x1B935B3`, `0x1B971BC`, `0x1B94816`): `0x1BCCF50` reads a null entry
  of a fixed array of 168 pointers (`+0x9C0`, the count at `+0xF00`) in the object `[0x2C79710]`
  (vtable `0x24F01D8`). `0x1BD0070` copies a list into that array with no bounds check. What the array
  holds, and what fills it while loading, is not decoded: no such copy ran during a 7 x 7 load.
- **Level of detail.** A render view's level of detail factor (`+0x608`, set at `0x1B87B0`) is a
  resolution x the camera's cot(fov / 2) (`+0x10C`) / 200, times `GrTools.SetLodScale` (view `+0x618`;
  `SetVertexLodScale` `+0x61C`, `SetModelLodScale` `+0x620`, `SetModelVertexLodScale` `+0x624`; all 1
  in the game). The resolution is the global `0x2B73434` (`SetLodFactorResolution`) or, at 0 (the
  game's), the view's width (`+0x5D8`): the render width, so upscaling lowers the detail. A LodScale of
  4 changed 254 pixels of a 4K view: the level of detail is not what pops in.
- **Cull lengths.** `Fox/Scripts/Gr/gr_init_dx11.lua` sets a `RejectionLengthBias` for each graphics
  quality, for `PluginModel` (Extra High 128) and `PluginClone`, the vegetation (250). Each is a static
  object (`0x2B74330`, vtable `0x20F4DF8`; `0x2B77FB0`, vtable `0x20FFA20`): the quality names' values
  in a map at `+0x18`, the selected value at `+0x60`, the applied one at `+0x64`, a pending change at
  `+0x68` (`0x205AF0` selects, `0x1C8500` applies or reverts). The draw jobs copy `+0x64` each frame
  (models at `0x1CA8E8`, `0x1CBC0F`, `0x1CD2AF`; vegetation at `0x206A63`, `0x206D46`, `0x206EF3`). How
  the length enters the cull is not decoded; the objects' `drawRejectionLevel`s name sizes (0.5 to
  32 m for models, 400 to 1 pixels for vegetation).
- **What the add-on does** (`src/draw_distance.cpp`). Vanilla changes nothing. Extended hooks
  `0x15F010` and sets the counts to 7, 7 and 7 where the description has the open world's layout (block
  sizes 128, 2 and 4, counts 5); at each game frame start it sets the resolution to 8192 and the
  applied lengths to 1024 and 2048. Far does the same with 7, 9 and 9. The game skips a selection whose
  value equals the applied one, so with the applied lengths overwritten it would never record its own:
  a hook on `0x205AF0` gives the selection the game's value to compare with, and a change it leaves
  pending runs on the game's values until applied or reverted, then counts as the game's. Back to
  Vanilla, the game's resolution and lengths return at once and its windows at the next location load.
  Untested: Central Africa, Mother Base (its own controller class), the cost in frame time.

## Photo mode

F2 freezes the game and flies a free camera through it; F3 hides the UI at any time. Traced
statically and with Frida in game: hardware write watchpoints on the scene constants' `m_eyepos`
and then on `GrCamera`'s position led to the camera selector.

- **Camera.** The scene constants come from the render view's `GrCamera` (`[view + 0x570]`, where
  the motion blur plugin also reads the shutter). The main camera selector's update (`0x437B60`,
  from `0x438470` on the game thread every frame) fills it. The selector (`CameraSelector`, main
  instance at `[0x2BA1668]`) keeps its cameras at `+0x90`, the count at `+0x88`; the first with its
  byte `+0xE0` set is the active one. From the active camera's rotation (`+0x120`, a quaternion
  x, y, z, w) and position (`+0x130`) the update builds `GrCamera`'s world matrix (`+0x30`; rows
  local x, up, forward, position) and view matrix (`+0x70`, the previous one moved to `+0xB0`),
  copies the lens (focal length `+0x3C`, near and far `+0x48` and `+0x4C`, shutter `+0x50`) and
  places the audio listener. Culling, lighting and the scene constants follow `GrCamera`.
- **Axes.** Fox Engine is right-handed with y up. A camera looks along its local +z, the quaternion
  turning x, y and z into `GrCamera`'s rows, so screen right is -x when looking along +z.
- **Pause.** The pause manager (the global `0x2B65D40`, vtable `0x20C56C0`) holds up to 16 requests,
  each a dword of pause bits with bit 31 set while active; their name hashes are at `+0x58`. Create
  (slot 3, `0xF1C80`) takes (manager, bits, name hash, name) and returns a pointer to the request,
  inactive. The game sets and clears bit 31 itself (`or` at `0x5395D1`, `and` at `0x5391D9`);
  release is slot 1 (`0xF1D10`, virtualised). Each frame start (`0x7F840`, through slot 2) ORs the
  active requests into `[manager + 8]`. Game time (`0x816A0`, on the time object
  `[0x2B641C8] + 0x228`) steps by zero while any bit is set, and 107 call sites test bits of their
  own. One session had eight requests, most of them `0x7FFFFFFE`: every bit but bit 0.
- **Weather.** The `WeatherManager` update (`0x537DD0`, on the game thread) returns early under bit 8
  (test at `0x537FBF`): the sky then keeps its weather and time, and the weather menu's changes
  never show. Bit 9 only stops the clock (and is passed on to the requests' update). Besides it,
  only `0x4939A0` tests bit 8 directly; jobs test masks from data (`0xDAF60`).
- **Input.** The game registers raw mouse and keyboard input without `RIDEV_NOLEGACY`. Its window's
  `WM_INPUT` handler (`0x53ADF0`) keeps the cursor centred and calls `0x1A45540`, which reads the
  `RAWINPUT` and hands it to the mouse (`0x1A45860`) or the keyboard (`0x1A45790`, virtualised).

What `src/photo_mode.cpp` does. It checks the first bytes of the selector update
(`48 8B C4 55 41 54 41 55 41 56 41 57 48 8D 68 A1`), the raw input reader
(`48 89 5C 24 08 48 89 74 24 20 57 48 81 EC 50 02 00 00`) and Create
(`40 56 57 41 56 48 83 EC 30`), and the manager's vtable. If any check fails, nothing is hooked and
F2 does nothing.

- **Start.** F2 (ReShade's key state at `reshade_present`) asks for photo mode. At the next selector
  update, the free camera takes the active camera's position, yaw and pitch (the roll is dropped),
  and the add-on's pause request turns active. The request is made once per session with
  `0x7FFFFFFE` and kept, so repeated use does not fill the game's 16. Without an active camera
  (loading screens) the request lapses.
- **Flight.** Each selector update, the mouse turns the camera (0.001 rad per raw count; pitch within
  ±1.55 rad). W and S move along the view, A and D along the horizontal right, Space and Ctrl along
  +y: 5 m/s, 25 m/s with Shift, by real time (`QueryPerformanceCounter`, at most 0.1 s a step). The
  active camera's rotation and position are replaced for the original update and put back after it,
  so the game's camera keeps its own.
- **Input.** In photo mode the raw input reader takes the mouse's movement for the free camera and
  gives the game only button and key releases, so nothing held when photo mode started stays held
  after it. The keys come from ReShade, and not while its overlay is open.
- **UI.** F3 or photo mode hides the UI: draws with a `Draw2D` vertex shader bound (see "The 2D
  pass") into a `B8G8R8A8_TYPELESS` target at the render size, the scene target, are skipped. The
  lens flares draw into the half-size layer and stay.
- **Weather.** While its request is active, photo mode turns the weather update's `je` at
  `0x537FC8` (`74 0C`) into `jmp` (`EB 0C`), so the update ignores bit 8 and the weather menu works
  with the game frozen; bit 9 still stops the clock. It checks
  `8B 48 08 C1 E9 08 F6 C1 01 74 0C` at `0x537FBF` first; without a match photo mode works and the
  weather menu changes nothing in it.
- **Unload.** `AddonUninit` clears the pause request's active bit before the hooks go, and puts the
  weather update's `je` back.

Checked in game with a Frida prototype, before the add-on code: with the request active, the scaled
frame time stayed 0 while the selector update ran every frame (124 calls in 2 s), and the active
camera raised 3 m for the update rendered the raised view, culled correctly.

## Known limitations of the tools

- **RenderDoc 1.46** captures nothing: with the device check passed (patched in memory through
  Frida during `--opt-delay-for-debugger`), the game stays on frame 1 at about 0.4 FPS. Thread
  stacks show every game thread waiting and none inside `renderdoc.dll`; ReShade with the same
  patch runs normally. Without the patch RenderDoc hits the device check like ReShade. Not pursued;
  the frame log and dumps replace it.
- **Frida must attach at spawn.** Attaching to the running game and scanning its memory crashed it
  in `frida-agent.dll` (`0xC0000409`). Spawning with the game folder as working directory works.
