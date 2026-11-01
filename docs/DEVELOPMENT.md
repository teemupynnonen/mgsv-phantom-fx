# Phantom FX development

## Build

Requires Visual Studio 2022 (or Build Tools) with the C++ workload and CMake 3.25 or later.
From a x64 developer prompt:

```
cmake --preset release
cmake --build --preset release
```

CMake downloads MinHook, the DLSS SDK v310.9.1 headers, loader, `nvngx_dlss.dll` and `nvngx_dlssd.dll`, and the
ReShade 6.8.0 add-on headers with the Dear ImGui header, each pinned by commit and checked by
SHA-256. Frame generation needs the Streamline SDK 2.14.1, which only its release archive carries:
the first configure downloads it once (about 280 MB, checked by SHA-256) and keeps the headers,
license and x64 runtime files. FSR needs the AMD FSR SDK 2.3.0's API headers and AMD's signed
Direct3D 12 DLLs, pinned by commit and checked by SHA-256 like the rest (about 66 MB). The add-on is
written to `build/release/PhantomFX.addon64` together with `nvngx_dlss.dll`, `nvngx_dlssd.dll`, Streamline's `sl.*.dll`
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
  and its CPU time a frame, with the time of every draw hook (`gi_inputs`' `HookTimer`) and of the
  constant capture's unmaps, and the reads the capture refused for a write of another size (the first
  also logged with its sizes);
- the ray-traced lights' "Lights in ten seconds";
- the physical sky's parameters, copied back from the GPU every ten seconds;
- the upscaler's frame counts every 1800 presents;
- each module's expired notes (see "Command lists" under "What the add-on changes"), in its status
  line, and the global illumination's kicks from a camera smaller than the depth it packs (see
  "Smaller views");
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

This runs eleven tests that need neither the game nor a GPU. `tests/upscaler_tests.cpp`: settings
(defaults, round trip, clamping, invalid files and values), the mode maths (render scales, texture LOD
bias), the render size plan (DLSS's own size or the nominal one, rounded up to even and never above the
output size, Custom's DLSS quality, the settings that make a new plan), the frames (when one starts,
the frame a pass belongs to, the main view, which uploads get the jitter and after which frames) and
the camera maths (main-view recognition, jitter direction and units, reprojection, camera cuts, jitter
phases and the Halton sequence, the camera frame generation gets). The main view and a sun pass from
frame logs, pasted in as the log printed them, check main-view recognition and a light pass's view space
on the game's own numbers. The constant capture runs on a WARP device: a thread reads only its own
writes and only at their size, and each effect keeps only what it reads.
`tests/scene_description_tests.cpp`: what the ray-traced scene takes of a model file, from small FMDL
files it writes: the mesh roles of each case measured in the game (a tarp's stand-in, a pylon's
lattice, glass, sky domes, inner walls, decals, hair, bushes), what each role gives its rays, a
cut-out mesh's texture, unreadable files and skins. `tests/scene_contents_tests.cpp`: the scene
contents over a fake scene memory with room limits: when models, variants, characters and textures are
let go, waiting for room, a variant without room (never traced, as today), texture copies arriving and
replacing one another, characters built, refitted and made again, the palette budget, frames without a
newer list, the instance cap's order and the terrain rings' remaking. `tests/light_choice_tests.cpp`:
which lights a kick gives the rays: the most pixels first, then the nearest, the light cap's edge, the
same choice whatever order the lights come in, a
light drawn from another eye left and counted, one reaching no pixel left out; a light's reach (around
the camera, behind it, a narrow spot's, the viewport's corner); a light as the rays take it (its range,
its rays' radius, where its shadow map starts) and its key from two cameras. `tests/game_shaders_tests.cpp`:
the game's shaders on a WARP device, each created from a DXBC header carrying a row's checksum: a sun
shader known by its checksum and drawn with it, unknown and missing shaders, every copy of one shader
known, code that is not DXBC, the first of a row's shaders kept (the ray-traced lights' twins),
everything let go at device teardown, and vertex shaders by their source name.
`tests/shader_bytecode_tests.cpp`: the dither patch on a WARP device, on shaders compiled as the game's
are: a dither centred on zero and one added whole, each flat at the dithered image's average once
patched, and the G-buffer's fade mask kept.
`tests/command_list_notes_tests.cpp`: how a module's notes go from a deferred context to its command list
and are handed over: once, a context starting empty after its finish or at a gone one's address, a list
run inside a deferred context joining its notes in the order they run, a forgotten list, notes expiring
at the fourth present, the next frame recorded during a present, and the lists waiting at the kick.
`tests/kick_traces_tests.cpp`: what a kick traces, from settings through the effect plan and what the kick
finds: path tracing keeping the rays of what it replaces, waiting for Ray Reconstruction, the lamps waiting
for the game's tables, a kick without a sun or lights, a cost measurement leaving the light out, the scene
view and the glass's targets. `tests/shared_textures_tests.cpp`: a shared output on a WARP device: nothing to
read until written, the same size keeping what it holds, a new size or a replaced texture leaving it unwritten.
`tests/d3d12_host_tests.cpp`: Direct3D 12 work at a point of the immediate context, on WARP devices: each side reads what
the other just wrote, frame after frame as the lists come round, and a failed evaluation leaves the next one fine.
`tests/kernel_table_tests.cpp`: what the build makes of the compiled kernels: each kind of binding its own type,
what the root signature binds left out, a slot with two names, a name with two shapes and a kernel without
bindings failing.
Everything else is checked in the game.

## Package a release

From the repository root, after building and testing:

```
cpack --config build/release/CPackConfig.cmake -B build/release
```

This produces `build/release/PhantomFX-<version>.zip` and its `.sha256` checksum:

