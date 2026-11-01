#pragma once

#include "phantom_fx_settings.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// What Phantom FX's passes cost on the GPU, for the settings window (docs/DEVELOPMENT.md, "Costs").
// While someone looks (the settings window open, the costs shown over the game, a measurement running,
// or a dev build keeping them on file), each pass ends a timestamp query before and after its work, in the context
// it records in. The game records a frame on its deferred contexts while the immediate context
// executes the one before, so a deferred context's timestamps follow its command list and count for
// the frame whose present follows the list's execution. They are read back a few frames later,
// without waiting, and averaged over about half a second.
//
// The ray tracing runs on its own Direct3D 12 queue, which times its phases itself (gi_renderer.cpp).
// Direct3D 11 waits for it (GiWait), so the wait holds those phases, not on top of them.
//
// A timed pass misses what it changes in the game's own work: GTVBAO leaves the game's SSAO out,
// the fog makes the game's fog texture larger. A measurement therefore times the GPU frame with
// everything on, then leaves out one effect after another; each difference is what that effect
// really costs.
namespace phantom_fx::performance
{
// The add-on's passes, grouped by effect in the budget (menu_budget.cpp).
enum class Pass : std::size_t
{
    UpscalerInputs, Upscaler, UpscalerOutput,
    SunTrace, SunLight, LampTraces, LampShadows, LampSoftLight,
    OcclusionPrefilter, OcclusionMarch, OcclusionDenoise, OcclusionApply,
    LightPrefilter, LightMarch, LightDenoise, LightUpsample,
    FogPass, SkyTables, FogShadow, FogScattering, FogIntegration, Sky,
    SeaWaves, SeaSurface, WaterCaustics,
    GiInputs, GiProbeLight, GiCombine, GiWait, LocalLights, PathTracedLight,

    // On Direct3D 12
    RayTracedScene, SunShadowRays, SunShadowDenoiser, LightShadowRays, LightShadowFilter, GiRays, GiDenoiser, Reflections, LampLight,
    HdrEncode, HdrDecode, HdrGrade,
    Count
};

using Milliseconds = std::array<float, static_cast<std::size_t>(Pass::Count)>;

// In milliseconds, averaged over about half a second.
struct Costs
{
    float frame{};    // Between presents: the whole frame, as the frame rate shows it
    float gpuFrame{}; // The GPU's work on a frame, from its first command list to its present
    Milliseconds passes{}; // 0 where a pass did not run
};

// What a frame took on the GPU.
struct FrameCost
{
    float gpuFrame{}; // 0 when no command list executed
    Milliseconds passes{};
};

// The real cost of each effect: the GPU frame with everything on, then with the effects left out
// one by one, cumulatively. Each step settles, then is timed. Effects that are off are skipped.
struct Measurement
{
    struct Step { std::string removed; float gpuFrame{}; }; // The first step removes nothing
    std::vector<Step> steps;
    std::uint64_t finishedAt{}; // GetTickCount64 when `steps` were measured
    bool running{};
    unsigned step{}, stepCount{};
};

struct Status
{
    bool overlay{}; // The costs show over the game
    Costs costs;
    Measurement measurement;
};

// ---- Setup, on the thread running the immediate context ----
// `report` is PhantomFX-performance.txt, which holds the costs while they show over the game or are measured.
void attach(ID3D11Device* device, ID3D11DeviceContext* immediate, const std::filesystem::path& report) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;

// ---- Recording, on any thread ----
struct TimedRun
{
    Pass pass{};
    Microsoft::WRL::ComPtr<ID3D11Query> start, end;
};

// Times the GPU work recorded on `context` from its construction until end() or its destruction.
// While no one looks at the costs it does nothing.
class Timed
{
public:
    Timed(ID3D11DeviceContext* context, Pass pass) noexcept;
    ~Timed() { end(); }
    Timed(const Timed&) = delete;
    Timed& operator=(const Timed&) = delete;

    // Ends the pass being timed, if any, and starts timing `pass`.
    void next(Pass pass) noexcept;
    void end() noexcept;

private:
    ID3D11DeviceContext* context;
    std::optional<TimedRun> run;
};

// What `from` recorded now belongs to `into`: FinishCommandList, or a command list executed in a
// deferred context.
void recorded(std::uint64_t into, std::uint64_t from) noexcept;
void forget(std::uint64_t contextOrList) noexcept;

// ---- The ray tracing's Direct3D 12 queue (gi_renderer.cpp) ----
// A frame's passes there, once the GPU is past them; they count for the frame being timed now.
void timedOnDirect3D12(const Milliseconds& passes) noexcept;
// Milliseconds by phase, in the order the phases first ran. The names are literals.
using PhaseTimes = std::vector<std::pair<const char*, double>>;
void addPhase(PhaseTimes& phases, const char* name, double milliseconds);

// ---- Immediate context ----
// Before the immediate context executes `list`.
void executing(std::uint64_t list) noexcept;
// Each present, after the add-on's own present work.
void present() noexcept;

// ---- Overlay (performance_menu.cpp) ----
Status status();
// The costs are on screen or kept on file: frames are timed for the next second.
void watch() noexcept;
void showOverlay(bool show) noexcept;
// Starts measuring each effect's real cost, unless a measurement is running.
void measure() noexcept;
// The settings window's budget page, whose presets switch `wanted`, and the costs over the game, drawn
// every frame.
void drawBudget(Settings& wanted);
void drawOverlay();
// The costs as plain text, for PhantomFX-performance.txt.
std::string report(const Status& state);
}
