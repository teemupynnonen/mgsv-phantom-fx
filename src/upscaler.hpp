#pragma once

#include "frame_generation.hpp"
#include "ui_redirect.hpp"
#include "upscaler_settings.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

// NVIDIA DLSS or AMD FSR (fsr_upscaler.hpp) after Fox Engine's depth of field and before its grading,
// in place of its FXAA, at native resolution (DLAA) or upscaling from a lower render size
// (render_scale.hpp, ui_redirect.hpp);
// docs/DEVELOPMENT.md ("What the add-on changes") describes how. The ReShade add-on (upscaler_addon.cpp) forwards the game's Direct3D calls, with
// native objects. Command lists are identified by their native deferred context or ID3D11CommandList.
namespace phantom_fx::upscaler
{
// The debug view that blends the ray-traced shadows' debug view over the image, with their probe.
constexpr int ShadowRaysView = 3;
// The debug view that blends the global illumination's denoiser history over the image.
constexpr int LightHistoryView = 4;
// The debug view that blends where each pixel's shadow rays leave over the image, with the shadows' probe.
constexpr int ShadowStartsView = 5;

struct Diagnostics
{
    int debugView{}; // 0 final image, 1 motion vectors, 2 where the engine's velocity was used, 3 the ray-traced shadows' rays (ShadowRaysView),
                     // 4 the global illumination's denoiser history (LightHistoryView), 5 where the shadow rays leave (ShadowStartsView)
    bool noJitter{};
    bool gameFxaa{}; // Comparison: leave the frame to the game's FXAA, keeping DLSS ready
    bool onlyScreenSpaceShadows{}; // The sun shadowed by the screen-space shadows alone
    bool showAmbientOcclusion{};   // GTVBAO's occlusion alone in place of the lit scene
    bool showIndirectLight{};      // GT-VBGI's light alone in place of the lit scene
    bool showGlobalIllumination{}; // The global illumination's light alone in place of the lit scene
    bool showTracedScene{};        // The global illumination's traced scene from the camera
    bool logGlobalIlluminationOften{}; // The global illumination's light logged every few frames, not every ten seconds
};

struct Status
{
    bool supportedBuild{}, failed{}, running{}, fxaaRecognised{}, upscalingAvailable{};
    std::string upscalerName; // "DLSS", or "FSR" with its version ("FSR 3.1.5") once it ran
    std::string failure, lastFallback;
    unsigned renderWidth{}, renderHeight{}, outputWidth{}, outputHeight{};
    std::uint64_t evaluations{}, fallbacks{}, resets{};
    float jitter[2]{};
    float mipBias{};
};

// ---- Setup ----
// `directory` is the add-on's, which holds PhantomFX.ini and the DLSS and FSR DLLs. On an unsupported
// game build upscaling stays off.
void initialize(const std::filesystem::path& directory, bool supportedBuild) noexcept;
// The game's device and immediate context; NGX receives `ngxContext`, ReShade's wrapper where there
// is one, so other add-ons can follow the evaluation.
void attach(ID3D11Device* device, ID3D11DeviceContext* immediate, ID3D11DeviceContext* ngxContext) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// The swap chain's size: the output size the upscaler upscales to.
void setOutputSize(unsigned width, unsigned height) noexcept;

// ---- Recording, on the game's render and worker threads ----
void shaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
// Notes the FXAA and camera velocity passes as they are recorded. Returns true when the draw was
// made on the UI target instead (the 2D pass and GammaControl while upscaling), or must not be made
// (FXAA, as DLSS runs before the grading), so the game's own draw must be skipped; the render
// target clear likewise.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept;
// Before the add-on's other draw hooks: notes the game's grading, which DLSS runs before. While
// upscaling, true for its full-size draws (the lens flare layer's composite and the grading), which
// are then bound to the output size (ui_redirect.hpp) and must be made in that state, whoever makes
// them; afterPostDraw then puts the game's bindings back.
bool beforePostDraw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept;
void afterPostDraw(ID3D11DeviceContext* context) noexcept;
bool clearTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* view, const float color[4]) noexcept;
void depthStencilCleared(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, UINT flags, float depth, UINT8 stencil) noexcept;
// Fills `biased` and returns true when any sampler should carry the texture LOD bias for the render size.
bool biasSamplers(ID3D11DeviceContext* context, unsigned count, ID3D11SamplerState* const* samplers, ID3D11SamplerState** biased) noexcept;
// A buffer mapped for writing, then unmapped with the data still mapped: the scene constants get
// the jitter here.
void bufferMapped(ID3D11Resource* buffer, void* data) noexcept;
void bufferUnmapped(ID3D11Resource* buffer) noexcept;
// What `from` recorded now belongs to `into`: FinishCommandList, or a command list executed in a
// deferred context.
void recorded(std::uint64_t into, std::uint64_t from) noexcept;
void forget(std::uint64_t list) noexcept;

// ---- Immediate context ----
// Before the immediate context executes `list`.
void executing(ID3D11DeviceContext* immediate, std::uint64_t list) noexcept;
void present() noexcept;

// ---- Frame generation (presenter.hpp) ----
// While the game presents through Direct3D 12: the upscaler's depth and motion are shared with it,
// and GammaControl also draws the frame before the HUD. Set at each present.
void setFrameGeneration(bool on) noexcept;
// What frame generation needs from the frame being presented, before present().
frame_generation::Inputs frameGenerationInputs() noexcept;

// ---- Settings UI (upscaler_menu.cpp) ----
Status status();
Settings currentSettings();
// Applies at once; PhantomFX.ini is written when the settings settle, or at detach().
void applySettings(const Settings& settings);
Diagnostics currentDiagnostics();
void applyDiagnostics(const Diagnostics& diagnostics);
// Soft shadows, screen-space shadows, GTVBAO and GT-VBGI together, for comparing with the game's own
// lighting (F1). Not saved: each start begins with them on, as their own settings say.
bool enhancementsOn();
void setEnhancements(bool on);
void toggleEnhancements();
void drawMenu();

// ---- Cost measurement (performance.hpp) ----
// Effects a measurement step runs without, while the settings, PhantomFX.ini and the overlay keep
// them on. Leaving out the upscaler shows the game's FXAA at the same render size.
struct LeftOut
{
    bool upscaler{}, sharpening{}, softShadows{}, sunScreenSpaceShadows{}, lightScreenSpaceShadows{}, rayTracedShadows{}, rayTracedLights{},
        ambientOcclusion{}, indirectLight{}, globalIllumination{}, volumetricFog{};
    // Part of the global illumination, left out before all of it: its light passes.
    bool globalLight{};
};
void leaveOut(const LeftOut& effects);
}
