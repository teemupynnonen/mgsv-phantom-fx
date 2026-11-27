#pragma once

#include "game_shaders.hpp"
#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// The scene in float, and HDR10 output (docs/DEVELOPMENT.md, "HDR"). Fox Engine tonemaps early, in the
// deferred resolve and in the sky, clouds and ocean, and everything after worked on that tonemapped image
// in 8 bits: glass and particles, the reflections, bloom, the depth of field, the grading, FXAA and the
// HUD. On a supported build the scene keeps its light until the grading, in SDR too, where the 8 bits
// showed as bands in smooth fog:
//
// - With the scene in float, the swap chain becomes R10G10B10A2 with flip model, so HDR output can turn
//   on and off without making it again: in the HDR10 colour space (PQ, BT.2020) with HDR output, sRGB
//   otherwise.
// - The scene target and its copy, which the renderer makes at known places, become 16-bit float.
//   They hold sRGB-encoded colour, which 8-bit formats encode and decode in their _SRGB views; the
//   float targets keep the encoded values, so a draw through such a view (the resolve) is encoded
//   after it, and reads through one (the reflections, from the copy) get a decoded twin.
// - The game's shaders with TppTonemap get it taken out as they are made; the sky's clamp to 1 after
//   it stays with HDR output only.
// - What expects the tonemapped image gets it: bloom's quarter-size copy is tonemapped as it is made,
//   and the depth of field composites over a float copy of the scene.
// - The grading pass tonemaps for the game's grading; with HDR output it then adds back the light the
//   tonemap took.
// - GammaControl, which wrote the back buffer, applies the game's brightness and maps the light to
//   the display: with HDR output, its range; in SDR, sRGB in 10 bits with a dither of half a step,
//   which hides the steps left.
// - The 2D pass's writes are clamped to 0 to 1, as the 8-bit target clamped them.
//
// DLSS runs in its HDR mode on the light (upscaler.cpp).
namespace phantom_fx::hdr
{
// What the session keeps of the scene's light: nothing (the game's 8-bit scene), the float scene with
// SDR output, or the float scene with HDR output.
enum class Mode : std::uint8_t { Off, FloatScene, HdrOutput };

// What the swap chain shows: HDR10, sRGB when the display has HDR off, or nothing of HDR's because
// it is off or could not start.
enum class Output : std::uint8_t { Off, Hdr10, Sdr, Unavailable };

// ---- Setup ----
// At start, before the game makes its swap chain and targets. A change takes a restart.
void enable(Mode mode) noexcept;
// The scene keeps its light in 16-bit float until the grading this session.
bool isSceneFloat() noexcept;
// HDR output runs this session.
bool active() noexcept;
Output output() noexcept;
// The peak brightness HDR maps to for a chosen one, in nits: 0 takes what the display reports.
float peakNitsFor(float chosen) noexcept;
// Checks the device can keep the scene in float; it and HDR stay off for the session when it cannot.
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Applies from the next frame.
void configure(const Configuration& configuration) noexcept;

// ---- Creation ----
// The game's swap chain, before it is made or resized: R10G10B10A2 with flip model while the scene is
// float, whether HDR output is on or not. Returns true when the description changed.
bool upgradeSwapChain(DXGI_FORMAT& format, DXGI_SWAP_EFFECT& effect, UINT& bufferCount) noexcept;
// The swap chain was made R10G10B10A2 this session.
bool tenBitBackBuffer() noexcept;
// After the swap chain was made or resized: HDR10 when its display shows HDR, sRGB otherwise.
void swapChainReady(IDXGISwapChain* chain) noexcept;
// A 2D texture about to be made: the scene target and its copy become 16-bit float, with unordered
// access. Returns true when the description changed; textureCreated follows on the same thread.
bool upgradeTexture(DXGI_FORMAT& format, bool& unorderedAccess) noexcept;
void textureCreated(ID3D11Resource* texture) noexcept;
void resourceDestroyed(ID3D11Resource* resource) noexcept;
// A view about to be made: views of the upgraded targets and the back buffer take their format.
// Returns true when the description changed; viewCreated follows on the same thread.
bool upgradeView(ID3D11Resource* resource, DXGI_FORMAT& format) noexcept;
void viewCreated(ID3D11View* view) noexcept;
void viewDestroyed(ID3D11View* view) noexcept;

// A pixel shader about to be made: TppTonemap taken out, or for the 2D pass, which blends with values
// beyond 0 to 1 that only the 8-bit target clamped, its writes saturated. Returns the code to make the
// shader from instead, or null to keep the game's.
const std::vector<std::uint8_t>* patchedShader(const void* code, std::size_t size) noexcept;
// The game's own code for code patchedShader returned, which the add-on's parts identify shaders by;
// other code as it is.
std::pair<const void*, std::size_t> originalOf(const void* code, std::size_t size) noexcept;

// ---- Recording, on the game's render and worker threads ----
// Before a draw, whoever draws it. Keeps a resolve's tonemap curve, copies the scene for the depth
// of field and gives its composite that copy. True when the draw writes through an sRGB view of an
// upgraded target, which encode() must follow.
bool beforeDraw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
// Encodes what the draw just made wrote to the render target bound.
void encode(ID3D11DeviceContext* context) noexcept;
// The grading pass and the reduction for bloom are drawn here; returns true when the game's draw must
// be skipped.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
// A clear through an sRGB view of an upgraded target, made here with the colour encoded. Returns
// true when the game's clear must be skipped.
bool clearTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* view, const float color[4]) noexcept;
// CopyResource into an upgraded target whose decoded twin is read, made here and followed by the
// decoding. Returns true when the game's copy must be skipped.
bool copy(ID3D11DeviceContext* context, ID3D11Resource* source, ID3D11Resource* destination) noexcept;
// Pixel shader views just bound: fills `replaced` and returns true when an sRGB view of an upgraded
// target is to be replaced by its twin.
bool replaceViews(unsigned count, ID3D11ShaderResourceView* const* views, ID3D11ShaderResourceView** replaced) noexcept;

// While GammaControl draws: the output shader in place of the game's, while the scene is float.
class OutputShader
{
public:
    explicit OutputShader(ID3D11DeviceContext* context) noexcept;
    ~OutputShader();
    OutputShader(const OutputShader&) = delete;
    OutputShader& operator=(const OutputShader&) = delete;

    bool bound() const noexcept { return context != nullptr; }

private:
    ID3D11DeviceContext* context{};
    Microsoft::WRL::ComPtr<ID3D11PixelShader> gameShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> gameBuffer;
};

// The format the scene target has this session.
DXGI_FORMAT sceneFormat() noexcept;
// The scene as the depth of field's composite last found it: its float copy, sRGB-encoded as the scene.
// None unless it is `width` x `height`.
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sceneBeforeDof(UINT width, UINT height) noexcept;
}
