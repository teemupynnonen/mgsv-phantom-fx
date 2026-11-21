# Building a new add-on on the core

`phantom_fx_core` is the part of Phantom FX that more than one add-on uses. It is a static library: each add-on links
it and carries its own copy. There is no shared DLL, no ABI, no version handshake and no load-order coupling. An add-on
that was not built against the core cannot use it, and two add-ons never share state through it.

PhantomFX and the blood add-on (`https://github.com/ZioPao/mgsv-phantom-blood`, branch `standalone`) are the two
consumers. The blood add-on is the worked example this guide is written from: its `addons/blood.cpp`, its
`settings_store` and its `CMakeLists.txt` are small enough to copy.

The core gives you the game access and the frame vocabulary, not an add-on framework. A new add-on still owns its
exports, its ReShade events, its log, its ini and its draw order. That is deliberate; see the end of this file.

## 1. The target

The core is its own CMake project at `src/core/CMakeLists.txt`. A new add-on adds it and links the target:

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyAddon VERSION 0.1.0 LANGUAGES CXX)

add_subdirectory(path/to/mgsv-phantom-fx/src/core core)

add_library(my_addon SHARED
    addons/my_addon.cpp
    src/features/my_feature.cpp
)
target_include_directories(my_addon PRIVATE src)
target_link_libraries(my_addon PRIVATE phantom_fx_core)
target_compile_features(my_addon PRIVATE cxx_std_20)
target_compile_definitions(my_addon PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(my_addon PRIVATE /W4 /WX /permissive- /utf-8)
set_target_properties(my_addon PROPERTIES OUTPUT_NAME MyAddon SUFFIX .addon64)

install(TARGETS my_addon RUNTIME DESTINATION . COMPONENT addon)
```

Inside the PhantomFX repository the first line is already there, so only the rest is needed; add the sources to the
existing target or give the add-on its own.

`phantom_fx_core` publishes:

| What | Value |
| --- | --- |
| Include directories | `src`, `src/core`, `src/core/game`, `src/core/capture` |
| Language | C++20 |
| Definitions | `WIN32_LEAN_AND_MEAN`, `NOMINMAX` |
| Libraries | `d3d11`, `d3dcompiler`, `minhook` |

It fetches MinHook itself when the consumer has none, and builds with the consumer's runtime (`/MT` in PhantomFX;
keep add-ons on the static runtime). Its tests are built when it is the top-level project, or when
`PHANTOM_FX_CORE_TESTS` is on. It also builds alone: `cmake -S src/core -B build`, then `ctest --test-dir build`.

## 2. The entry point

ReShade loads `MyAddon.addon64` and calls the four exports itself. The core does none of it. A minimal entry point,
copied from `addons/blood.cpp`:

```cpp
#include "core/game/device_check.hpp"
#include "core/log.hpp"
#include "features/my_feature/my_feature.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <d3d11.h>
#include <reshade.hpp>
#include <filesystem>
#include <string>

extern "C" __declspec(dllexport) const char* NAME = "My Addon";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "What it does, in one line.";

namespace
{
HMODULE self{};
command_list* immediateList{};
bool ownsMinHook{};

// Log and ini live beside the add-on, as ReShade keeps its own.
std::filesystem::path dataDirectory()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        const auto length = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return std::filesystem::current_path();
        if (length < path.size()) return std::filesystem::path(path.substr(0, length)).parent_path();

        path.resize(path.size() * 2);
    }
}

void onInitCommandQueue(command_queue* queue)
{
    auto* context = reinterpret_cast<ID3D11DeviceContext*>(queue->get_native());
    if (queue->get_device()->get_api() != device_api::d3d11 || !context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || immediateList)
        return;

    immediateList = queue->get_immediate_command_list();
    my_feature::attach(reinterpret_cast<ID3D11Device*>(queue->get_device()->get_native()), context);
}