| Path in the ZIP | What |
| --- | --- |
| `PhantomFX.addon64`, `nvngx_dlss.dll`, `nvngx_dlssd.dll` | The add-on and the DLSS and Ray Reconstruction models, beside `mgsvtpp.exe` |
| `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `nvngx_dlssg.dll` | NVIDIA Streamline and the frame generation model |
| `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`, `amd_fidelityfx_framegeneration_dx12.dll` | AMD FSR upscaling and frame generation |
| `PhantomFX/README.md`, `CHANGELOG.md`, `LICENSE` | Player documentation and the add-on's license |
| `PhantomFX/*-LICENSE.txt` | NVIDIA DLSS, NVIDIA Streamline, NVIDIA NRD (global illumination's denoiser), AMD FSR SDK, MinHook, ReShade, Dear ImGui, AMD FidelityFX (RCAS), Bend Studio (screen-space shadows), three-gtvbao and Intel XeGTAO (GTVBAO), AMD Capsaicin (global illumination) |

CPack packages only the `addon` component, so SDK headers, static libraries, tests, debug symbols
and local settings are left out. ReShade is installed separately by the player.

## Sources

The code is laid out by what it does for the player:

- `src/features/`: one folder per feature, with its settings and what draws it. A pull request for
  one feature stays in its folder.
- `src/passes/`: the game's passes that several features change, and what they share there. A
  feature plugs into a pass; the pass composes the features' parts into the game's draw.
- `src/`: the add-on itself (the core, the window, the cost measurement) and what everything shares
  (the game's code, the camera, Direct3D helpers, the log).

### The add-on

| File | Purpose |
| --- | --- |
| `src/addon.cpp` | ReShade add-on: exports, events, build line, MinHook set-up, NGX's wrapped context |
| `src/core.cpp`, `src/core.hpp` | The settings in force: loaded and saved (`PhantomFX.ini`), the diagnostics, the enhancements switch (F1), the effects a cost measurement leaves out; every change goes to every module as a `Configuration` (`configureModules`) |
| `src/modules.cpp`, `src/modules.hpp` | The feature and pass modules, one row each: what each does as the game's device comes and goes, at each change of the settings and at each present |
| `src/phantom_fx_settings.cpp`, `src/phantom_fx_settings.hpp` | Phantom FX's settings, its features' together (`forEachFeature`), loaded and saved by walking their descriptions; the diagnostics, the effect plan (which lighting effects run, and what replaces what) and `Configuration`, what each module configures itself from |
| `src/effects.hpp` | The lighting effects, in the cost measurement's order, and how one the plan runs is doing (`Health`) |
| `src/features/setting_visitor.hpp` | What a feature's description says: its item, its settings' keys, ranges and controls |
| `src/menu.cpp`, `src/menu.hpp` | The "Phantom FX" overlay window: its pages, each feature's item drawn from its description, and what the running game adds to it (`statusOf`) |
| `src/menu_budget.cpp`, `src/menu_budget.hpp` | The window's sections, and each effect as a budget line: its state, its switch and its timed passes |
| `src/fxui/` | fxui, the reusable frame budget settings window for ReShade add-ons (see its README.md) |
| `src/performance.cpp`, `src/performance.hpp` | The passes' GPU costs: timestamp queries, the averages, the real cost measurement, `PhantomFX-performance.txt` (see "Costs") |
| `src/performance_menu.cpp` | The budget page, the costs over the game, and the costs as text |

### Features

Each folder holds the feature's `settings.cpp` and `settings.hpp` (its settings and `describe()`), and
what draws it, when that is the feature's own.

| Folder | What draws it |
| --- | --- |
| `upscaling/` | `upscaler.*`: DLSS or FSR before the grading, in place of FXAA: NGX, composite, the grading at the output size, texture LOD bias. `frames.*`: the frames the upscaler runs on: when one starts, its main view and its jitter. `render_size_plan.*`: the render size plan, from the upscaler, mode and output size, with DLSS's own size from NGX. `d3d12_host.*`: Direct3D 12 work at a point of the immediate context, for FSR and Ray Reconstruction (see "FSR upscaling"). `upscaler_shaders.hpp`: the upscaler's inputs and motion vectors, composite with RCAS and debug views, stretch fallback. `fsr_upscaler.*`: FSR upscaling on Direct3D 12. `ray_reconstruction.*`: DLSS Ray Reconstruction on Direct3D 12 (see "Ray Reconstruction"). `fxaa.*`: hook on the FXAA plugin, the FXAA pass at any post processing setting |
| `frame_generation/` | `presenter.*`: the game's stand-in swap chain, presented through Direct3D 12. `frame_generation.*`: DLSS Frame Generation, Streamline, its inputs and camera, Reflex. `fsr_frame_generation.*`: AMD's swap chain and its inputs |
| `sun/` | Settings only: the sun size the soft and ray-traced shadows and the global illumination share |
| `soft_shadows/` | Settings only: a mode of the sun light pass's shaders (`passes/sun_light_pass.*`) |
| `sun_contact_shadows/` | `sun_contact_shadows.*`: the sun's screen-space shadow, traced once per depth a command list records |
| `lamp_contact_shadows/` | `lamp_contact_shadows.*`: a shadow-mapped point or spot light's screen-space shadow, drawn into the light targets' alpha between its two draws. `lamp_contact_shadows_shaders.hpp`: that draw |
| `lamp_soft_shadows/` | `lamp_soft_shadows.*`: the replacement shaders a point or spot light with a shadow map lights with, and their options. `lamp_soft_shadows_shaders.hpp`: the game's `SSLighting2_Point` and `SSLighting2_Spot` rebuilt, with the light's shadow map filtered by contact-hardening soft shadows |
| `contact_shadow_shape/` | Settings only: the contrast, reach, thickness and fade of the contact shadow trace (`passes/contact_shadow_trace.*`), which the sun's and the lamps' share |
| `ray_traced_shadows/` | `gi_shadow.*`: the sun's shadow rays and the FidelityFX Shadow Denoiser on Direct3D 12. `gi_shadow.hlsl`, `gi_shadow_filter.hlsl`: the rays, the denoiser's temporal pass and filters |
| `ray_traced_lights/` | `local_lights.*`: the game's point and spot light draws kept for their stencil, their light drawn again at the sun pass's list with specular and the rays' shadow (see "Ray-traced lights"). `light_choice.*`: which lights a kick gives the rays (each light's reach, the light cap) and each light as the rays take it. `lamp_shadow_rays.*`, `light_shadows.hlsl`: the lamps' shadow rays and filter, a trace (see "What a kick traces") |
| `global_illumination/` | `gi_light.*`, `gi_light.hlsl`: the light on Direct3D 12, its rays, the denoiser's inputs and the render size (see "Global illumination"); their hits' light and the hash grid cache in `passes/ray_tracing/shaders/gi_hit_light.hlsli`. `gi_denoiser.*`: NVIDIA NRD's RELAX denoiser without NRI. `gi_lighting.*`: the game's sky and sun in the light's terms. `light_combine.*`: the traced light in place of the game's probe light at the resolve |
| `path_tracing/` | `path_tracing.*`: path tracing on Direct3D 11: the traced reflections added at the resolve and blended as the game blends its own, the lamps' light added at the draw point, the masks' atlas (see "Path tracing"). `path_tracing_shaders.hpp`: those draws. `glass.*`, `glass_shaders.hpp`: glass and mirrors, their surfaces drawn beside the game's and what they reflect added over the scene. `sky_capture.*`: the game's sky drawn again for the rays. `path_tracing_rays.*`: its passes on Direct3D 12. `reflections.hlsl`, `direct_light.hlsl`: the reflections and the point and spot lights |
| `ambient_occlusion/` | `gtvbao.*`: GTVBAO's occlusion multiplied into the indirect light at the upscale. `gtvbao_shaders.hpp`: that multiply |
| `indirect_light/` | `ssgi.*`: VB-SSGI's bounce, added to the diffuse light the resolve reads. `ssgi_shaders.hpp`: the gather (with its per-tap visibility test), the temporal accumulation, the a-trous denoiser and the resolve |
| `volumetric_fog/` | `atmosphere.*`: the larger fog texture, the game's fog pass over all of it, the shadowed fill before the resolve. `atmosphere_shaders.hpp`: the game's fog formula per froxel with the sun shadowed; Hillaire's atmosphere tables, the physical fog and sky (Epic Games' reference, MIT) |
| `sea/` | `sea.*`: the sea's waves and its surface in the G-buffer in place of the game's sea, its level and wind from the game's, its depth for the guides and its level for the ray-traced scene (see "Sea"). `sea_shaders.hpp`: the spectrum, the FFT, the maps, the surface |
| `photo_mode/` | `photo_mode.*`: photo mode (F2), the game's pause, a free camera through the camera selector, the game's input held back; hiding the UI (F3). `focus.*`: its autofocus |
| `hdr/` | `hdr.*`: the float scene, in SDR too, and HDR10 output: the swap chain, the float scene targets and their sRGB views, the shader patches, the grading and output passes (see "HDR"). `hdr_shaders.hpp`: the encode and decode of the float scene, bloom's reduction, the grading, the output to PQ or, in SDR, to the game's 8 bits with a dither |
| `frame_rate/` | `frame_rate.*`: hook on the frame wait setter, the optional 60 fps unlock |
| `motion_blur/` | `motion_blur.*`: hooks on the motion blur plugin, velocity passes every frame |
| `depth_of_field/` | `depth_of_field.*`: the game's depth of field with its focus edges at the render size: each pixel's own cover from its depth, the far layer kept apart, the composite, and the lens's far blur (see "Depth of field"). `depth_of_field_shaders.hpp`: the cover, the lens's passes and the composites |

### Passes

| File | Purpose |
| --- | --- |
| `src/passes/sun_light_pass.*` | The game's sun light pass drawn with replacement shaders, with soft shadows, the sun's contact shadows and the ray-traced shadow composed in. `sun_light_pass_shaders.hpp`: the game's sun light shaders rebuilt with them |
| `src/passes/local_light_pass.*` | The game's point and spot light draws, with the lamps' contact shadows and soft shadows composed in: the marking and lighting draws |
| `src/passes/contact_shadow_trace.*` | Bend Studio's screen-space shadow trace, for the sun's and the lamps' contact shadows. `contact_shadow_trace_shaders.hpp`: Bend's trace ported to FXC |
| `third_party/bend_sss/bend_sss_cpu.h` | Bend Studio's dispatch layout, unchanged (Apache License 2.0) |
| `src/passes/indirect_light_pass.*` | The game's sky light, upscale and resolve: the G-buffer notes, the resolve's white occlusion and diffuse light, the diagnostics views, the ray-traced scene's notes. `indirect_light_pass_shaders.hpp`: the diagnostics views |
| `src/passes/visibility_bitmask.*` | The visibility bitmask march GTVBAO uses: the depth prefilter, the march, the denoiser; VB-SSGI takes the prefilter's distance mips for its per-tap occlusion test (`prefilterDistances`). `visibility_bitmask_shaders.hpp`: the march (GTVBAO, from three-gtvbao), XeGTAO's noise and denoiser |
| `src/passes/ray_tracing/ray_tracing.*` | The ray-traced scene's frame on Direct3D 11: the kick, the waits, the shared textures the global illumination and the ray-traced shadows hand back |
| `src/passes/ray_tracing/kick_traces.*` | What each kick traces, the one answer for everything that makes, runs or reads a trace (see "What a kick traces") |
| `src/passes/ray_tracing/trace.*` | A trace's steps, and the traces in the order the frame runs them (see "What a kick traces") |
| `src/passes/ray_tracing/shared_textures.*` | What Direct3D 11 and 12 share: the Direct3D 11 half of a trace's results, and the rows that open, transition and bind a trace's textures |
| `src/passes/ray_tracing/gi_renderer.*` | The Direct3D 12 side: the device, the scene's frame (the scene contents' update, then the recording), each feature's passes recorded in turn, the probes |
| `src/passes/ray_tracing/scene_contents.*` | The scene contents: the game's models, their variants, characters, terrain rings, the sea's plane and textures the ray-traced scene holds, made as the scene memory has room and let go after a while unplaced; each frame's instances of them; their diagnostics (status, the probes' hits, F10's `models.txt`) (see "Ray-traced scene") |
| `src/passes/ray_tracing/scene_memory.hpp` | The scene memory: the seam the scene contents reach the GPU through (the pool, structures, texture slots, instances) |
| `src/passes/ray_tracing/gi_scene.*` | The ray-traced scene on Direct3D 12, the scene memory: the geometry pool, bottom-level structures (a geometry per mesh, compacted), the geometry table, the game's textures' slots, the frame's instances and top level (see "Ray-traced scene") |
| `src/passes/ray_tracing/engine_scene.*` | The game's loaded models from its visibility managers and object brush, its characters from its model list, their model files read once, its texture manager, the terrain's heightmap inputs (see "Scene objects") |
| `src/passes/ray_tracing/scene_description.*` | What the ray-traced scene takes of a model file: its FMDL meshes, each with its mesh role from its material, its alpha and shadow bytes and its model source, their base textures and skins; the flags, texture and alpha threshold a role gives its row of the geometry table (see "Ray-traced scene", "Mesh roles") |
| `src/passes/ray_tracing/engine_brush.*` | The game's object brush: its loaded blocks' vegetation and props, read from their placement records (see "Scene objects") |
| `src/passes/ray_tracing/gi_inputs.*` | What the ray tracing takes from the game's draws: the main camera of the frame drawn, its skinned meshes' bones, the terrain's inputs |
| `src/passes/ray_tracing/gi_textures.*` | Shared copies of the game's textures the ray-traced scene's meshes use, from its texture manager (see "Ray-traced scene") |
| `src/passes/ray_tracing/gi_d3d12.hpp` | The kernels, root signature and descriptor tables every ray-traced feature shares |
| `src/passes/ray_tracing/gi_slots.hpp` | The slots of those tables, a type a kind; `gi_bindings.h`, generated, names each one (see "Kernels and their slots") |
| `tools/kernel_bindings/` | Build tool: C++'s slot names, `Kernel` enum and table of DXIL, from the compiled kernels (see "Kernels and their slots") |
| `src/passes/ray_tracing/shaders/` | HLSL the ray-traced features share: `gi_shared.h` (also C++), `gi_common.hlsli`, `gi_shadow.hlsli` (where shadow rays leave a surface; the FidelityFX Shadow Denoiser's data access), `gi_hit_light.hlsli` (a ray's hit lit as the global illumination lights it, the hash grid cache), `ggx_specular.hlsli` (the specular of what path tracing's rays hit), `gi_scene.hlsl` |
| `third_party/ffx_denoiser_shadows/` | AMD's FidelityFX Shadow Denoiser headers, unchanged (MIT; `licenses/AMD-FidelityFX-LICENSE.txt`) |

### Shared

| File | Purpose |
| --- | --- |
| `src/render_scale.cpp`, `src/render_scale.hpp` | Hook on the renderer's per-frame step: the engine renders at the requested size |
| `src/ui_redirect.cpp`, `src/ui_redirect.hpp` | The draw call every module sees; the grading, the 2D pass and `GammaControl` on output-size targets while the engine renders below them |
| `src/game_constants.hpp` | The game's constant buffers by name (see "Constant buffers"): the system, scene, sun and lamp light, material, object, bones and probe constants, each with its slot, and a light pass's view space in the world |
| `src/constant_capture.cpp`, `src/constant_capture.hpp` | The constant capture: what each of the game's threads last wrote to the constant buffers the contact shadows and the ray tracing read, copied at the game's unmap after the upscaler's jitter, read by type and only at the size written |
| `src/camera.cpp`, `src/camera.hpp` | The main camera in the scene constants: main-view recognition, jitter, reprojection, camera cuts |
| `src/game_shaders.cpp`, `src/game_shaders.hpp` | The game's shaders the add-on knows, in one table: each pixel shader's checksum, the game pass it draws and what tells it apart from its pass's others (a sun's shadow map, a light's kind); its vertex shaders by source name (the 2D pass, models, terrain). Every copy the game makes is known; a draw looks its pixel shader up once (`DrawnShader`) |
| `src/game_lights.hpp` | What a point or spot light shader is (its row in `game_shaders.hpp`), for the local light pass and the ray-traced lights: its kind and twin, a light's range, its sphere on screen |
| `src/device_check.cpp`, `src/device_check.hpp` | Lets the game start under ReShade (see below); the build check |
| `src/game_code.hpp` | The game's image, checks of its code, hooks (MinHook) and code patches |
| `src/game_memory.hpp` | Guarded reads of the game's objects |
| `src/game_loop.cpp`, `src/game_loop.hpp` | Hooks on the main loop's frame start and end, for Reflex and the weather menu |
| `src/fidelityfx.cpp`, `src/fidelityfx.hpp` | Loads the AMD FSR API, for FSR upscaling and frame generation |
| `src/weather.cpp`, `src/weather.hpp` | The overlay's weather and time control, in Utility (see "Weather and time") |
| `src/sun_movement.cpp`, `src/sun_movement.hpp` | The sun and moon move every frame instead of in the game's steps (see "Sun movement") |
| `src/game_dither.cpp`, `src/game_dither.hpp` | The game's colour dither taken out of its shaders as they are made (see "Dither") |
| `src/dev_tools.hpp` | Whether the build has the Developer page (the dev preset) |
| `src/game_lua.cpp`, `src/game_lua.hpp` | Lua in the game's script state, and the Developer page's console (see "Game Lua") |
| `src/travel.cpp`, `src/travel.hpp` | The Developer page's travel: free roam in another map (see "Travel") |
| `src/frame_log.cpp`, `src/frame_log.hpp` | Development diagnostics, off by default: shader log, F10 frame record, constant and texture dumps |
| `src/resource_dump.cpp`, `src/resource_dump.hpp` | Format names, constant rows, DDS saving for the dumps |
| `src/shader_bytecode.cpp`, `src/shader_bytecode.hpp` | DXBC checksum (bytes 4 to 20 of the container), the instruction walk, the output saturation patch, the dither patch |
| `src/log.cpp`, `src/log.hpp` | `PhantomFX.log` beside the add-on, one per game session |
| `src/clock.hpp` | `QueryPerformanceCounter` ticks and the milliseconds between them |
| `src/frame_latch.hpp` | A module's settings as configure sets them and as the frame being recorded uses them, latched at present |
| `src/command_list_notes.hpp` | A module's notes by deferred context, then by command list, until the immediate context runs it; their merge in the order they run, and their expiry (see "What the add-on changes", "Command lists") |
| `src/hresult.hpp` | `check`: a failed Direct3D or Windows call as an exception naming the step |
| `src/d3d11_helpers.hpp` | Direct3D 11 helpers for the effects: shader compiles, dynamic constant buffers, compute textures, texture sizes |
| `src/d3d12_helpers.hpp` | Direct3D 12 helpers for the ray tracing, FSR and the presenter: barriers, the device on Direct3D 11's GPU behind ReShade's proxy, shared textures and fences opened in Direct3D 12 |
| `src/version.rc.in` | Version resource template: name, author and version from `CMakeLists.txt` |

### A new feature

A new lighting effect takes:

1. Its folder in `src/features/`: `settings.cpp` and `settings.hpp` with its settings and
   `describe()`, which gives their `PhantomFX.ini` keys, ranges, controls and menu text, and what
   the effect replaces or builds on (`needs`, `needsRayReconstruction`: it runs only while that runs); and what draws it. The settings file goes in `PHANTOM_FX_FEATURES` and the
   renderer's in `add_library` (`CMakeLists.txt`), its settings in `Settings` and `forEachFeature`
   (`src/phantom_fx_settings.hpp`), and its switch in the presets where it belongs.
2. Its `Effect`, at its place in the cost measurement's order (`src/effects.hpp`).
3. Its renderer's row in `Modules` (`src/modules.cpp`): its `attach`, `configure`, `present` and
   `detach`. Its `configure` keeps its settings (in a `FrameLatch`) and whether the effect plan
   runs it (`configuration.effects.runs`), so the game build, the cost measurement and the
   enhancements switch follow. Where it changes a game pass others change too, it plugs
   into that pass (`src/passes/`). Where it handles the game's draws itself, it knows them by their
   game pass (`row()` of the draw's `game_shaders::DrawnShader`; a shader the table lacks is a new
   row in `src/game_shaders.hpp`) and goes in the draw events (`onDraw`, `onDrawIndexed` in
   `src/addon.cpp`): what it only takes from a draw is a `note`, which returns nothing and runs with
   the other notes, before every owner; what may draw it in the game's place, or skip it, is a `draw`,
   which goes with the owners and takes game passes no other owner takes.
4. Its timed passes in `Pass` (`src/performance.hpp`), and its budget row in `EffectLines`
   (`src/menu_budget.cpp`): their names and its `health`, which says whether it waits or failed.

Any other feature takes steps 1 and 3. What the running game adds to its menu item (a state, a
message, choices the GPU cannot run) comes from its `statusOf` in `src/menu.cpp`.

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
| `nvngx_dlss.dll`, `nvngx_dlssd.dll` | This build (the DLSS SDK's, copied beside the add-on) |

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

**Fix.** ReShade loads add-ons inside `D3D11CreateDevice`, before the check runs.
`passDeviceCheck()` (`src/device_check.cpp`) verifies the build and the function's first bytes
(`40 53 48 81 EC 60 04 00 00`) and writes `mov al, 1; ret` (`B0 01 C3`) over its start, in memory
only. The game file is never changed. Without the add-on, ReShade alone cannot run the game.

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

**Unlock.** `src/features/frame_rate/frame_rate.cpp` verifies the build and the setter's first bytes
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
where the engine's object velocity was used, of the global illumination denoiser's history (red
where it started a pixel's light over within three frames, yellow under ten) and of NRD's validation
layer for that denoiser (see "Global illumination", "Denoiser"), jitter off, the game's FXAA for comparison (DLSS stays
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
  with everything on, then leaves out, one more at a time and only where running: the volumetric
  fog, the ray-traced shadows, the ray-traced lights, the global illumination (its light passes,
  then the rest), VB-SSGI, GTVBAO, the lamp and the sun contact shadows, the soft shadows, the
  sharpening and last the upscaler (the game's FXAA at the same render size, as the Diagnostics
  comparison). The lighting effects go in `Effect`'s order, and what runs is the effect plan's
  (`planEffects`, `src/phantom_fx_settings.hpp`): a replaced effect stays off while its replacer is left out,
  so each step leaves out one effect alone. 1.5 s to settle and 2.5 s timed per step; the
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
   the scene halved by a `CopyRenderBuffer`, two `MotionBlurMcGuire` passes at 960x540 between two
   8-bit targets (the second reads the first's result), blended over the scene (`CopyRenderBuffer`,
   SRC_ALPHA / INV_SRC_ALPHA). `MotionBlurMcGuire` places its samples in texture coordinates: the
   step is the velocity times `cPSObject` c11.w times `cPSSystem`'s `m_renderBuffer.zw`, and the
   result's alpha is scaled by its input's alpha.
   - `MotionBlurTile` keeps the longest velocity of each 2x2, halving from the render size to tiles
     of 32 render pixels (five passes, 1280x720 to 40x23 in frame 6607); `MotionBlurTileNeighbour`
     keeps the longest of each tile's 3x3.
   - `MotionBlurBakeVelocityDepth` writes at half the render size each pixel's speed, depth and the
     longer of its own velocity and its tile's (`t4`, bilinear). It point-samples the velocity and
     depth at the render size by texture coordinate, makes that coordinate from the clip position,
     and reads no size of its own target.
   - `MotionBlurMcGuire` reads only the scene (`t0`) and the bake (`t5`). It point-samples the bake at
     its own pixel and at each of its six taps for its depth and velocity tests, which set the weights
     and the result's alpha.
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

The add-on reads these by name through `src/game_constants.hpp`, whose `static_assert`s check each
register's offset.

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
- Exec repeats the test, then sets the blur's shutter ratio at `[plugin+0x70]`: shutter x amount
  times the frame rate (1 over the double at `[[view+0x570]+0x130]`, or 60 when bit 0 of
  `[plugin+0x78]` is set), capped at 1 and doubled. It draws the blur through three renderer calls
  (vtable `+0x468`, which takes `plugin+0x6C`, `+0x470`, `+0x478`): tiles, neighbours, bake, the
  McGuire passes and the composite. The cap is reached whenever frame rate x shutter x amount passes
  1, so a scale on the amount alone does not shorten the blur there.
- The flag is cleared every frame: when the add-on stops setting it, the next frame has no
  velocity passes. The three globals have no RIP-relative writers; they are written through
  pointers, presumably from the graphics settings. The add-on writes the amount (see "What the add-on
  changes", "Motion blur").

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

### Dither

The game adds a fixed 8x8 ordered dither to some of its colour, against banding in its 8-bit scene.
Before the upscaler it is a cell per render pixel, which DLSS enlarges with the image: a grid over
dark areas, worst at night in fog (issue #28). Found 2026-10-28 in the shaders' disassembly
(`%TEMP%\mgsv-fog\all`) and the frame records.

- **The textures.** Four 8x8 `B8G8R8A8_UNORM` textures, read through `g_samplerPoint_Wrap` (`s8`) at
  `frac(pixel / 8)`. Red is an ordered pattern from 1 or 2 to 253 or 254, averaging a half; alpha
  averages a quarter (64 of 255; 71 in one), flat in two of them and ordered in the others; green and
  blue hold 1. 42 pixel shaders call one `inMesh` and add it to their colour: the three sky draws
  (`t8`) and `DR_VolFog_TppTonemap_MD` (`t15`) its red as `(d - 0.5) / 128`, `tpp3dfw_constant_sky`
  and `tpp3dfw_generativeClouds` as `(d - 0.5) / 64`, the other forward materials (`fox3dfw_*`, glass)
  mostly its alpha as `d / 128` on their fog's colour and transmittance. 211 G-buffer shaders read the
  same textures as `g_tex_mesh`: red against the model's fade, which dissolves it, and alpha times
  `cPSSystem` `m_param.w` as the alpha test's threshold, so the textures stay.
- **When.** All 7 frame records with `Sky_Draw_TppNoSky` (fog) resolve with `DR_VolFog_TppTonemap_MD`;
  none of the other 161 does. The plain resolve has no dither. The dither is added after TppTonemap,
  in linear colour before the sRGB view encodes it: at night's 8 to 16 of 255 a step of 1/256 is
  several levels.
- **Patch.** `create_pipeline` hands Direct3D a copy of each shader with an `inMesh` texture (by
  reflection) in which every sample of it is a `mov` of the textures' average in each channel (a half,
  1/255, 1/255, a quarter), in the sample's swizzle, and nops to the same length. Every pixel then gets
  what the dither added on average: the centred forms nothing, the forward materials a quarter of
  1/128, so they keep their brightness. It runs before HDR's patch, which finds its TppTonemap the
  same way; both orders give the same code. The add-on's own sky draw has no dither either.
- **SDR.** Without the dither SDR's 8-bit scene banded a little in smooth fog (2026-10-28: steps of 1
  and 2 levels at DLSS Quality); with it, DLSS's image was softer. It comes out in SDR too. Since
  2026-10-30 SDR keeps the scene in 16-bit float, as HDR does, and dithers once at the output (issue
  #54, see "HDR").
- **Left as they are.** The G-buffer's alpha test with the ordered alpha stipples cut-out edges (hair,
  manes) before DLSS too; taking it out would make those edges hard. The shadow filters'
  `MicroDither` and the sun pass's 2x2 tap offset move a comparison by an eighth of a texel.
- Checked over the 806 pixel shaders the frame log saved: exactly the 42 (and HDR's 4 copies of
  them) patched, all taken by Direct3D on an RTX 5080.

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
| `DR_VolFog_TppTonemap`, the resolve recorded; nine variants in `src/passes/indirect_light_pass.cpp` | `00a609370c2e8b47a645263f91f6d3e8` |

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
| `0x15F010` | Creates the stream controller from that description (only the setup calls it) |
| `0x2B73434` | The level of detail's reference resolution, set by `GrTools.SetLodFactorResolution` (`0x1BB2D0`); 0 uses each view's width |
| `0x2B74330`, `0x2B77FB0` | The models' and the vegetation's `RejectionLengthBias` settings; `+0x64` the value the draw jobs read |
| `0x205AF0` | Selects those two settings' value by a graphics quality's name |
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
  every shown one in view was. The scene leaves hidden models out. `+0x1B8` the model's fade, 0 (gone)
  to 1 (as drawn); the constructor (`0x1C77B0`) sets 1 (`+0x1BC` 0.9, `+0x1C0` 1). Watched 2026-10-27
  in Mother Base, read four times a second: a model faded in from 0.03 to 1 in steps of about 0.14,
  several times, and three near the camera dipped to 0.83, held, and came back to 1. Held at 0: of 257
  shown rigid models only two, a building's outer shell (model `84a2063b554c5d57`, mesh 0, main view
  only, 1,318 triangles) and one whose meshes the scene leaves out anyway, and 23 hidden ones; none of
  240 instance groups' models. The shell stayed at 0 while the player walked 45 m away and back. What
  writes the fade is not found; the draw-list builder does not read it. The shell lay 13 cm off a wall
  the game drew, and an F10 frame there held no draw of it; in the scene, the wall's global
  illumination rays met its inside (63 of the shadow probe's 64 within 1 m) and the wall went black up
  close, where the copy tolerance (gi_common.hlsli) no longer skipped it. The scene leaves models faded
  to 0 out. Open: whether a model faded to 0 still casts in the game's shadow maps (the shell casts
  nothing: main view only).
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
  360 skinned. Characters are among them and in no visibility manager. So, at times, are a few rigid models, the
  scene taking those from here: the helicopter's cabin between missions (`84a2d14c3f28beb5`, `84a3b06a43ea483f`, at
  (0, 1200, 0), 71% of the frame there), 438 models in all there, and none at Mother Base or in Afghanistan. Arriving
  from another map no visibility manager holds it; straight from Mother Base one can. Without them the rays had no
  walls: the cabin was lit 4 times as brightly as the game's, and the sun's rays reached 76% of the frame the game's
  shadow map has blocked.
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
  and the shadow-only decal (alpha 128, shadow 66) test; alpha 16 (drawn forward, shadow 4) does not and casts a
  solid shadow. The scene takes alpha bits 5 and 7 as cut out, the rest as opaque. Alpha bit 4 marks a mesh drawn
  forward, not glass: most of those are `fox3DFW_ConstantSRGB`. One with shadow bit 0 the game never casts,
  and the scene leaves it out for every model. Glass is known by its material instead (see "Path tracing",
  "Glass and mirrors"): taken as solid, a helicopter's windows kept the sun out of its cabin (2026-10-25).

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
  module notes as a deferred context records (the upscaler's grading, FXAA pass and velocity
  texture, the ray tracing's inputs and camera, the point and spot lights, the timed passes) is kept
  by context in the module's `CommandListNotes` (`src/command_list_notes.hpp`), moved to the command
  list at `FinishCommandList` (ReShade's `execute_secondary_command_list` with the new list and the
  deferred context) and handed over once, as the immediate context is about to run the list (the
  same event, which fires before the real call).
  - Notes merge in the order the immediate context runs them; each module's `merge` says what later
    means for each note: flags add up, the latest texture, sky or sun wins, the point and spot
    lights and the timed passes append, the first camera stays.
  - A list a deferred context runs inside it (the same event with the context and the list) joins
    the context's notes after what it noted so far. The game is not known to do it: the log says so
    once if it does.
  - Notes that waited through three presents without being handed over expire at the fourth: a
    context never finished, a list never run. The diagnostic log counts them, in the upscaler's
    status line, the global illumination's (with its cameras), "Lights in ten seconds" and a
    "Costs:" line when there are any; normal play should have none.
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
  confirmed with 4x jitter (only that sign is sharp). The upload whose `c0-c3` is `P x V` with `V`
  in `c8-c11` and the largest viewport (`c22.xy`) is the frame's camera, captured before the jitter;
  the first one with ties. Until 2026-10-26 the first won, and DLSS never ran in the intro: its mirror's
  reflection is a camera of its own, drawn first at 1080x608 ("the camera's viewport is not the FXAA
  pass's size"). A frame DLSS did not run
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
  only ever called on the immediate context. With the scene in float, as on every supported build
  since 2026-10-30, the colour is the scene's linear light before any tonemap or grading, in
  `R16G16B16A16_FLOAT`, and the feature has `IsHDR` (see "HDR").
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
    field's copy of the scene (its input has the render size; it stays, before DLSS), the
    half-size layer's composite over the scene (lens flares) and, with motion blur on, the blur's
    composite: those two go to the output target.
  - The motion blur gathers at half the output size from DLSS's image, as it does at half the render
    size without upscaling (frame 141835, 1920x1080 to 3840x2160, before this: it gathered at 960x540
    from the jittered scene before DLSS, and its composite stretched that four times over the
    output, pixelated). Two more targets at half the output size stand in for the game's two, in the
    scene's format: the game's are 8-bit, which clipped HDR highlights while the camera moved.
    Before the first `MotionBlurMcGuire` pass, the output target is halved bilinearly into the
    first, with the alpha of the game's own halved scene (the output target has the composite's
    alpha, which the blur would take for its own). Each pass then reads the add-on's target in place
    of the game's, writes the other, and keeps the game's `cPSSystem`, so the samples land as far
    away on screen; the composite reads the last one. The game's targets are matched by address on
    the recording thread, within the frame being recorded.
  - The blur's velocity and depth (`MotionBlurBakeVelocityDepth`) are baked at half the output size
    too, into a third surface in the game's 8-bit format, and both passes read it at `t5` in place of
    the game's. With the game's bake at half the render size, each gather pixel took its depth and
    velocity tests from a bake pixel up to three times its width, and a moving character's
    silhouette stepped by it: 5-6 px at Ultra Performance and 2-3 px at Quality over 3840x2160, none
    at DLAA, where the two sizes match (issue #48). The bake reads no size of its own target (see
    "How Fox Engine renders", step 9), so it is drawn again with the game's constants, only its
    target, viewport and scissor changed. The tiles it reads stay at the render size: they are 32
    render pixels wide and read bilinearly by texture coordinate. The game's own bake still draws, so
    the passes fall back to it whenever the add-on's is missing.
  - With the scene in float the blur takes the same route without upscaling too (DLAA, or the
    upscaler off), as the game's two targets are 8-bit. They are the game's shared half-size scratch
    targets, which its SSAO draws into earlier in the frame (frame 141835, #491 to #496), so they
    are not made float where the game creates them. The first pass halves the scene the game's own
    halving `CopyRenderBuffer` read (the last copy on the thread whose target the pass reads), and
    the composite stays on the scene, reading the add-on's result at t0; the game's input goes back
    after the draw. The bake keeps the game's: it already has the gathers' size. The blur's surfaces
    are made apart from the other output-size targets, so this route makes only them: 41 MB at 4K
    (8 MB of it the bake's, unused here), where the full set would add 166 MB unused.
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
  with its motion blur on. `motion_blur.cpp`
  checks the build and the first bytes of PreExec (`80 79 68 00 49 8B 80 70 05 00 00`) and Exec
  (`48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20`) and detours both with MinHook at `AddonInit`.
  While DLSS replaces FXAA (any DLSS mode, not the FXAA comparison) or **VB-SSGI** runs, PreExec runs
  the game's code and then sets the velocity bit, and Exec draws the blur only when the game's
  setting asks for it and the add-on's **Motion blur** toggle is on. So the velocity passes run
  every frame whatever the game's setting — VB-SSGI's temporal accumulation reprojects with the
  buffer they draw. Off and the comparison give both back to the game's setting from the next frame.
  When a check fails nothing is hooked and the game's setting stays in charge. `AddonUninit` removes
  all engine hooks with `MH_Uninitialize`. The blur runs after DLSS, at half the output size while
  upscaling (see "The UI at the output size").
  After PreExec, the add-on writes the amount that gives Exec's shutter ratio (see "Velocity without
  motion blur") the game's own times the **Strength**, from the same view's frame rate and shutter
  (`motion_blur::scaledAmount`). Above 100% it grows only up to Exec's cap. While frame generation
  runs the ratio is halved as well, as NVIDIA's DLSS-G guide asks: frame generation gets the blurred
  image, and each frame it shows covers half the motion. Checked with F10 records (the DLSS rows
  before the blur came back with the toggle):

  | Motion blur | Add-on | Velocity passes | Blur passes |
  | --- | --- | --- | --- |
  | Off | DLAA | yes | none |
  | Off | Off | none | none |
  | Low | DLAA, before the toggle | yes | none |
  | Low | Off, or the FXAA comparison | yes | tiles and both McGuire passes |
  | On | Performance at 4K, toggle on (frame 141835) | yes | tiles and both McGuire passes |

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
  size it averages out.
- The motion blur's velocity and depth exist only at the render size, and its bake point-samples
  them. A moving silhouette therefore still steps by a render pixel where that is wider than the
  gather's two output pixels: at Ultra Performance over 3840x2160, by 2 and 4 px in turn, as the
  gather's 2 px pixels fall on the render's 3 px ones (3 px on average). Sampling them bilinearly
  would not help: it would blend a character's depth and velocity with the background's into values
  that belong to neither.

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
  memory. The game's swap chain becomes a stand-in (`src/features/frame_generation/presenter.cpp`)
  whose back buffer is a texture in the game's format that Direct3D 11 shares with Direct3D 12. A
  native Direct3D 12 device
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
  A native Direct3D 12 device on the game's GPU opens them
  (`src/features/upscaling/fsr_upscaler.cpp`, on `d3d12_host::Host`, which Ray Reconstruction shares); Direct3D 11 signals a shared fence and flushes after the inputs pass, Direct3D 12 runs FSR on its own queue,
  and Direct3D 11 waits on the GPU before the composite reads the output.
- **Colour.** FSR is made for linear colour, so the inputs pass decodes the sRGB-encoded image into
  `R16G16B16A16_FLOAT`, the game's 8-bit LDR one too, and the composite encodes FSR's output again,
  as for DLSS in its HDR mode. With the scene in float FSR runs in its HDR mode.
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

## Ray Reconstruction

With **Ray Reconstruction** (`ray_reconstruction=1`, under the upscaler, DLSS only, shown while a
ray-traced effect is on) NVIDIA's DLSS Ray Reconstruction takes DLSS's place: one model that denoises the ray-traced light and upscales, as NVIDIA's
DLSS-RR Integration Guide (DLSS SDK 310.9.1) describes it. It runs for a frame when it is asked for, the
scene is float (see "HDR"), NGX says this PC runs it, and the ray-traced scene's kick made its guides
that frame, so with the global illumination, the ray-traced shadows or the ray-traced lights on;
otherwise DLSS runs, and the upscaler's status line says why.

- **On Direct3D 12** (`src/features/upscaling/ray_reconstruction.cpp`). NGX's Direct3D 11 path for it
  crashed on its first evaluation, inside NVIDIA's code, in both the SDK's 310.9.1 and the driver's
  310.9.16 (2026-10-30): `NGXCubinD3D11::MarkBufferResourceAsAccessWrittenD3D11` (its
  `ngx_cubin_d3d11.cpp`, line 950) reads a dispatch's list of written resources, which only a later
  virtual call sets, through a null pointer (an access violation reading `0x34`). Neither the camera's
  matrices nor another add-on following NGX made a difference. So, as FSR, it runs on a native Direct3D 12
  device on the game's GPU: the upscaler's colour, depth, motion and output and the guides are shared
  textures, Direct3D 12 waits on a shared fence for Direct3D 11's inputs and Direct3D 11 for its output.
  The colour is the float scene's linear light (`IsHDR`), the depth the hardware's (reversed-Z), the
  motion per render pixel; the preset `ray_reconstruction_preset` (Default, F, E or D, NGX's values, for
  every quality mode, as DLSS's preset); world-to-view and view-to-clip matrices for row vectors
  (`camera::StreamlineCamera`), which Streamline always passes; at least 32 jitter phases, as the guide
  asks. Turning it off releases the feature and keeps NGX: shutting NGX down on Direct3D 12 broke frame
  generation's feature on the presenter's device (Streamline: "NGX evaluate feature failed 0xbad00004"
  from that moment). Ray Reconstruction and DLSS each keep their own history, so the one taking over
  from the other starts over. Where it cannot start, or fails, DLSS runs in its place from that frame for
  the session; the release ships NVIDIA's release `nvngx_dlssd.dll` beside the add-on.
- **Guides** (`GuideSource` in `ray_tracing.cpp`, at the kick, from the G-buffer at the render size): the
  world-space normal with the roughness (the specular target's `x`) in `w`, `R16G16B16A16_FLOAT`, NGX's
  packed roughness; the diffuse albedo, linear, `R8G8B8A8_UNORM`; the specular albedo, `R16G16B16A16_FLOAT`
  (a few hundredths, too small for 8 bits), as the game weights its sky reflection (`SKY_REFLECTION`): the
  material table's colour (`g_MaterialTexture` row 0.25) times the specular target's intensity `y`, times
  the Fresnel table `g_lut1` at the view angle for the material's F0 (row 0.75) or the sun pass's least F0
  (`cPSLight` `c2.z`, 0.21 on Mother Base's deck, 0 in the helicopter; the frame before's), times half of
  1 minus the roughness. The game's materials are tuned to that weighting: with a physical one (NVIDIA's
  `EnvBRDFApprox2`), Snake's red arm (an 11% white specular of roughness 0.59) reflected 3 to 4 times as
  much and the sky's light turned it grey. The game then scales its sky reflection by `max(shadow, c2.x)`
  times `c2.y`, which the guide leaves out: `c2.x` was 1, and `c2.y` (0.36 to 0.49) drifts with the time
  of day, a scale for the sky map rather than for the material. Where the sky is: no normal or roughness,
  albedo 0.5, no specular albedo. The sun pass's `t9` and `t10` give the tables
  (`indirect_light_pass::materialTables`).
- **Depth of field guide.** Ray Reconstruction runs after the game's depth of field, which blurs the
  noisy light into spots, and NVIDIA's guide asks for its guide then: `luminance(after - before)`, with
  luminance `(r + 2g + b) / 4`, `R16_FLOAT` at the render size, 0 where nothing was blurred. On a frame
  `DOF_ScatterCompositeFinal` drew (noted at its recording, `Recorded::dofFrame`), the upscaler's input
  pass makes it from the scene and the float copy the composite drew over (`hdr::sceneBeforeDof`), both
  decoded to linear light. Preset D cannot take it, so it gets none.
- **Noisy light for it** (`kick_traces::KickTraces::isForReconstruction`, `GIFrameConstants::reconstruction`).
  Ray Reconstruction asks for samples apart at every render pixel, without other denoisers. While it
  denoised the frame before (the upscaler counts its frames, `ray_tracing::reconstructed`), the kick
  leaves the light and the shadows to it: the global illumination takes a sample a render pixel, one ray
  (two on foliage) whatever the ray quality, its grid the render size, without NRD (not even made) or
  `PrepareGuides`, and `CompositeLight` passes each render pixel's own rays' light on (`PerRenderPixel` in
  `gi_light.hlsl`), without the foliage memory, whose light is other pixels' and frames'; the sun's shadow is each pixel's ray as it found the sun (`TraceSunShadows` writes the
  output, AMD's denoiser is left out and starts over after); the lights' shadows are each pixel's rays'
  share (`TraceLocalLightShadows` writes the output, the filter is left out), their starts white noise in
  place of the interleaved gradient noise, which is a pattern across the screen. A frame it left to DLSS, or
  none, turns the denoisers back on for the next. The global illumination's light is made again when its
  grid changes. Its denoiser's debug views are off meanwhile. Measured on a 2560x1440 render (a base,
  2026-10-30): the light 2.1 ms with NRD at half size, 1.8 ms a render pixel; the sun's shadows 2.8 to
  3.4 ms with the denoiser, 1.4 ms without.

## Path tracing

With **Path tracing** (`path_tracing=1`, under lighting) the reflections, the sun's light and every point
and spot light's light come from the ray-traced scene, while DLSS Ray Reconstruction denoises (see "Ray
Reconstruction") and the global illumination runs. It is a lighting effect of its own
(`Effect::PathTracing`), which builds on the global illumination and Ray Reconstruction
(`FeatureInfo::needs`, `needsRayReconstruction`: without them the plan has it `Lacking`, and its budget
line says what it needs) and waits for Ray Reconstruction to denoise: the frames it denoised before take
it (`kick_traces::KickTraces::isPathTraced`, `ray_tracing::isPathTracing`), and the budget line says when it
waits. Primary visibility stays the G-buffer, as in Cyberpunk 2077's path tracing.

It does what the ray-traced shadows and lights do, with rays of its own, so it replaces them in the plan
while its switch, the global illumination's and Ray Reconstruction's are on, and with them what they
replace (soft shadows, sun contact shadows, the lamps' contact and soft shadows): replacers chain, and the
farthest along that is on takes the place (`EffectPlan::replacers`). The sun size and the flashlight
brightness show in its item too. Without those switches the hybrid stays: the game's reflections and light
shaders, with the ray-traced shadows and lights where they are on. Where Ray Reconstruction is asked for but
cannot start, path tracing waits and the effects it replaces stay off; what it would take (the sun's rays,
the game's lights) is left alone until it runs.

- **Reflections** (`TraceReflections`, `src/features/path_tracing/reflections.hlsl`): a ray a render pixel
  along a reflection the surface's roughness spreads (GGX's visible normals, Heitz 2018, roughness squared,
  from Ray Reconstruction's normal guide). On a surface of roughness 0.5 and up (`RoughSurface`), most of what
  the game draws, a reflection is a faint blur: the ray's hit takes the light cache's light for its place (`CachedLight`),
  one ray a pixel; where no ray has lit that cell yet, and at a self-lit mesh, it is shaded as below. On a glossy
  surface the hit has its material (see "Hits" below) and takes the sun, a light and a self-lit mesh afresh, each
  diffuse by its albedo. The sun and the light are specular by GGX too; a self-lit mesh's specular light is the
  specular ray that meets it. One of its lobes then goes on, chosen by what each reflects: the
  specular one, on a glossy hit, by GGX's visible normals again, so a reflection holds reflections, up to three
  specular rays a path (`SpecularRays`), or the diffuse one, whose ray ends the path at the sky's light or the
  light cache as the global illumination's rays do. A ray that gets away takes the
  sky the game draws (see "The sky" below), as the disc of the sun stays the sun's specular. Its light times the
  specular albedo guide and the exposure goes to the resolve, which scales it by the menu's **Reflections**
  strength (`path_traced_reflections`, 1 by default) and adds it to the game's specular light
  (`path_tracing::addReflections`), and its hit distance to Ray Reconstruction (`pInSpecularHitDistance`,
  65504 where it got away), which moves the reflections with what they reflect. The game's sky reflections
  make way: the sun pass leaves its own out (`SKY_REFLECTION 0`), indoors the sky's specular pass
  (`SunLight_ES`) is skipped, and in rain the rain filter's on wet surfaces goes (below). With the sun pass's
  shaders unavailable its sky reflection stays (the budget line says so).
- **The game's reflection blend, from the rays.** The game's reflection of what is around a surface is not a
  material's reflectance but a look of its own: `LocalReflectionTPP` and `LocalReflectionAddBuffer` give
  `scene x (1 - w) + w x I x S x F x (1 - t) x hit`, with `I` the specular intensity, `S` the pass's strength
  (`cPSObject` `c12.x`), `F = R + (1 - R)(1 - NV)`, `R = (1 - roughness)^4` whatever the material's F0, and
  `w = I x S x (1 - t) x` the fades: the surface's distance (gone at 100 m) and the ray turning towards the eye
  (gone at -0.5 towards it), `t` being how far across the screen the march went against its reach (`c12.z / 2`),
  all times the fog to the 8th. On the sortie screen's floor (roughness 0.6, F0 0.1, seen at NV about 0.2, frames of
  2026-10-30) `w` reaches 0.6 to 0.9 and `F` is 0.8, where GGX reflects 0.13 and the sky reflection's weight, which
  `Reflection` carries, is 0.1: at that weight alone such floors lost the reflection of what stands on them. So the
  blend stays, with the rays as its source. `TraceReflections` writes each hit into a map like the game's
  (`g_ReflectionMap`, `ray_tracing::reflectionMap`: the light by `F`, capped at 4, and the fades; nothing on the sea,
  which keeps water's reflectance, rougher than 0.9 or below an `F` of 0.1, as the game's mask) and the ray's way
  across the screen (alpha of `Reflection`). `ReflectionBlend`, drawn for `LocalReflectionAddBuffer`, fades them by
  `t` and the fog (`FogAt`), and takes the menu's **Reflections** strength. A ray lacks the screen-space march's
  limits: what is off-screen or hidden reflects too, without a fade at the screen's edges. A hit behind the eye gives
  the blend nothing. Only where a surface was drawn in front of the G-buffer's, water, which the G-buffer
  and so the rays lack, does the blend keep the screen-space map: where the depth the reflections' mask read
  (`LocalReflectionMask`, `2d9cb6c5e467cff5e9c6401b4aafebb7`, `t0`) is nearer than the G-buffer's as the kick packed
  it (`ray_tracing::surfaceDepth`). Glass there leaves it out (`glass::surfaceWeights`): that map is the surface's behind
  the glass.
- **Glass and mirrors** (`glass.*`, `TraceSurfaceReflections`). The game's glass has no reflection of its surroundings.
  `fox3dfw_glass*` (disassembly and frames of 2026-10-31) sample a cube map, `t2`, or the common one, `t11`, 128 x 128,
  and draw `(ambient + diffuse) x lerp(R, lerp(GlassColor, R, F), GlassColor.a) + specular`, with `R` the cube map by
  `ReflectionColor` (by the colour of `t0` in the `_color` variants), premultiplied by `alpha = cover x F x` the
  vertex's alpha, `cover` being `t0.x` or, in those variants, `t0.a`. `F` is Schlick's from the material table's F0, but
  glass's rows have an F0 of 0.58 to 0.95 (rows 54, 154, 158: 0.88), so it hardly varies with the angle: how much
  a pane reflects is its texture's. The windows measured have `GlassColor` (1, 1, 1, 1), the doctor's lenses (1, 1, 1, 0).
  The eyes' shell is additive glass (ONE, ONE). A mirror, `fox3dfw_parallax_refrection`, is no reflection at all: a
  painted picture of the room (`t0`, 512 x 512), its UV shifted by a height map, unlit, by a mask (`t2.y`); with a
  negative `cPSMaterial` `c0.x` it reads a texture across the screen, flipped, the intro's mirror camera. Where they
  draw: model glass after the sky and before the game's reflection blend, in the list before it, writing depth; `_prim`
  glass, alpha bit 6 in its file, among the particles after the blend, without depth: 16 of the 17 glass draws of
  four frames (a truck, the hospital's windows, a doctor's glasses). Every glass draw of 141 F10 records comes
  before the list with `Tonemap_Lum_Lacc`, which opens with it. Their pixel shaders' hashes in a frame record
  are the game dither's patched ones (`22ab650c`, `79fd24a8`, `61ff7b30`, `800abf58`, `d12d4eca`, `1e3ae7c7`): find glass by
  its vertex shaders. So while the reflections are traced:
  - **The draws** (`glass::draw`). Each leaves out what it reflects with (`GameShader::reflectionSlot` unbound, reading
    black), which keeps its tint, highlights, cover and fog, and is drawn again with the game's vertex shader and its
    depth test into three shared targets: the depth, the normal with `GlassRoughness`, and the weight the
    game's shader gives its reflection, `alpha x (1 - GlassColor.a + GlassColor.a x F) x ReflectionColor x` the fog's
    share (`GlassSurface`; `MirrorSurface`: its mask and the fog, the normal from the position across the pixel, as
    its vertex shader gives none). The last drawn is the nearest, as the game sorts what blends; the weights blend as the
    game blends, so a pane drawn twice, as many are, weighs as it shows. A mirror with the live picture stays the game's
    (its texture is a render target), and so does whatever a smaller camera's view draws.
  - **The trace** (`ray_tracing::traceSurfaces`). Before the exposure's list, a frame that drew a surface signals the fence,
    and Direct3D 12 traces the marked pixels on the scene the kick built, in a list of their own (`gi_renderer::traceSurfaces`), up to
    `SpecularRays` as any glossy surface; Direct3D 11 waits. The rays' hits come from the same `ReflectedLight`. So
    whatever the game draws reflects, with its own shape and normal map, a character's visor without the rays' pose,
    and a frame without glass traces nothing. The cost goes to the **Reflections** line of the budget.
  - **The composite** (`glass::executing`, `Composite`). The reflection by the menu's **Reflections** and the weight, sRGB
    encoded and added by the surface's alpha, in the scene's encoded values, as the game blends its glass, over a copy of the
    scene, before the exposure, the depth of field and the upscaler read it. So a particle in front of a pane has the
    reflection over it. A traced reflection is added to what the draw left, as encoding the sum of the two would need
    each pane's own colour.
  - **The rays.** Glass used to be left out for its alpha and shadow bytes. It is now `MeshRole::Glass`, taken by its material: any
    with a `GlassColor` vector, which four techniques have and no other (`fox3DFW_Glass_CommonRef`, `_CommonRef_LNM`,
    `_LNM` and two unnamed); mirrors are `MeshRole::Mirror`, by their technique, `fox3DFW_ParallaxReflection`. They are non-opaque
    in their structures, so `CutOutBlocks` lets every ray through in one place: the sun's and the lamps' light comes through,
    tinted or not, and the global illumination's rays and the camera's pass as before. Only a reflection's rays meet them
    (`RayAlongReflection`, `TraceReflectionRay`): by chance, as much as a pane's cover (`Base_Tex_LIN`, red, or alpha in a colour
    texture) by its table's row gives, a ray reflects as from a mirror, and otherwise goes on, through at most `MostPanes`, with
    nothing behind the pane taken for a copy of it. A mirror reflects all of a ray: it has no row of the table, and its mask is
    on the second texture coordinates, which the scene does not keep. So glass shows in reflections, and mirrors in each
    other. The rays leave out glass's tint.
  - **Ray Reconstruction** (`glass::guideReconstruction`). Most glass draws no depth, so the motion, made from depth, moved a
    pane's detail as what is behind it, and with the guides of that too its cracks melted away in motion and came back
    once still (the player's hospital window, 2026-10-31: the same pose settled, then arrived at in half a second, 0.6 m to
    the side; the same held-still turn of 0.08 radians as a control, which depth does not change). Most of that detail is the
    traced reflection's: with the game's reflection in its place, `path_traced_reflections=0`, the pane has a third of it
    and keeps 87% moving. Where a pixel's glass covers at least `DominantCover` of it, 0.1, its depth, motion, normal,
    roughness and reflection hit distance go in place of what is behind, as NVIDIA's guide (3.4.1, 3.4.9) asks of a window.
    In one scene, detail kept moving went from 45% to 71%, off the glass 85%, and the difference from settled from 0.0114 to
    0.0073; turning stayed at 71 to 72%, as a wrong depth predicts. Cover 0.5 keeps less than 0.25, and 0.1 and 0.05 are the
    same. Blending the albedos too gained no more than repeat runs differ by (0.0074 to 0.0070) and takes three textures, so
    they stay as they are. The gap left to 85% is unexplained; the reflection's noise is the suspect, as neighbouring pixels
    differ by about four times its mean.
  - The glass with `Prim_TppFxGlassMat*` and `tpp3dfw_MetalicBacteriaGlass` are
    no part of this: none drew in 205 F10 records.
- **Hits** (`HitMaterialOf`, `gi_hit_light.hlsli`). The G-buffer's shaders (`fox3ddf_blin` and its family) take a
  surface's look from three textures and one number, and the scene reads the same from the model file: the
  material parameters `SpecularMap_Tex_LIN` (`g_tex_srm`: red the specular rate, green the roughness, the G-buffer's
  `rt2` y and x), `NormalMap_Tex_NRM` (`g_tex_normal`: x in alpha, y in green, z what is left, on a frame of the
  vertices' normal, their tangent and `cross(normal, tangent)` times the tangent's w) and the vector
  `MatParamIndex_0` (x its row of the game's material table, `rt2` z, `cb4[0].x`). FMDL's vector parameters: block 4's
  count at `+7` and first at `+10`, block 7's entries as a texture's (name, then index), each index four floats into
  section 1's part 0. The textures are copied as the base textures are (`gi_textures.cpp`), as data, not sRGB. Each
  row of the table becomes its specular colour at normal incidence in the guides' pass (`g_MaterialSpecular`,
  256 x 1: the row's colour times the Fresnel table at a cosine of 1, as the sun pass and the guides work it out),
  which the SRM's rate scales. The vertices' normals and tangents (usages 2 and 14, four halves each) go into the
  pool after a mesh's other bytes, 16 bytes a vertex (`GIGeometry::normalOffset`); `Skin` turns a character's with its
  pose. Normal maps and the SRM are read at the level of detail of the ray's cone; the reflections' cone spreads
  with the surface's roughness, so a sharp reflection reads sharp textures. Only the reflections read all this: to
  the global illumination's diffuse rays a hit stays a matte surface of its triangle's normal, as the materials cost
  them 2.5 ms a frame and showed nothing (9.7 ms against 7.2, 2026-10-30), and the light cache stays diffuse.
- **Self-lit meshes** (`SampledEmitterLight`, `gi_hit_light.hlsli`). Nothing in the G-buffer glows. What does is drawn
  forward by `fox3dfw_constant*` (a bulb, a sign): its texture times the vector `SelfColor` times the x of
  `SelfColorIntensity`, times the exposure, in the game's light units (seen 80 to 221,182, with the sun about 20,000).
  The scene took these for glass and dropped them. A mesh whose colour times intensity reaches 10 is now
  `MeshRole::Emissive`, whatever its alpha and shadow bytes (`0x10`, `0x25` on most), but for an additive one
  (alpha bit 0): those are glow cards around lamps, four vertices, no surface. The sky's dome, the moon (intensity 1)
  and a mesh painted black fall below it. Self-lit meshes give light and take none, and rays towards lights pass them
  (`GI_GEOMETRY_EMISSIVE`, with `GI_GEOMETRY_NO_SHADOW`). While they give no light, without path tracing or with the
  slider below at 0, every ray passes them, as a black hit would dim what is behind. A placed mesh within 200 m of the
  eye is an emitter: the 64 strongest at the eye go in the frame's constants (`GIEmitter`), each with the reach of its own light past which a
  surface does not consider it. Their diffuse light is sampled directly at the pixel
  (`TraceLight`), at the rays' hits (`HitOwnLight`) and in the cache's cells (`HitDirectLight`): one chosen by its
  light there (weighted reservoir sampling), a triangle of it and a point on that, with a shadow ray; it is an
  unbiased estimate. A diffuse ray that meets an emitter takes none of its light, as that would count it twice; a
  specular ray always does, which is how it shows in a reflection. A self-lit mesh left out of the 64, or on a
  character, gives its light to the diffuse rays that meet it. The intensities are set for how a bulb looks, and
  rooms are lit by the lamps placed beside them: a mesh lights others with at most the sun's radiance
  (`MostEmission`), and the menu's **Self-lit surfaces** (`path_traced_emission`, 1 by default, 0 to 4) scales it.
- **The sky** (`sky_capture.*`). The sky's light is nine numbers, without a horizon, clouds or the sun's glow, so a ray
  that got away reflected a smooth blur. The sky reflection map cannot stand in: `TPPSunLightReflectMap` fills it
  from baked cubes (`g_EnvSpecCubeTexture`), and the sun pass reads two levels of one rate from it. So the game's
  own sky draws are drawn twice while path tracing runs: `Sky_Draw_*` (or the physical sky's shader in its place), then
  `tpp3dfw_constant_sky` and `tpp3dfw_generativeClouds` (`GamePass::SkyDome`), each straight after itself into one
  face of a 256 x 256 cube (`R16G16B16A16_FLOAT`), a face a frame, with the depth off and every state put back.
  Their shaders read everything of the camera from constants, so a face takes copies with the camera turned to it:
  `b2` (`m_view`, `m_projection` a right angle across without the jitter, `m_projectionParam`, the size in
  `m_viewportSize`), `b0` (`m_renderInfo`, the fog's screen position), `b3` (`viewFar`, which only scales, and
  `clipPlaneOfView` off), `b5` (`m_viewWorld` taken from the game's view to the face's) and `b7`, `cVSWork`
  (`m_matrix[0]` clip to view, `m_matrix[1]` view to world). View space is x right, y up, z forward. Faces are
  turned as Direct3D's cube faces are, so a ray's direction samples the cube as it is. At the next kick the last
  frame's face is decoded (the scene's values are sRGB encoded, past 1 too), capped at 4 and divided by the exposure of
  its frame into the rays' units, in a shared cube. Reflections read it (`SkySeen`); the diffuse rays keep the sky's
  light, the level the game's ambient is tuned to. The fog these shaders read is the main view's, sampled at a
  face's screen position: a small error far from the view. Until all six faces are drawn, and two seconds after the game
  last drew a sky, reflections take the sky's light. It costs a few sky-sized draws at 256 x 256 a frame.
- **Rain.** In rain the sun pass has no sky reflection (`SunLight_DL_CS`, `SunLight_DL`): the resolve
  (`DR_TonemapRainFilter`, `DR_TonemapRainFilter_NoIR`, from their disassembly, 2026-10-30) adds one on wet
  surfaces instead, after `occlusion x (diffuse x albedo + specular)`: a cube map (`t1`) along the
  reflection, blended towards its alpha by the rain's floor normal map (`t0`), squared and scaled
  (`cPSMaterial` `c1`), times `(1 - |NV|)^2 + 0.25`, the fade, the sky-facing screen (`t2`) and the
  specular intensity (`inSpecular`, `t3`'s y, its only read). With the traced reflections in the specular
  light, `t3` is unbound for the draw (it reads 0), which takes that reflection alone out
  (`GameShader::hasRainReflection`, `indirect_light_pass.cpp`).
- **The sun** (`PATH_TRACED` in `sun_light_pass_shaders.hpp`): the sun pass's own shader, lit by the rays'
  shadow alone, without the shadow map or the contact shadows, which it does not trace (and the rays leave
  nothing to the map: no shadow range for untextured cut-out meshes), inside the game's shadow range with
  its clouds' shadow and height fade (`CLOUD_SHADOW`), and without the game's specular shadow floor and sky
  reflection. Its specular stays the game's tables (`g_lut2` by the halfway vector's angle, the roughness and
  the anisotropy, `g_lut1` for Fresnel), not GGX: measured against the frames of 2026-10-30 on Snake's suit
  (roughness 0.5 to 0.9), the game's lobe at the same roughness is up to 1.8 times GGX's at its peak and a tenth
  of it on the flank, so GGX spread the highlights into a haze: the fine creases and fabric went, and the
  sortie screen's floor took three times the specular light. The sun's rays are traced while path tracing
  runs, whether or not the ray-traced shadows are on.
- **Point and spot lights** (`TraceLampLight`, `direct_light.hlsl`), in place of the game's light shaders:
  each light's light as they work it out (`LocalLightReaching`: its falloff, a spot's cone, the game's
  volume for it; a masked spot's mask; the light through leaves, the game's `LN_trans`; its diffuse and
  specular scales, `cPSLight` `c8.y` and `c8.x`, 1 for the latter where its shader has none, as the
  ray-traced lights draw them), times the exposure as theirs (`falloff x c4 x m_exposure.z`), with their
  specular as the sun's: the game's tables, towards the light as a disc of its size (`c4.w`). Until the first sun
  pass has given those tables, the game's shaders draw the lights, as a session can start where no sun shines. A
  pixel picks two lights in proportion to their light there without shadows (weighted
  reservoir sampling over all the frame's lights, white noise) and traces a shadow ray to a point on each
  one's disc, as the ray-traced lights' rays leave (hair's share, the faces that cast); every light is
  shadowed in full, as the ray-traced lights shadow them. At the draw point (`path_tracing::addLampLight`,
  from `local_lights.cpp`) one full-screen draw adds their diffuse and specular light to the light
  targets, so the skin filter, the resolve and the exposure see it, in place of each light the kick traced
  drawn again. Path tracing takes the lights' records whether or not the ray-traced lights are on; on its
  own it takes the game's lights only while it runs.
- **ReSTIR DI**, tried on 2026-10-30 (Bitterli et al. 2020: 8 random candidates, a visibility ray, the last
  frame's reservoir where the pixel was, 4 neighbours, the biased 1/M weights): shadows lagged in game. A
  light its ray found blocked went on to the next frames with no weight but up to 20 samples, so where a
  blocker moved away its light came back over about 20 frames. Removed.
- **What it does not trace.** Beams stay the game's, drawn with their shadow map (see "Ray-traced lights",
  "Beams"). A light the kick did not take (recorded after it, left for another frame's camera, or past the
  128 the rays take) is drawn again as before, unshadowed: in the sessions of 2026-10-30, 195 ten-second
  counts had none but one (17 left for another frame's camera at a cut).
- **Masks.** A masked spot's mask (the game's `t4`) goes into a tile of an atlas of
  `GI_LAMP_MASK_TILES` x 256 x 256 (`path_tracing::packMasks`, `MaskCopy`, sampled as the game samples it,
  bilinear and wrapped), each mask once a frame; past the atlas's tiles a spot is lit unmasked, which the
  log says. The projection is the shader's (`cPSScene` `c16`, `c17`, `c19` on the light pass's view
  position, `uv = xy / w x 0.5 + 0.5`, v flipped), taken into the world through that view
  (`light_choice::maskRowsInWorld`).
- **Guides.** Besides Ray Reconstruction's, the kick writes the specular colour with the translucency, and
  the light through leaves' floor (the translucency times the material's) with F0 and the anisotropy (both
  `R8G8B8A8_UNORM`). `g_lut2`, 64 x 16 x 16, goes to Direct3D 12 as 64 x 256, its slices one below the other
  (`SpecularTableAt`, `direct_light.hlsl`).
- **What stays the game's.** Its shadow maps: 85 of its pixel shaders sample them (its lit particles,
  glass, rivers and liquids, decals, forward models, the terrain's self-shadow), and the volumetric fog the
  sun's for its light shafts. Its screen-space reflections' march, for water. Its probe light in
  the diffuse light target, which its exposure measures.
- **Cost** (2026-10-30, 2228x1254 rendered for 3840x2160): the light's line in the log,
  which holds the reflections and the lights, 4 to 5.7 ms, against about 2.4 ms with Ray Reconstruction
  alone. The budget has its own line: the reflections, the lights and adding their light.

## HDR

With `hdr=1` in `PhantomFX.ini` (a restart applies it) the game outputs HDR10 (`src/features/hdr/`).
The game tonemaps early, in the deferred resolve (`TppTonemap`, `cPSMaterial` c0 = a, b, c, usually
4.87, 0.6, 0.453: x up to b, then b + c - 1 / (a (x - b + c)), which an 8-bit target clamps at 1,
reached at x = 4) and inline in 23 forward shaders (sky, clouds, oceans, wormholes, sand wind), and
everything after worked on that image in 8 bits. With HDR the scene keeps its light up to the
grading, as RenoDX's mods do, and the grading and output follow RenoDX's and Luma's defaults.

**The float scene in SDR.** On a supported build the scene keeps its light in SDR too (since
2026-10-30, issue #54): all of the below but the swap chain, the light the grading gives back and
the HDR output. In 8 bits, smooth fog showed steps of one and two levels, a few of three (measured
in a screenshot at 3840x2160, DLSS Quality); the game hid them with an 8x8 dither before the
upscaler, which the upscaler made a grid (issue #28, see "Dither"). The grading is the game's own
and the sky keeps its light (see "TppTonemap out"), so below the tonemap's knee the image is the
game's but for the steps; above it, glass, particles and the reflections blend over the light before
the tonemap rather than after it, as with HDR. The image is rounded to 8 bits once, by
GammaControl's write to the game's `R8G8B8A8_UNORM_SRGB` back buffer, with up to half a level either
way of white noise (PCG of the pixel, one value for the three channels), which turns the steps left
into grain too fine to see. It costs what HDR's scene costs: about 0.3 ms at 4K on an RTX 5080
(0.03 ms encoding the scene, 0.27 ms decoding its copies). DLSS and FSR run in their HDR modes on
it. If the GPU cannot read `R16G16B16A16_FLOAT` through unordered access views, the scene stays the
game's 8-bit one.

- **Swap chain** (HDR output only). `create_swapchain`: `R10G10B10A2_UNORM`, flip model
  (`FLIP_DISCARD`, at least 2 buffers). At `init_swapchain` the colour space is set on the native
  swap chain (ReShade's proxy skips `SetColorSpace1` once an add-on changed the format):
  `G2084_NONE_P2020` when the output's `DXGI_OUTPUT_DESC1` says Windows shows HDR, otherwise
  `G22_NONE_P709` and SDR output. ReShade's runtime is told the same with `set_color_space`. The
  game's `R8G8B8A8_UNORM_SRGB` views of the back buffer become `R10G10B10A2_UNORM`. With frame
  generation the presenter makes its swap chain from the changed description, and the hudless
  texture is `R10G10B10A2_UNORM`.
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
  add, mul, add`, the movc's literals 1/0 and 0, the div's -1), is patched: the movc's second
  literal takes the first's, so the linear part is always kept, and the curve's multiply becomes an
  `and` with the zero beside it (a multiply would keep the curve's pole as NaN). With HDR output the
  three sky draws keep their clamp to 1 after it (`min` with a literal of ones; no other shader has
  one): above it the sun's sprite shows as a ring around a dimmer centre (issue #47), and the
  physical sky clamps the same way for the sprite it draws. In SDR both are lifted to 65504: the
  grading's TppTonemap and clamp to 1 then make the game's image, where a clamp to 1 in light would
  show all of the sky's light from 1 to 4 as TppTonemap(1) = 0.81, never white. 27 shaders: the 4
  resolves and the 23 forward ones. The patched code gets its checksum again (`resign`), and
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
  game's grading as its shader does it (bloom screened in gamma 2, the 16x16x16 LUT), then, with HDR
  output, RenoDX's `UpgradeToneMap`: the luminance the tonemap took is added to the graded luminance,
  and the graded colour scaled to it. Below the knee (0.6) the image is the game's. Alpha keeps the
  game's luma.
- **DLSS** runs with `IsHDR` on the linear light, after the depth of field and before the grading;
  RCAS sharpens through x / (1 + x).
- **Output.** `GammaControl` draws with the add-on's shader: the game's brightness setting as it
  applies it, the 0 to 1 range decoded with gamma 2.2 (the SDR look on 2.2 displays, Luma's and
  RenoDX's default) and above it with sRGB, then RenoDX's Neutwo on the brightest channel, which puts
  100 times the game's white at the display's peak (`hdr_peak_nits`, 0 for the peak the display
  reports) and leaves values well below it nearly unchanged, BT.709 to BT.2020, the game's white at
  `hdr_paper_white_nits` (203), and PQ. Without HDR on the display: sRGB, clipped at 1. In SDR: the
  brightness, then linear light for the back buffer's sRGB view to encode, with the dither above.

Not handled:

- Particles or glass that blend with alpha above 1 would darken their background, as the 2D pass did;
  none were seen.
- The reflections' own targets are 8-bit, so reflected highlights stop at the game's white.
- Motion blur blurs through the add-on's float targets in place of the game's 8-bit ones (see "The
  UI at the output size"), so moving highlights keep their brightness.
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
then only checks a flag on each full-screen draw. **Lamp contact shadows** give every point and spot
light a short shadow of its own, marched through the depth buffer inside the light's own draw
(`drawMarking` in `src/features/lamp_contact_shadows/lamp_contact_shadows.cpp`, from the local light
pass), so they are a toggle of their own: the sun's is one Bend trace a frame, the lamps' a march per
light, in the light's own volume draw.

- **Replacement shaders** (`src/passes/sun_light_pass_shaders.hpp`). `SunLight_DL_CS_ES` and
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
  jitter; the constant capture keeps them, as for the ray tracing) and looked up by the buffers the draw
  binds. Depth is reversed (near 1, far 0, the border colour). Bend's starting values: 60 samples,
  surface thickness 0.5%, bilinear threshold 2%, contrast 4, early-out on.
- **Foliage casts like everything else.** The trace reads its casters from the depth buffer alone,
  as Bend's does, so grass, bushes and tree leaves both cast and receive. Its own measured effect
  is the reason it is worth having: a clump's blades shade each other, which the sun's shadow map
  resolves only in cascade 0 (3 mm texels) and the rest of the range does not. An earlier build
  skipped the foliage material (43 in the specular target's `z`, as in "GTVBAO") to keep clumps out
  of it; the skip is gone so the term matches the screen-space shadows in `renodx-mgsv`. If a
  clump darkens too far, **Contact shadow thickness** and **Contact shadow contrast** set how deep a
  sample casts and how hard the transition is.
- **Draw.** The game's compute bindings in the slots the trace uses are saved and restored around
  it; the replacement pixel shader, `t7` and `b6` are bound for the game's draw, made by the
  add-on, and the game's shader, `t7` and `b6` restored. The options apply from the frame after
  the next present, so both parts of a frame's sun pass see the same. A sun pass without its depth or constants, or an error, keeps
  the game's draw and logs the reason once.
- **The lamps march instead.** Bend's compute shader costs a dispatch over the light's whole screen
  rectangle at the render size, 120 samples a pixel, and a second draw of the light's volume to put
  the result in the light targets' alpha: ten lamps in a night base turn that into a frame-rate drop.
  The lamps' marking draw now marches the depth buffer itself, the shape `renodx-mgsv`'s screen-space
  term has (`src/features/lamp_contact_shadows/lamp_contact_shadows_shaders.hpp`): the ray leaves the
  surface the pixel shows, steps towards the light, and counts the depth reads with a nearer surface
  in front of it. The reach is `Contact shadow reach` in pixels of the render size, capped at 6 m
  and at the light (`min(reach, 6 m, light distance)`: nothing past a light shadows it), with a step
  every two pixels and the steps crowded near the receiver; a caster
  counts within `max(0.25 m, 1 cm a metre of depth)` of the ray, which keeps layered gear and
  overlapping terrain patches from reading as a wall in front of their own surface, and the march
  stops once the shadow is as dark as `Contact shadow contrast` allows. A pixel outside the light's
  reach (one depth read and a distance) or past the fade is lit without marching, and a lamp wholly
  past the fade keeps the game's draws. The sun keeps Bend's trace, which costs one to eight dispatches
  for each recording of the depth it reads, and its look is the reference the lamps' march was matched
  to.
- **Far faces only.** The game's marking draw culls none and its lighting draw culls front faces
  (frame logs, 2026-10-29: every one of 1,166 lights). The shadow draw drew with culling off too, so a
  closed volume ran the march on both faces and threw one result away; the game's own marking state
  would do the same. It draws the far faces instead, as the lighting draw does: it covers the pixels
  that read the alpha, with the camera inside the volume too, at half the pixels.
- **Four texels a gather.** The march took one depth a step. A `Gather` returns the four texels inside
  a two-pixel square for one instruction - the ground two steps cover - so a pair of steps is one
  request and four comparisons: half the memory instructions, twice the samples. The window a caster
  may sit in is compared in the buffer's own depth, `A + B / z` from `cPSScene` `c21`, so the two
  conversions a pair costs are shared by its four texels instead of one a texel. The sampler it reads
  through is the add-on's, bound at `s0` for the draw and put back after.
- **Pixels the light cannot light are not marched.** A surface facing away from a lamp takes none of
  it: the game's shaders clamp `dot(normal, light)` at zero and scale both the diffuse and the
  specular by it, so the light adds nothing there and the alpha the march writes is never read. The
  march returns after one read of the G-buffer's normals (`t0`, decoded as `lamp_soft_shadows_shaders.hpp`
  decodes it) when that dot is zero or less. Where the marking draw leaves `t0` unbound the shader
  marches as before, and the options say which it is. A lamp whose reach cannot touch the screen - its
  sphere off it, or wholly behind the camera - keeps the game's draws, which is `reachesScreen` in
  `lamp_contact_shadows.cpp`.
- **A lamp's reach comes from its falloff** in `cPSLight` `c6` (from the disassembly, `d` the distance
  plus `c6.x` for points): `SSLighting2_*` light `1 / d^2 - d^2 c6.w`, zero at `c6.w^(-1/4)`;
  `Lighting_*_NA` light `(1 - d^16 c6.z) / d^2`, zero at `c6.z^(-1/16)`. The march takes it as the
  distance past which a pixel is lit without a step.
- **Diagnostics.** "Sun contact shadows only" draws both parts with the far shader: the sun is then
  shadowed by the trace alone.

Checked in game at 3840x2160 with DLAA and frame generation, in Afghanistan at dusk and in
daylight: the shaders compiled and both sun shaders were identified at start-up, the trace ran for
each frame's recording (a single dispatch, with the sun off screen), the lit image looked as before,
and contact shadows showed under Snake's boots, in grass and around rocks. With the trace alone,
characters close to the camera keep only the shadow near their feet.

- **Contrast and reach.** Two settings shape both, under **Contact shadows** with their
  switches: **Contact shadow contrast** (0.5 to 4, 1 by default) scales the trace's distance-to-shadow
  window, so higher is darker and harder and lower is a soft grey wash, 1 being the trace itself;
  and **Contact shadow reach** (8 to 120 pixels of the render size, 80 by default) is how far along the ray the march runs. The sun's reach costs one depth read group when the
  shader's `SAMPLE_COUNT` went from 60 to 120, and the march stops at the setting, so a shorter reach
  is cheap; the last eight samples of it fade out as Bend's last eight do. The lamps take the reach as
  their march's step count, a step every two pixels. It is in pixels, so at 4K
  the same reach covers half the world distance it does at 1080p.
- **Thickness.** Bend's 0.5% window was a constant. It is a setting now (`SurfaceThickness`, 0.2% to
  2.5% of the depth between a sample and the far plane): how deep a surface may sit behind the
  receiver and still count as a contact. RenoDX exposes the same dial per light type for the sun. The
  lamps' march keeps RenoDX's own, a fixed 5 mm (`ThicknessMetres` in `lamp_contact_shadows.cpp`): the
  share grows with the distance until, past 1%, it reaches the bound a caster may sit in and leaves no
  window at all, so the term died silently for receivers past 10 m, and below that raising it narrowed
  the march where it widens the sun's trace. The setting now shapes the sun's contact shadows alone.
- **Distance fade.** Bend's shader has no fade, so a contact shadow at 100 m was as sharp as one
  underfoot. The trace now fades its result with view depth (B and A from `cPSScene` c21 give the view
  Z from the depth it already reads), full strength out to 60% of the setting and nothing past it.
  RenoDX fades for the same reason: past that range the shadow map has the detail and the trace only
  has guesses.

Not handled:

- The trace reaches 120 pixels at most: near characters fill much of the screen, so only
  the start of their shadow is found. The shadow map gives the rest.
- Forward and transparent objects, drawn after the sun, neither cast nor receive the trace.

## Lamp soft shadows

With **Lamp soft shadows (PCSS)** on, a point light whose shader is `SSLighting2_Point` draws with the
add-on's replacement (`src/features/lamp_soft_shadows/lamp_soft_shadows_shaders.hpp`) instead of the game's, and its own shadow map is
filtered with contact-hardening soft shadows: sharp where the object meets the light, wider the
further the shadow falls, as a real bulb's is.

- **Why the shader and not the blend.** The add-on's other local-light work (the screen-space shadow,
  the ray-traced lights) only *scales* the game's light through the targets' alpha. A soft shadow
  cannot be added that way: the game's four comparisons have already decided the pixel is in shadow,
  and scaling a zero leaves zero. The filter has to replace the comparison, so it replaces the shader.
- **The body is the decompiled one** (renodx-mgsv's `SSLighting2_Point` port, itself from 3Dmigoto),
  kept in its register form: rewriting 500 lines of register code into named variables risks a
  transcription error that no test here would catch, and FXC folds the redundant moves away.
- **Dual-paraboloid atlas.** The point light's shadow map is two hemispheres side by side; a texel
  covers a different world size at a hemisphere's centre than at its rim, so the penumbra uses a
  compromise scale, `3 x distance / hemisphereRes`, as renodx-mgsv does. The search radius is
  `g_lampSearchTexels`, the penumbra's growth `g_lampHardening`, its widest `g_lampSoftness` (10 cm),
  and how dark its core gets `g_lampDarkness`.
- **Point and spot.** Two shaders are rebuilt: `SSLighting2_Point`, whose dual-paraboloid atlas needs
  the compromise scale above, and `SSLighting2_Spot`, whose map is a plain projection, so its penumbra
  follows `m_shadowProjection` the way the sun's cascades do. Both are the quartic, specular,
  shadow-mapped variants; the others (no specular, no shadow map) keep the game's shader, and a light
  the game draws *without* a shadow map -- most of its point lights -- has no shadow to soften at all.
- **Identified by checksum.** The local light pass (`src/passes/local_light_pass.cpp`) knows every
  point and spot light shader by its checksum (`game_lights.hpp`), and the soft shadows replace the
  two kinds above at the lighting draw (`SSLighting2_Point`, `afbc12167037e4e0d653da184c7e3e82`;
  `SSLighting2_Spot`, `ac0947110cb0f17f4ffe930a6a11617d`). With the lamps' contact shadows on as well, that draw also takes
  their blend. The log says "the game draws a ... with the soft shadows" once for each shader that
  draws.

Not handled yet: the same lights without specular, and the masked spots, which have their own shaders.

## Global illumination

The indirect light that replaces the game's baked probe light (`src/features/global_illumination/gi_light.cpp`,
`gi_light.hlsl`, `gi_denoiser.cpp`, `src/passes/ray_tracing/shaders/gi_hit_light.hlsli`), rebuilt on 2026-10-24 the way path tracers with a
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
  render pixel of its 2 x 2, by depth (`DepthPickOf`): the nearest on one checkerboard colour, the
  farthest on the other, as half-resolution effects downsample depth. With the top left, as first built,
  a wire, a fence or a grass blade under a pixel wide had it one frame and what lay behind the next as the
  jitter moved them, and the denoiser started their light over every frame with the camera still. The
  depth pick holds where its surface is the one the light grid held there last frame (`PickOf`: within 3%
  of the view depth, through the camera's motion); elsewhere the render pixel of the 2 x 2 whose surface is
  takes its place, if one is, so a light pixel stays on its surface as the jitter moves an edge across its
  2 x 2. With the depth pick alone the denoiser started the light over along edges every frame, about five
  times the young history in grass (2026-10-29, NRD's validation layer). The choice goes to `g_Light_Pick`
  for the frame's later passes (`RenderPixelOf`). Checked against the 2 x 2 light pixels around where it
  was, as NRD's own test takes them, it looked worse in game. The
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
- **The cache** (AMD's two-level hash grid of GI-1.2): the bounce hits' cells take up to four of their
  hits' light a frame (`PopulateCells`: sun, sky and the lights, each through a shadow ray), blended
  1/16; tiles nothing touched for 50 frames go. 30% of the hits trace on to a third hit
  (`PopulateMultibounceCells`), whose cell's light, direct and indirect, becomes their cell's indirect
  light a frame later (`UpdateMultibounceCells`): the cache feeds its own light back, a bounce more each
  frame, as SHaRC's and DDGI's caches do, and a bounce keeps only its albedo's share, so the sum
  settles. A ray that gets away counts as bringing no light: the sky is in the direct light already.
  Until 2026-10-30 it was GI-1.2's as AMD wrote it: the third hit's direct light alone, three bounces in
  all, averaged over the rays that hit, which made it too bright by the share that got away (1.4 times
  in the median of the logs, twice in open ground). Compared in game the same day, the two changed the
  light little: the G-buffer's albedo averages about 0.26 (linear, a 4K frame), about 80% of the
  multibounce rays hit, so a bounce keeps about a fifth of the one before; the bounces after the third
  added about what the third had been too bright by (+0.2% in the sun, none seen in a dim room).
  Bright, closed rooms, where few rays get away, gain the most.
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
  G-buffer pack puts in the normals' alpha), and 2.5D motion: the camera's (`CameraMotionOf`: the pixel's
  unjittered clip position through the last frame's clip from this one's, built in double from the
  projections and views apart, `camera::previousFromCurrentByParts`; through its world position and the
  game's float projection-views it was off by up to 0.19 render pixels near the camera on a still frame,
  changing with each jitter, their translation holding the camera's place in the world, 2026-10-29), or a
  moving mesh's from the game's velocity where it drew one, and the view depth's change: the camera's, and
  a moving mesh's from the last frame's view depth where it was when that lies within 10% of its own (as
  if it stood still, the change was off by the camera's movement where the camera follows it, and the
  player's light started over every frame). The camera goes in with the engine's x
  mirror moved from the projection into the view (as for Streamline), and, as NRD requires, without the
  upscaler's jitter (c28 taken out of the projections, the motion without the jitter's change, the jitter
  apart as `cameraJitter`): with it in, grass and fine detail shimmered as the jitter moved them, most
  visibly with DLSS preset M. Foliage is NRD's strands (`strandMaterialID`, 1 cm
  thick), whose history survives far more depth change: the game sways its grass and bushes without
  velocity. History: half a second at the real frame rate (smoothed as NRD asks), a fast history of a
  tenth of a second that the long one is clamped to, three frames rebuilt after a reset, anti-firefly on;
  a camera cut (`camera::cameraCut`) restarts it. Tried on 2026-10-25 and left out: REBLUR in place of
  RELAX (blurrier), a wider disocclusion threshold, and the render pixels' light without their own normal
  (neither steadier). The light goes in in the game's units over 1024 (`LightScale`), about 0.001 on
  screen by day, far under the 0 to 250 NRD's README asks for. Scaled into that range (x256) it blotched
  in the shade (2026-10-29): at x1, RELAX's fixed floor of 1e-4 on its a-trous luminance sigma
  (`RELAX_Atrous.cs.hlsl`) takes over in the dark and blurs more there, and a looser
  `diffusePhiLuminance` (4 or 8) at x256 did not make up for it. Kept at x1. NRD's validation layer
  (`OUT_VALIDATION`, its checks of the guides and motion and the frames of history) is the **Global
  illumination denoiser (NRD)** debug view; its texture is always made, at the light's size.
- **The render size** (`CompositeLight`). The four light pixels around each render pixel, weighed as a
  bilinear filter would times how alike their view depth (within 5%) and normal (cosine to the 8th) are,
  else the one nearest in depth; each resolved against the render pixel's normal
  (`NRD_SH_ResolveDiffuse`), scaled to keep the light it has at its own normal. Irradiance over pi times
  the exposure, as before. NRD's own resolves in its place (2026-10-29; its spherical harmonics upsampled
  to the render pixel, then `NRD_SH_ResolveDiffuse` or `NRD_SG_ResolveDiffuse`, with and without
  `NRD_SG_ReJitter`): more of the normal maps' detail (+15 to +53%), the light 10 to 20% brighter or
  darker by the scene, harsh on faces; left out for the smoother look.
- **Albedo learning** (`LearnAlbedo`): one light pixel of each 16 x 16 a frame.

ReSTIR GI's spatial reuse was tried on 2026-10-24 (each light pixel resampling its own ray's light and
six neighbours' on its surface, with the Jacobian and a visibility ray for a chosen neighbour's sample,
this frame's samples only): in game the light was far less stable than without it. Picking one of seven
samples makes the noise come in clumps that move every frame, which the denoiser smooths far worse than
noise a pixel at a time (Epic's MegaLights left ReSTIR out for the same reason). Removed.

Next step planned, once measured as needed: a history confidence for the denoiser from samples traced
again with the last frame's random numbers (NRD's `IN_DIFF_CONFIDENCE`).

With DLSS Ray Reconstruction denoising, the light takes a sample a render pixel and goes out without
NRD (see "Ray Reconstruction").

### What a kick traces

A kick can run six traces on the ray-traced scene: the lamps' shadow rays, the lamps' light, the sun's rays,
the light, the reflections and, later in the frame, the glass's reflections. An effect's state does not say
which: under path tracing the sun's rays run while the ray-traced shadows are replaced, and the lamps' shadow
rays run until the game's tables arrive, while the ray-traced lights are replaced.

`kick_traces.*` holds the one answer, in two pure steps. `askedBy` takes what the effect plan asks of the rays
for a frame. `tracesOf` adds what the kick finds: whether Ray Reconstruction denoised, a sun, lights, the
game's tables and the glass's targets. The kick makes the textures of what it traces, `gi_renderer` runs
`Frame::traces`, and the threads recording the next frame read the last kick's answer
(`ray_tracing::isPathTracing`). Only the path-traced question comes earlier (`isPathTraced`), because the
kick takes the lights and sets up the glass for it.

A trace owns what it needs on both Direct3D sides, behind `trace::Trace`, and `trace::inOrder` lists them in
the frame's order. The kick calls `gather` on each, on Direct3D 11: it makes the trace's results. The frame
then calls `prepare` on each, for its memory and last probe, and `bind` and `record` on those the kick runs.
`release` gives its Direct3D 12 memory back when the rays stop, and `detach` the rest.

A trace declares each texture it shares in a `SharedRow`: its format and slot, whose kind says read or write. `SharedBindings` opens it
in Direct3D 12, puts it in the frame's tables, moves it out of the common state for the frame's passes and back,
and marks a `SharedOutput` written once the frame is submitted. A `SharedOutput` keeps its own lock, since the kick
and the threads recording the next frame use it at the same time.

So far the lamps' shadow rays are a trace (`lamp_shadow_rays.*`); the others are recorded by `gi_renderer` itself.

### Kernels and their slots

A kernel's HLSL is the one home of its slots: the `register()` lines, each in the file of the trace that owns it. A
kernel's one home is its `add_gi_kernels` line in `CMakeLists.txt`. The build reads the rest from the compiled kernels,
so nothing is typed twice.

`tools/kernel_bindings` reflects each kernel's DXIL with DXC's own reflection (`dxcompiler.dll`, beside `dxc.exe`) and
writes three headers into `build/<preset>/gi_shaders`:

| Header | What it holds | Who includes it |
| --- | --- | --- |
| `gi_bindings.h` | Each slot by its HLSL name without `g_`, in `namespace binding`: `binding::Depth`, `binding::Light_Noisy.at(1)` | `gi_d3d12.hpp` |
| `gi_kernel_list.h` | The `Kernel` enum, in `add_gi_kernels` order | `gi_d3d12.hpp` |
| `gi_kernel_bytecode.h` | `Bytecode`, every kernel's DXIL in that order (about 8 MB of headers) | `gi_renderer.cpp` |

To add a resource: declare it in the HLSL, use it in a kernel, then bind `binding::Name`. DXC drops what no kernel
uses, so until then the name does not exist. To add a kernel: add it to `add_gi_kernels`.

The build stops when a slot has two names, a name has two shapes across kernels, or a kernel has no bindings. Each kind of
binding has a type of its own (`gi_slots.hpp`), so `Descriptors::texture` does not compile with an output's slot, and `at()` past the
end of an array does not compile either.

Not checked: a view's format against the HLSL (a write-only `RWTexture2D<float4>` on a UNORM texture is fine, a typed
load needs `unorm`), and a slot left null at run time, which the optional inputs are on purpose.

The tool reads DXC's reflection and not the binding table DXC prints in each kernel's `.h`: that table is for people,
and runs its columns together (`UAVunorm_f32`).

### Smaller views

The rays trace the main view only, one kick a frame. The intro's mirror is a camera of its own, drawn
whole before the main view into a corner of the same targets (1080x608 in 2560x1440, `frame-22272`,
2026-10-26): its own sky light, upscale, lights, `SunLight_ES` and resolve (`DeferredRendering_VolFog`).
Until then its upscale kicked the rays too, so they ran twice a frame there, and each kick's denoiser
history was the other camera's. A pass whose viewport does not cover its render target
(`viewportFillsTarget`) is now such a view and keeps the game's light: no kick, wait or draw point; its
lights drawn by the game, unshadowed; no ray-traced sun shadow in its sun pass; and its resolve the
game's probe light with white in place of the occlusion, as the game's SSAO is skipped while the global
illumination runs (both cameras' SSAO passes fill their half-size target, so they cannot be told apart).
The rays still trace from the camera of the first main-camera draw in the last list run before the
kick (`gi_inputs`), not the frame's largest as the upscaler's main view: the diagnostic log counts the
kicks whose camera's viewport is smaller than the depth they pack, which a smaller view's camera would be.

## Ray-traced scene

What the global illumination and the ray-traced shadows trace (`src/passes/ray_tracing/gi_scene.cpp`, filled by the scene
contents in `scene_contents.cpp`), laid out as DXR scenes are. Rebuilt 2026-10-23 from one structure per
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
  first no larger than 512 pixels (a cut-out mesh's) or 256 (the others') into a shared texture of the
  typeless format, viewed as sRGB on Direct3D 12, and looks again every 300 frames for the game's
  replacement as finer levels stream in. Checked in game against the G-buffer's `t0`: 370 of 377 meshes'
  base textures were the very texture the game drew with; 6 were missing from the manager's streaming
  list (the array is walked since), 1 mesh's shader samples another texture at `t0`. On Direct3D 12 each
  texture has a slot, a descriptor in a range after the frame tables (`t0` on in `space2`, 4096), written when it
  arrives, a new one for a replacement, the old one free again once the GPU is past the frame; a texture no
  model uses is let go. They stay in the common state: the shaders' reads promote them. The copies are made
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
- **Hidden mesh groups** (2026-10-26). A rigid model has its mesh groups' states too, as a character
  does (`hiddenGroupsOf` in `engine_scene.cpp`: the same count and state bytes on the model object, a
  group hidden while any of bits 0x33 is set), and a placement carries the groups its model hides
  (`ModelInstance::hiddenGroups`, the first 64). One that hides any takes a structure of its model's
  other meshes (`ModelRecord::Variant`, a structure a set of hidden groups, made once the model's is, let
  go when no placement asked for it in the models' retention time), and the probes find a hit's mesh
  through it (`partOfGeometry`). Before, the hospital's entrance (`84a224c8a02fc625`) put all its meshes
  in the scene: the game drew its group 1 (meshes 0 to 11) and never its group 2 (12 to 30), rubble slabs
  that floated over its porch in the rays and cast blots on it from a lamp the game draws unshadowed (the
  lamp probe and the scene view). In two captures there, 11 placements hid groups and the game drew none
  of their 29 hidden meshes. F10's `models.txt` prints each mesh's group and each placement's hidden
  groups.
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
  by their alpha; its other forward meshes the main view alone shows (alpha bit 4 with shadow bit 0) stay out, and its
  glass is glass (see "Path tracing", "Glass and mirrors"). The helicopter's
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
  Since 2026-10-26 a rigid model's solid stand-ins (shadow bit 1 without alpha bits 5 or 7) stay out
  too: with the main view's own meshes in the scene they are offset, simpler copies the camera never
  sees, and blocked light where nothing was drawn. In the prologue's hospital a building's 25-triangle
  stand-in (model `84a1b19dea6952c1`, mesh 9) kept 80 to 100% of a room's main lamp, an unshadowed spot,
  off its wall 0.9 m from it (the lamp probe), and the scene view went red over the room: the camera's
  rays met stand-ins short of what the game drew. Cut-out stand-ins stay in, for the main-view cut-out
  meshes left out (the pylon's lattice).
  Since 2026-10-27 a solid stand-in is kept again where its model's main view shows cut-out meshes the
  scene leaves out (shadow bit 0 with alpha bit 5 or 7), as the only thing the rays have of them, and
  only for rays towards lights (`GI_GEOMETRY_SHADOW_ONLY`, `StopsRay`): every other ray passes it, and
  the eye's ray does not take it for a pixel's own surface. Mother Base's scaffolds' white tarps cast no
  ray-traced shadow: their sheets (model `84a07d3c8f5c1982`, meshes 3 to 8, alpha 32, shadow 1) were
  drawn in the main view only, their 72-triangle stand-in (mesh 12, shadow 2) into all four sun
  cascades, and neither was in the scene (an F10 frame, matched by the meshes' index ranges). The
  stand-in is a 33 cm slab around each sheet, its faces 16.3 cm off it: met by every ray, a sheet's
  global illumination rays would meet its inside, as a wall's met the faded-out shell (see "Model",
  `+0x1B8`). Of 110 shown rigid model files there, 4 had solid stand-ins and 3 of them, all scaffolds,
  main-view cut-outs; the hospital building has none (its left-out meshes are glass), so its stand-in
  stays out.
  Since 2026-10-27 every other rigid solid stand-in is kept too, for the sun's rays alone
  (`GI_GEOMETRY_SUN_ONLY`, `StopsShadowRay`). At Serak Power Plant a room's walls (model
  `84a3e8003e2406b0`, meshes 2, 4 and 11, shadow bit 0) and a cave's rock overhang (model
  `84a2b02d4835dd8b`, mesh 0) are drawn in the main view only, and their stand-ins (meshes 3, 5 and 12;
  mesh 1) into the sun's shadow maps: mesh 12's 180 indices from 1122 were drawn four times into the
  8192x8192 atlas, once a cascade (an F10 frame, matched by index range). With both out of the rays, the
  sun lit the room's lockers and the cave's walls where the game kept them dark. A lamp's rays still
  pass these stand-ins, as the hospital's kept a room's lamp off its wall. Should the sun-only rule
  mislead too, the exact rule is the game's per-draw pass masks: which shadow maps each mesh is drawn
  into.
  Open: a tarp on a fence (model `84a1bc5b4702c16b`) is a
  main-view cut-out (mesh 2) cast in the game by a flat 4-triangle stand-in (mesh 3, shadow bit 1) up
  to 25 cm off it, so its shadow rays start on the stand-in or meet it, in patches that change with the
  distance; taking a rigid model as its main view shows it, as a character is, would fix it, untried
  as it may drop a building's shell. The shadow probe lists every model at a character's place
  with all its meshes: each one's group and state, alpha and shadow bytes, whether it is shown and drawn
  (posed by a draw this frame), and the meshes left out.
- **Model files read once.** A model the game has not placed for 600 frames is let go and its file
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
- **Mesh roles** (2026-10-27). `scene_description.cpp` gives each mesh of a model file its role from
  its material where that says (glass, a mirror, a self-lit mesh), else from its alpha and shadow bytes, whether the file
  has bones and its model source, by the rules above ("Characters", "Vegetation"): solid, cut-out, leaf, hair, main view
  only, shadow only, or dropped. The
  role alone sets its row's flags. `tests/scene_description_tests.cpp` keeps the cases measured in the
  game as rows. Checked against the code before it for every alpha and shadow byte, rigid and skinned
  and from each source, every mesh had the same flags but two kinds. A rigid model's solid stand-in the
  main view draws too (shadow bits 0 and 1), beside main-view cut-outs, was kept as both main view only
  and shadow only, which no ray stops on (`StopsRay`): it is dropped. The 15 F10 `models.txt` dumps of
  2026-10-26 and 27 had none (one mesh with shadow 0x03, in a model without main-view cut-outs, was
  dropped already). And a skinned file the object brush asked for first gave its see-through meshes
  both leaf and hair: they are hair now. The brush never places a skinned file, so only a character of
  the same file could show it.
  A model file is read once, by its path hash, with the roles of the source that asked for it first;
  the object brush's differ from the others'. The log counts the files the brush and another source
  both asked for ("asked for by the object brush and another source"), to see whether it happens.
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
- **The sea** (`GI_INSTANCE_SEA`, 2026-10-30): a plane at the sea's level under the eye, one-sided as the
  terrain; see "Sea".
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
illumination's light on or off; `kick_traces::Asked::runsRayTracedShadows` turns them on, and the
light passes run only with `runsLight`. Lamp contact shadows stay as they are: lamps are not ray
traced here (see "Ray-traced lights"). With DLSS Ray Reconstruction denoising, the rays go out without the
denoiser (see "Ray Reconstruction").

- **Rays** (`TraceSunShadows` in `src/features/ray_traced_shadows/gi_shadow.hlsl`). One ray a pixel at the render
  size, from the depth's world position towards a point on the sun's disc (uniform over the disc of
  the sun size's angular radius, a new point each frame), up to 5 km. The sun's direction is the sun
  pass's (`cPSLight` `c0`), noted the frame before, as the scene is traced before that frame's sun
  pass is recorded.
- **Which faces cast** (`StopsShadowRay`, 2026-10-26). The game draws its shadow maps with front faces
  culled (1,227 of 1,267 shadow map draws in a capture of the prologue's hospital; the main view culls
  back faces), so an opaque surface casts from its back alone, as the light sees it. Rays towards the
  sun and the lamps do the same: an opaque surface they meet on its back lets them through, past copies
  of the surface and in the closest stretch by the next hit, further on by tracing the ray again with
  every triangle looked at, only when the first hit found was such a surface. Cut-out geometry, which
  the game mostly draws two-sided (most of the 40 two-sided shadow draws), casts from both sides. The
  global illumination's light and sun rays at its hits do the same; its sky rays and bounces see both
  sides. Before, the hospital corridor's windows, one-sided cards facing out (seen from outside, culled
  from inside), kept an outside searchlight out where the game lets it in through them. A rigid model's
  meshes the game never draws into its shadow maps (shadow bit 0, `GI_GEOMETRY_NO_SHADOW`), which the scene
  takes for where rays leave a room's walls and for the global illumination, do not stop these rays at all:
  one in a stairwell's windows (model `84a083c0196e01e1`, mesh 0) kept the moonlight, a shadow-mapped spot,
  off the wall it lights in the game. A character's main-view meshes still cast (its hair), but its
  see-through ones the game never casts with (cut-out with shadow bit 0, or alpha bit 6:
  `GI_GEOMETRY_HAIR`) only in part, since 2026-10-26: every ray towards a light, the sky's included,
  keeps half its light past each layer it crosses (`HairTransmittance`, `PassHair`), the idea of deep
  opacity maps (Yuksel and Keyser, 2008). Taken as solid, hair cards (Snake's: model `84a0c8705a4a98ca`,
  meshes 18 and 19, alpha 0xa0 shadow 0x01) cast hard, fully dark shadows the game never shows. The lamp shadows
  count the share that gets through, and the hair's distance for the filter's penumbra; the sun's rays,
  whose denoiser takes a pixel as lit or not, are lit with that share as their chance. So the rays
  towards lights take what the game's shadow maps hold, but for the solid stand-ins left out. Diagnostics,
  "Shadows from every surface (old)" (`shadowDiagnostics.x`), goes back to every surface casting from both
  sides.
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
  kilometres away is metres deep per pixel; the temporal pass is AMD's but in `gi_shadow.hlsl`, its
  17x17 local neighbourhood (which clamps the history and sets the variance the filters go by)
  counting only pixels on the pixel's own surface, each within 5% of the pixel's distance from the
  other's plane. AMD's counts every pixel within 8 whatever lies there: Snake in the sun in front of
  a shaded wall in Mother Base (2026-10-29) lit a band of the wall along his outline, 8 render pixels
  wide at any render scale, while the shadow rays debug view showed the wall blocked up to him. The
  neighbourhood's spread has the most its mean can be off by chance added (`0.25 / n`, `n` the rays
  it is worth): the few rays on a thin edge's surface often agree, and a spread of 0 threw the history
  away. The history and moments come bilinearly from the last frame's 2x2 pixels around where the
  pixel was, among those that pass AMD's depth test each. AMD's tests the one pixel the point falls
  in, which with the upscaler's jitter often lay across an edge: the history started over along every
  edge each frame and the raw rays flickered there (2026-10-29: a throwaway view of the history's age
  showed young history along every depth edge, with AMD's neighbourhood too; gone with **Jitter
  off**, and with the 2x2 test). Correcting the game's velocity for the last frame's jitter (it runs
  from the unjittered position) made no visible difference and was left out.
- **Sun pass** (`src/passes/sun_light_pass.cpp`, `sun_light_pass_shaders.hpp`). The shared R16_FLOAT shadow goes to
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
With DLSS Ray Reconstruction denoising, the rays go out without the filter (see "Ray Reconstruction").

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
- **Recording** (`features/ray_traced_lights/local_lights.cpp`). A light's marking draw is made as the game makes it; its lighting
  draw is made without a pixel shader, so the stencil is left as the game leaves it, and noted:
  `cPSScene`, `cPSLight`, `cPSMaterial` (as the constant capture keeps them), `t0` to `t10`, `s8` to `s11`,
  the light targets and the viewport. Both draws are taken before the lamp contact shadows see them.
- **Which lights.** At the kick, the lights of the lists recorded and not run yet whose `cPSScene` eye
  is the kick camera's (within a millimetre) go to Direct3D 12 in world space (`cPSScene` `c8` to `c10`
  take the light passes' view space there), up to 128, those reaching the most pixels on screen first,
  so that past 128 the smallest go without, the same ones frame after frame; those reaching none are
  never drawn again. Among as many pixels the nearest come first, and exact ties go by the light's key
  (its kind, its place to the centimetre, its colour), so which lights go without depends on the lights
  alone, not on the order the game drew them in (since 2026-10-28; `light_choice.cpp`). (On 2026-10-23, with a bit a light, they were taken in the order of a hash map of
  command lists, which changes every frame: in a base at night with about 100 lights and 64 bits, a
  different 45 went without each frame and a spotlight flickered between its shadow and none.)
- **Soft shadows** (`TraceLocalLightShadows` in `src/features/ray_traced_lights/light_shadows.hlsl`, 2026-10-24). One value a
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
  (`SampledLocalLight` in `src/passes/ray_tracing/shaders/gi_hit_light.hlsli`), first and later
  bounces, besides the sun and the sky: every light's diffuse light there without shadows, as the game's shaders work it out
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
- **Drawn again.** At each list with a draw point, which waits for Direct3D 12, the immediate
  context draws the lights of the lists run so far before the list runs: each its volume as the game drew it (below), cut
  by a scissor to the light's sphere (a spot's: the bounding sphere of its cone) on screen, in the light
  vertex shaders' form (the clip-space position twice, then where it samples the light targets). On the
  volume's far faces the share (1 for a light not traced) goes into both light targets' alpha, 0 where
  the surface lies past them; on its near faces 0 where the surface lies short of them; then on the far
  faces its shader with specular, with the noted bindings and constants, adds its light scaled by that
  alpha (`DEST_ALPHA`, `ONE`), as the lamp contact shadows do. Never at once, before its own list has
  run: the copy of its volume (below) is on that list, and a later kick's rays are not waited for until
  the next draw point. Until 2026-10-26 a light in a list run after the frame's first draw point was
  drawn at once, which broke the intro: its mirror is a second camera drawn whole first (1080x608 in the
  2560x1440 targets, its own kick, lights and `SunLight_ES`), so all the main view's lights were drawn
  before their lists ran, inside stale volumes (`frame-22272`: the main view's six lights added at most
  0.0003 and left 0 in the targets' alpha everywhere).
  A draw point is a pass after the game's lights and before anything
  reads them: the sun pass; indoors, where no sun shines, the sky's specular alone in its place
  (`SunLight_ES`, `367e8f6128befd48f2840ab3d6ed19b4`: `cPSLight` `c2.x` and `c3`, the sky reflection at
  `t15`; its list waits as the sun pass's does, `ray_tracing::noteSkySpecular`); or the
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
  the light through the vent falls. Where a box cuts across an open floor its light stops in a straight
  line along the tiles (the prologue's hospital, `frame-71823`, 2026-10-26). Diagnostics, "Light
  volumes", draws the lights over all their reach instead, those with a shadow map or every one: with
  every one, the room's fill lights showed as pools on its floor and round a picture on its wall, where
  the game's boxes keep them; a rule that frees only real lamps is still to be found.
- **Where the rays end** (2026-10-26). A light's shadow rays end 10 cm short of it (or its size), the lamp
  around it; a spot the game shadows with a shadow map's end where that map starts, its near plane:
  `m_shadowProjection` (`cPSScene` `c12` to `c15`, view space to the map, the depth reversed: cleared to 0,
  tested greater-equal) gives depth 1 that far along the axis, and along a ray it lies that over the ray's
  cosine to the axis. Nothing nearer is in the game's map. The prologue hospital's corridor lamps sit 13 cm
  above its ceiling in a box (a building mesh the game does cast with, `shadow 0x00`) that every ray met 0.3
  to 0.6 m short of the light, so the corridor stayed dark (the lamp probe). The global illumination's
  bounce and the lamp probe end theirs the same way.
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
  the 128 bits, and those lost; and how many lights' volume bounds are kept. Those are kept by light
  until the device goes, so a count that grows every frame is a light whose key (its place to the
  centimetre, its colour) keeps changing: a lamp that moves or flickers (a flashlight's are never kept). The cost table times the drawing and the Direct3D 12 rays ("Ray-traced
  lights") and measures the whole by leaving it out; the log has the rays too ("shadow rays for lights"). The flashlights alone took 0.18 ms of rays at 2560 x 1440 in their first session,
  every one traced at the kick.
- **Lamp probe** (`ProbeLocalLights`, 2026-10-26). With the shadow rays' debug view chosen, one thread
  at the centre pixel works out every light there without shadows and traces a ray to each one's centre
  as the shadow rays leave (`ShadowSurfaceAt`, `SurfaceRayBlocked`, short of the lamp): the share those
  rays let through, the filtered share the lights are drawn with, and the six strongest lights, each with
  its part of the light, its distance and what blocks it. It shows under the debug view and goes to the
  log as it changes ("Lamp probe"), sun or none: the sun's probe needs a sun pass, which rooms lack.

Not handled:

- Lights recorded after the kick are drawn without their shadow; the log tells how often.
- The rays know each light's volume by its bounds in the world, a frame or two late (since
  2026-10-26): as a light is drawn again, a compute pass takes its volume's vertices (clip space, as
  copied from its draw) through the inverse of its pass's `m_projectionView` into their lowest and
  highest corners, read back three frames later and kept by light (its kind, its place to the
  centimetre, its colour; the game's lights hold still). `LocalLightAt` gives no light outside them (2 cm
  spare), so the bounce, the share's picks and the lamp probe keep each light within its box, as the
  game and the light drawn again do. Before, the global illumination's bounce lit every surface within a
  light's reach: in the prologue's hospital three bluish fill lights 200 m wide and a 250 m spot
  standing in for the moon (colours about 1 : 1.3 : 2) lit the rooms they are boxed out of, and the
  light came out blue (its mean 1 : 1.4 : 2, the night sky's 1 : 1.8 : 3.5) where the game's is warm. A
  box is not turned with a spot's beam, so a spot's bounds are those of its turned box and cone, a
  little more than the game's. A flashlight's volume moves with it and is not kept; nor is a light
  whose bounds have not come back, which lights over all its reach.
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
  context. Each time the game binds the G-buffer it keeps a view of the third target, the specular
  target, whose `z` is the material. The sun pass reads it too (`t1`, `g_SpecularTexture`) but cannot
  give it: not every scene has one (mission 10010's hospital room has none), and a
  view kept from it would outlive the target. The game makes new targets at a new render size, so
  back at the first size the old view fits and shows that moment's picture for good, as a ghost in
  everything path tracing reads from the specular target. `onDraw` calls GTVBAO before the sun
  shadows, which draw the sun pass themselves.
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

## VB-SSGI

With **Indirect lighting (VB-SSGI)** on, screen-space global illumination with a visibility test
adds one bounce of light
between the surfaces on screen. The chain is ported from the RenoDX MGSV add-on's indirect lighting
(`renodx-mgsv` `src/games/mgsv/il`), which reads better than the visibility-bitmask gather SSILVB
used. Off, the add-on only checks a flag on each full-screen draw.

- **Where.** At the resolve, which already has the frame's finished diffuse light: the probes and
  sky (occluded by GTVBAO when it is on), the sun with its shadows, the local lights and the skin
  filter. The resolve and the sky light pass are in different command lists, so the passes take the
  normals any sky light pass bound last (the G-buffer's, one texture for the session).
- **Velocity.** The temporal accumulation reprojects with the game's velocity buffer (camera and
  object motion in `.ba`), which `MotionBlurCameraVelocity` draws right after the G-buffer. The
  indirect light pass notes that draw's render target and makes a view of it; the engine draws the
  velocity passes only with motion blur or an upscaler, so while VB-SSGI runs the add-on forces them on
  (`motion_blur::wantVelocityPasses`). Without the buffer the history blend collapses to zero and
  the raw gather shows. Measured in the RenoDX add-on: the TAA jitter cancels inside the buffer (the
  velocity pass runs with the jittered projection), so no jitter term is subtracted.
- **Gather** (`ssgi_shaders.hpp` `shaders::Gather`), recorded at the resolve's draw on its deferred
  context, with the game's compute bindings saved and restored: from the resolve's depth (`t11`),
  diffuse light (`t9`), albedo (`t8`, its sRGB view, so linear), the specular light (`t10`) and
  `cPSScene`. A golden-angle disk of up to 16 jittered taps per pixel, each rejected by plane
  elevation, source facing and inverse-square distance, returns the mean `albedo x diffuse +
  specular` of the taps that survive; alpha is validity. The radius is a share of the screen's
  height (80% by default), and the same radius in view units at the pixel's depth. Into an
  `R16G16B16A16_FLOAT` bounce target at the gather's size (full, half or quarter, rounded up).
  Before it, `visibility_bitmask::prefilterDistances` fills this add-on's own copy of the distance
  mips at the render size, and each tap marches them (the **Visibility** setting's steps, 5 by
  default) from the receiver to the sample: a surface nearer than the ray blocks the tap, so light
  does not bleed through occluders, which the disk alone would not see. 0 disables the test.
- **Accumulate** (`shaders::Accumulate`): reprojects with the velocity, fetches the ping-ponged
  history bilinearly, clips it to this frame's 3x3 neighbourhood, and rejects on depth, normal and
  screen bounds (a separate aux target carries the history's depth and normal). The blend is an
  adaptive ramp capped by the History setting, and the history's length rides in alpha. Running
  luminance moments go into an `R16G16_FLOAT` target, which the denoiser reads as its noise estimate.
  Non-finite values are guarded everywhere: one bad value would be carried forward by the temporal
  blend, squared into infinity by the moments, and render black forever.
- **Denoise** (`shaders::Filter`): an a-trous edge-aware filter, 1-3 dispatches of 5x5 taps whose
  stride doubles each step (1, 2, 4), weighted by normal, depth and the measured per-pixel noise.
  Never-sampled pixels are skipped, not averaged in. The targets alternate so no dispatch reads what
  it writes.
- **Resolve** (`shaders::Resolve`) at the render size: a depth-joint bilateral upsample of the
  filtered gather, times Strength, written as `diffuse + bounce` into an `R16G16B16A16_FLOAT` copy of
  the diffuse light (alpha kept).
- **Resolve draw.** Drawn by the add-on with that copy in `t9` (and white in `t13` with GTVBAO on);
  the game's `t9` is restored. The game's occlusion, with GTVBAO off, multiplies the bounce like the
  rest of the light.
- **Options** apply from the frame after the next present. A resolve without normals, with sizes
  that differ or without `cPSScene` keeps the game's diffuse light and logs the reason once.
- **Diagnostics.** "VB-SSGI only" writes the light without the game's diffuse light and draws the
  resolve with `shaders::Show`: the bounce alone, as light before the albedo. The **Debug view**
  setting shows the gather alone, the history delta, the history length, the reprojection offset or
  the measured variance, and it draws through `shaders::Show` too: the game's resolve would multiply
  the light by the albedo, and the gathered light is what is being judged. Both views write the
  resolve's alpha, `sqrt(luminance / 32)`, which exposure, bloom and depth of field read from the
  scene target (`CopyReductionX4`, `DOF_ScatterBakeFirst`); with 1 there, they blurred the view.

Not handled:

- One bounce, from what is on screen: light from off-screen or hidden surfaces is missing, and the
  bounce fades towards the screen's edges.
- The probe lighting may already hold some bounce from static scenery, which VB-SSGI adds again.
- Forward and transparent objects neither give nor receive the bounce.
- The gather's plane test rejects samples on the receiver's own surface, which starves flat ground
  of valid samples (26-73% of them); the denoiser fills the holes, so the ground reads slightly
  flat rather than broken. A fix needs real radiance (a hemisphere ambient term), not merely more
  accepted samples.

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
- **Without a sun pass.** Only a fill whose command list had a sun pass before its resolve is physical,
  and only after one does the sky draw take the physical sky; indoors, without one, the game's look
  stays. Until 2026-10-26 the first sun pass's constants stood for the sun from then on: the main
  menu's daytime sun (towards 0.49, 0.75, 0.44) lit the sky outside the prologue's hospital at night,
  which has no sun pass (`frame-73986`: only `SunLight_ES`). The global illumination's sun went the
  same way; it now has none after a frame without a sun pass.
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
  last slice's fog in front, then the game's TppTonemap (`cPSMaterial` `c1`) and sRGB encoding, or
  the encoding alone in the float scene, which takes TppTonemap out of it, clamped to 1 with HDR
  output (see "HDR", "TppTonemap out"). No dither (see "Dither").
  Its two textures go to `t6` and `t7` for the draw; the game's are bound again after it.
- Checked on WARP with made-up constants: blue sky with a pale horizon at noon, orange towards the sun
  at 1 degree, the pink band and the Earth's shadow at -2 degrees.

## Sea

With **Sea** (`sea=1`, under atmosphere; `Effect::Sea`, first in a cost measurement's order) the sea around
Mother Base is the add-on's (`src/features/sea/`), drawn into the G-buffer, so the game's lighting, fog and
screen-space reflections, the global illumination and path tracing take it as any surface.

- **The game's sea** (2026-10-30, frame 29300 and the shader dump). `Primitive_TppFourierOceanHQ` and `LQ`,
  the near patches (`draw 256`), and `Fill`, the far sea (indexed, no depth write), drawn after the sky over
  the lit scene (blend ONE / INV_SRC_ALPHA); `Tpp24hOcean` and `TppBaseOcean` exist and no capture drew
  them, so the add-on leaves them alone. Its waves are a 64 x 64 field the CPU makes each frame, copied at the frame's start into two
  `R32G32B32A32_FLOAT` textures (displacement, gradient). Its sky reflection is two fixed colours,
  (0.06, 0.105, 0.195) to (0.135, 0.22, 0.28), times its Fresnel, with the sun's highlight, foam textures and
  a depth tint: no reflection map, no screen-space reflections. It is in neither the G-buffer nor the
  ray-traced scene. Its pixel shaders as the game makes them: HQ `799e8c706b42d3aa642843f8e73fdaf9`, LQ
  `536ed28c18bb66d2828795bdcf5fc3d5`, Fill `9291bd0ac69ab6cdeae6244f778c4ddb`, 24h
  `15813be2f09855201bece13ee4ad0ee4`, Base `fa14008376499401986af5774bccce68`; the float scene takes TppTonemap
  out of them (see "HDR"), and the frame log names them after the patched code (`0b289738...`, `10ece44e...`,
  `51218f2d...`, `bbc39752...`, `28488df6...`). `game_shaders.hpp` has both of HQ, LQ and Fill (`GamePass::Sea`).
- **Its level and wind.** The near patches' material constants (`cVSMaterial`, `b4`): `c0.z` the level, `c1.xy`
  the wind's direction, `c1.w` a tile's size, `c0.xy` a scroll that moves with the wind. Read back at Mother
  Base: `c0.z` -23.4, as the patches' world matrices place them (`cVSObject` `c5.w`), the wind towards
  (0.8, 0.6). The far sea's `c0.z`, its `waterHeight` (its vertex shader clips at the plane y = waterHeight),
  is 5 m lower, -28.4: taken first, it put the sea 5 m below the pillars' waterline. Through the constant
  capture these buffers read as other draws' constants (levels from -62 m to 2000 m within a second), so
  the add-on copies the near patches' `b4` on the GPU once between two presents (`CopyResource`): the frames
  recorded after it read the copy. The copy's list hands it over as the immediate context runs it; only
  then does the sea draw, and the CPU reads the copy back once a second for the ray-traced scene
  (`sea::level`), the readback after the copy on the GPU (read before it, the first readback was zeros: the
  sea 23 m too high). The sea waits for the near patches: a frame with only the far sea gives no level.
- **The waves** (`sea_shaders.hpp`): Tessendorf's FFT ocean ("Simulating Ocean Water", 2001) on three tiles
  of 256 x 256 texels, 400 m, 59 m and 8.7 m across, each with the wavenumbers from 6 of the next tile across
  down to its own: 400 m to 9.8 m, 9.8 m to 1.45 m, 1.45 m to 7 cm. The spectrum is JONSWAP with Donelan and
  Banner's spreading, as Horvath gives them ("Empirical directional wave spectra for computer graphics",
  2015). Past the fetch at which the wind fully develops the sea (a dimensionless fetch of 1.58e4, where
  JONSWAP's peak meets Pierson and Moskowitz's, 0.877 g / U) it grows no more, with Mitsuyasu's peak boost
  (1980): JONSWAP's fetch formulas for the 300 km of open ocean gave 1.75 m waves at 4 m/s. Summed offline
  over the tiles' texels, the significant heights came within 15% of Pierson and Moskowitz's: 0.39 m at
  4 m/s, 0.89 at 6, 1.58 at 8, 4.68 at 14. Each frame on the GPU: the spectrum at the frame's time (its
  Gaussian amplitudes from a hash of the texel, so nothing is kept), four complex fields a tile (two real
  fields in each: heights, sideways displacement, slopes, the displacement's derivatives), Stockham's
  radix-2 inverse FFT (Govindaraju et al. 2008) along the rows and then the columns in group-shared memory,
  then the maps with their levels of detail: the displacement; the slopes with the displacement's stretch;
  and the slopes' squares with the foam, whose levels give the spread of slopes a pixel covers (LEAN mapping,
  Olano and Baker 2010; for the sea, Bruneton et al. 2010). Foam gathers where the displacement's Jacobian
  falls below 0.55 and fades over 2.5 s. The wind is the weather's, 6 m/s in sun, 8 cloudy, 14 in rain, 10 in
  sandstorms, 4 in fog, turning over about 20 s, times **Waves** (`sea_waves`); its direction the game's.
- **The surface**, into the G-buffer at the frame's first draw of the main view's G-buffer: the add-on
  watches the render target binds for three 8-bit targets of one size with depth, and the viewport filling
  them (the intro's mirror draws into a corner). It draws with the game's own camera constants (`b2`, the
  jitter in them). Twelve rings around the eye (Losasso and Hoppe's geometry clipmaps, 2004), 128 cells
  across, the finest 0.25 m apart and each twice as coarse, the twelfth 65 km across; one instanced draw,
  no vertex buffer. A ring's vertices inside the finer ring are NaN, which drops their triangles; towards its
  edge a ring's odd vertices slide onto the next ring's grid (Strugar's CDLOD morph, 2009), their
  displacement's level of detail the same on both sides. The Earth's curve, and past the game's 4 km far
  plane the depth 1e-7, so the sea meets the sky at the horizon. As the game's objects write it: the albedo
  (sRGB values) from water's, linear (0.002, 0.012, 0.03) (`GI_SEA_ALBEDO`), to foam's 0.7; the normal in
  view space (`EncodeViewSpaceNormal`) from the slopes, tilted back where it faces away from the eye; the
  roughness from the slopes' spread with Cox and Munk's slope variance the tiles leave out; specular
  intensity 1; material 106 (white, F0 0.027, the table's nearest to water's 0.02). Its own depth goes to a
  fourth target (`sea::surfaceDepth`). The game's sea draws are skipped while the add-on's is drawn: while the
  game drew its sea in the last 60 s, decided at present for the frames recorded after it; a second without
  the main view's G-buffer (a loading screen) forgets it. Sea on or off takes effect at a present too, so the
  add-on's sea and the game's change together. A frame recorded across that present can have both or
  neither. A draw that fails gives the game's sea back, and the budget says the sea is unavailable.
- **The G-buffer** (`fox3ddf_blin` and the sun pass): `rt0` the albedo, sRGB values, alpha 1; `rt1` the
  view-space normal (`xy x 0.5 + 0.5`, `sqrt(z x 0.5 + 0.5)`), alpha the material's `CalcedReflectionTh` (0 on
  most pixels); `rt2` the roughness, the specular rate, the material (`/ 255`) and the translucency. The
  material table (`g_MaterialTexture`, 256 x 2): row 0 a colour and a translucency scale, row 1 F0 in red and
  the anisotropy in alpha; materials 0 to 67 and 100 to 167 are filled, alike. The screen-space reflections'
  mask takes smooth surfaces within 100 m, so they reach the sea without path tracing.
- **Reflections.** With path tracing, the traced reflections land on it as on any surface. Ray Reconstruction's
  specular albedo guide (see "Ray Reconstruction") gives it water's own reflectance where the sea's depth is
  the G-buffer's, Karis' fit of GGX's split sum for F0 0.02 ("Physically Based Shading on Mobile", 2014), in
  place of the game's weighting, floored at the sun pass's `c2.z` (0.21 on the decks) and halved: the sea
  reflects almost all of the sky at grazing angles, and 2% looking down.
- **In the ray-traced scene**: a plane at the level under the eye, 100 km across, two triangles in a static
  structure (`GI_INSTANCE_SEA`), one-sided with its front up, as the terrain is: the decks' rays meet its top,
  and a ray leaving a wave's trough below the plane passes its back, so it lies at the level itself. Its hits
  take water's albedo (`HitAlbedo`). Before, the rays down from the decks met nothing and took the sky's light.
  The plane and the guide's water are in a kick only when the list that drew the sea into the frame's
  G-buffer ran before it (handed over, see "Command lists"): a frame counter by presents does not tell, as
  the game records the next frame while it runs the last. A path-traced reflection's hit on the plane is lit
  like any other hit, diffuse: no reflection of its own.
- **Cost** (2026-10-30, 4K output): 0.27 ms for the waves and the surface (the budget's timed passes). The
  lighting and the rays the sea's pixels then take are in the cost measurement's step for it.

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
  and far split at device depth c11.x. It gathers the four depths under each half-size pixel: the near
  layer's radius comes from the nearest, the far layer's from the farthest, and the far colour from
  the four behind c11.x alone. One background pixel of four makes all four blurred.
- **Scatter.** `DOF_ScatterBakeSecond` makes 1/4, 1/8 and 1/16 size levels. `DOF_ScatterSprite`
  (indexed, 2048 quads a draw) draws a quad per pixel of a level, sized by its blur, shaped by a
  64x64 bokeh texture and divided by its area, with additive blending; big blurs go to the small
  levels. Near, then far. Small blurs are gathered with a few taps (`DOF_ScatterGather1`,
  `CompositeNear`, `CompositeFar`). `CompositeNear` draws into the half-size result and `CompositeFar`
  blends under it (one minus the target's alpha, plus the target). The far layer's cover is at least
  `saturate(2 (r - c12.x) / (c12.y - c12.x))`, `r` the half-size pixel's radius.
- **`DOF_ScatterCompositeFinal`** writes `blur + (1 - coverage) x scene` from the half-size result
  and a copy of the scene (`CopyRenderBuffer`) into the scene target, sRGB-encoded, alpha kept.
- Weak points: everything is half size or less, so slight blur turns blocky and shimmers at focus
  edges; one depth splits near from far; the HDR lights are not tonemapped back, so they clip.

The add-on keeps the game's depth of field before DLSS (see "What the add-on changes"), with its
focus edges at the render size (`features/depth_of_field`).

- **Why.** The farthest of four depths lays the blur over the rim of whatever is in focus, in steps
  of a half-size pixel. Those steps stay where they are as the jitter moves the image, so DLSS and Ray
  Reconstruction keep them, and they widen as the render size falls: 2 output pixels at native 4K, 3.4
  at Balanced. In frame 32570 (Balanced, 4K), the steps along a photo's edge fell every 48 render
  columns, on the columns the block rule gives from that frame's depth (29, 76, 123), not on the
  geometry's (every 24).
- **The pass.** At `DOF_ScatterBakeFirst`, while the game's constants are bound, a draw at the render
  size works out each pixel's own far cover by the formulas above. `DOF_ScatterCompositeFar` draws
  into a layer of its own, unblended, which leaves the near layer alone in the game's target. The
  composite is then `near + (1 - near.a) x lerp(scene, far.rgb / far.a, own cover)`. The half-size far
  layer gives the blur's colour, and each render pixel's depth says how much shows. That edge is the
  geometry's, which moves with the jitter and which DLSS resolves as any other. Where the two covers
  agree this is the game's blend; the game's shaders and these on WARP agree to under an 8-bit step
  over four lenses, and to the last digit in the composite. The near layer stays as the game drew it,
  because near blur rightly spreads over what is in focus behind it.
- **What it leaves.** A far blur's bokeh no longer spreads over what is in focus in front of it, which
  a lens would not do either. Far cover under 5% fades out, as a colour from so little of a pixel
  stands for nothing.
- **The background's blur** (Camera, **Depth of field**, `depth_of_field_far_blur`). **MGSV** keeps the
  game's far layer in the composite above. It is a soft smear, not a lens's disc: built from layers
  down to 1/16 size, it falls off like a Gaussian. **Lens**, the default, skips the game's
  `CompositeFar` draw and makes its own layer at half the render size, at the composite:
  - The prefilter leaves out what is in focus and weighs each pixel down by how bright it is (Karis),
    so one noisy ray cannot turn into a disc. Its colour is multiplied by the radius, so a bilinear tap
    beside an in-focus texel keeps its colour.
  - A disc gather of up to 96 taps on a golden-angle spiral, where a tap counts if its own blur reaches
    this far (Jimenez 2014, "scatter as you gather"), then two 8-tap fills between the taps. Past 96
    taps, from about 11 render pixels of radius, the taps spread out and a small lamp's disc breaks
    into them. One fill as wide as the taps are apart lands on the neighbouring taps, so the first ring
    is 0.105 of the radius and the second 0.05. Offline, the gather's maths at 60 render pixels: a lamp's
    ripple 0.65 with one fill, 0.42 with 192 taps, 0.12 with two fills; frame 32570's scene against a
    true disc, rms 1.12, 0.98, 0.61 of 255. Mips the width of the gap were worse than either, at 1.97.
  - The composite stretches the layer with a cubic B-spline in four bilinear taps (Sigg and Hadwiger
    2005). A pixel's cover is its own radius: sharp under half a render pixel, all blur over one and a
    half. The radius follows the game's formula, starting at its smallest size (c12.x), and comes from
    a second channel of the cover draw.
  - Measured on the copy (4K, Quality, Ray Reconstruction, f/1.4), against a true disc blur of the
    no-blur frame: detail, rms 8.80 (game) to 7.75; a flat wall, 2.05 to 1.78. Lamps at strong blurs
    were checked offline alone, and the cost is unmeasured.

  The near layer stays the game's with either.

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
- **The add-on** changes none of this. Its draw distance setting (1.5.0 to 1.7.0) widened the windows
  with a hook on `0x15F010` and raised the resolution and lengths each frame; it was removed.

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

What `src/features/photo_mode/photo_mode.cpp` does. It checks the first bytes of the selector update
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