void onDestroyCommandQueue(command_queue* queue)
{
    if (queue->get_immediate_command_list() != immediateList) return;

    my_feature::detach();
    immediateList = nullptr;
}
}

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon, HMODULE reshadeModule)
{
    if (!reshade::register_addon(addon, reshadeModule)) return false;

    self = addon;
    const auto directory = dataDirectory();
    openLog(directory / "MyAddon.log");
    log("My Addon.");

    // One MinHook instance per process: a second add-on that hooked first keeps it.
    const MH_STATUS hooks = MH_Initialize();
    ownsMinHook = hooks == MH_OK;
    if (hooks != MH_OK && hooks != MH_ERROR_ALREADY_INITIALIZED) log(std::string("MinHook: ") + MH_StatusToString(hooks));

    my_feature::loadSettings(directory / "MyAddon.ini");
    if (isSupportedGameBuild()) log(my_feature::hook());
    else log("This game build is not supported; the feature stays off.");

    reshade::register_event<reshade::addon_event::init_command_queue>(onInitCommandQueue);
    reshade::register_event<reshade::addon_event::destroy_command_queue>(onDestroyCommandQueue);

    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon, HMODULE reshadeModule)
{
    my_feature::detach();
    my_feature::saveSettings(dataDirectory() / "MyAddon.ini");
    if (ownsMinHook) MH_Uninitialize();

    immediateList = nullptr;
    reshade::unregister_addon(addon, reshadeModule);
}
```

Three habits from that file:

- **Own MinHook only if you initialised it.** `MH_Uninitialize` from one add-on removes every add-on's hooks.
- **One log per add-on.** `openLog` is per DLL; two add-ons writing `PhantomFX.log` interleave.
- **One ini per add-on.** A shared file lets one add-on's defaults overwrite the other's keys.

## 3. The events you will want

Register only what the feature reads. The core's helpers and the event that feeds them:

| Event | What to call |
| --- | --- |
| `init_command_queue`, `destroy_command_queue` | Your `attach(device, context)` and `detach()`; the immediate context is the one to keep |
| `init_pipeline` | `game_shaders::pixelShaderCreated`, `vertexShaderCreated`, `inputLayoutCreated`; the blood also reads its own pool shaders here |
| `draw`, `draw_indexed` | `game_shaders::DrawnShader shader(context)`, then your `draw` (see the next section) |
| `bind_pipeline` | `game_shaders::vertexShaderOf(reinterpret_cast<IUnknown*>(pipeline.handle))` for vertex-shader-based dispatch |
| `map_buffer_region`, `unmap_buffer_region` | `constant_capture::bufferMapped(buffer, data)` / `bufferUnmapped(buffer)` |
| `present` | `FrameLatch::present()`: the next frame starts with the settings that changed |
| `reshade_present` | Hotkeys, through the `effect_runtime*` you stored from `init_effect_runtime` |
| `init_swapchain` | Output size, if the add-on upscales |
| `init_resource`, `destroy_resource` | If it tracks the game's textures by resource |

A draw event returns true to replace the game's draw. Return false to leave it alone.

## 4. The order a frame runs in

A feature declares the slot it works in and a priority inside it. Lower priority runs first; equal priorities keep the
order they were added in. The first step that takes the work ends the list, so a draw is drawn in the game's place
exactly once.

```cpp
using DrawOwners = fxcore::Chain<ID3D11DeviceContext*, const ui_redirect::DrawCall&, game_shaders::DrawnShader&>;
DrawOwners owners;

void registerDrawOwners()
{
    owners.add(10, "my feature", my_feature::draw);
    owners.add(20, "another pass", another_pass::draw);
}

// In the draw event:
const bool drawn = owners.run(context, call, shader) != nullptr;
```

`DrawClaim` covers work claimed outside a chain: `take(owner)` is true for the first owner, false for a second, and the
caller logs the refusal. `clear()` starts the next draw.

The slots: `FrameStart`, `GameDraw` (the game's draws, before lighting), `ShadersBound`, `SceneReady`,
`BeforeUpscale`, `BeforePresent`, `Overlay`. A feature that needs an ordering the slots do not express is a reason to
add a slot, not to invent one locally.

## 5. Settings

The core owns the file format, not the settings. Each add-on keeps its own struct, its own keys and its own file:

```cpp
struct Settings
{
    bool on{true};
    float radius{0.05f};
};

void load(const std::filesystem::path& file, Settings& settings)
{
    if (!std::filesystem::exists(file)) return;

    std::ifstream input(file);
    if (!input) throw std::runtime_error("Cannot read MyAddon.ini.");

    const auto values = fxcore::settings::read(input);
    if (const auto it = values.find("myaddon"); it != values.end()) fxcore::settings::readFlag(it->second, settings.on);
    if (const auto it = values.find("myaddon_radius"); it != values.end()) fxcore::settings::readFloat(it->second, settings.radius, 0.01f, 1.0f);
}
```

Writing is the other half; the core has no writer, so stream the keys out yourself:

```cpp
std::ofstream output(file, std::ios::trunc);
output << "myaddon=" << settings.on << '\n';
output << "myaddon_radius=" << settings.radius << '\n';
```

`readFlag` takes only `0` and `1`; `readFloat` clamps to the range the caller gives; `number<T>` takes the whole text,
so trailing junk is refused. A value the file does not hold leaves the setting as it was.

For settings the render thread reads, hold them in `fxcore::FrameLatch<Settings>`: `set` from the thread that changes
them, `present()` at the present event, `frame()` from the draw code. The frame being recorded never mixes two sets.

PhantomFX's `SettingVisitor` is PhantomFX's: it describes PhantomFX's effects and is not part of the core. The blood
add-on carries a small visitor of its own in `settings_store.cpp`; either pattern is fine for a new add-on.

## 6. Tests

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

## What the core provides

| Header | What it gives |
| --- | --- |
| `core/order.hpp` | `Slot`, the slots a feature can work in; `Chain`, the steps of one slot in priority order; `DrawClaim`, one owner per draw |
| `core/frame_latch.hpp` | `FrameLatch<T>`: the settings a frame started with, so a frame never mixes two settings sets |
| `core/settings/ini.hpp` | The settings file format: `Values`, `read`, `parseLine`, `trim`, `number<T>`, `readFlag`, `readFloat` |
| `core/log.hpp` | `<add-on>.log` beside the add-on, `log`, `hex`, `logOnce`; one writer per DLL |
| `core/clock.hpp` | `QueryPerformanceCounter` ticks and milliseconds |
| `core/hresult.hpp` | `check`: a failed Direct3D or Windows call as an exception naming the step |
| `core/d3d11_helpers.hpp` | Shader compiles, dynamic constant buffers, compute textures, texture sizes |
| `core/shader_bytecode.hpp` | DXBC checksum, the instruction walk, the output saturation and dither patches |
| `core/game/game_code.hpp` | The game's image, code checks, hooks and patches |
| `core/game/game_constants.hpp` | The game's constant buffers by name, and a light pass's view space |
| `core/game/game_shaders.hpp` | The game's pixel and vertex shaders in one table, and `DrawnShader`, a draw's lookup |
| `core/game/game_lights.hpp` | What a point or spot light shader is: kind, falloff, twin, range |
| `core/game/device_check.hpp` | Lets the game start under ReShade, and the build check |
| `core/capture/constant_capture.hpp` | What the game's threads last wrote to the constant buffers, read by type |

## Namespaces and includes

- `fxcore`: the core's own code. `Slot`, `Chain`, `DrawClaim`, `FrameLatch`, `fxcore::settings`.
- `phantom_fx`: code that moved out of the add-on unchanged. The log, the clock, `check`, the Direct3D helpers,
  `shader_bytecode`, the game's constants, lights and shaders, `device_check`, `constant_capture`.

The moved files kept their names, so `#include "game_shaders.hpp"` or `#include "log.hpp"` resolves in either add-on.
New core code is included by path: `#include "core/order.hpp"`, `#include "core/settings/ini.hpp"`.

## Coexisting with PhantomFX

ReShade delivers every event to every add-on, in load order, with no priorities between DLLs. Two add-ons must replace
different things: the blood add-on claims character draws, PhantomFX claims passes. A draw both would claim is a
double draw, not a merge.

What each add-on pays for separately, by design:

- its own `constant_capture` (two captures, two maps of the same buffers);
- its own `game_shaders` table (each registers the pipelines it sees);
- its own MinHook instance (shared per process, owned by whichever initialised it);
- its own log and ini.

Ordering inside one add-on is the schedule above. Ordering between the two does not exist; do not design around it.

## What the core does not provide yet

- ReShade event dispatch, an add-on base class or a registration list.
- The overlay and its pages; each add-on draws its own, or has no UI.
- The texture registry (`GameTexture`, `gameTextureOf`, `currentLevelOf`). The blood add-on keeps its own copy in
  `src/features/blood/texture_registry`; a new add-on that needs the game's textures copies that file or lifts it
  into the core.
- A settings UI, a version check between DLLs, or ordering between add-ons.

Each waits for a second consumer to need it. `docs/addon-plan.md` says what was deferred and why.
