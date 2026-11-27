#pragma once

#include "d3d11_helpers.hpp"
#include "game_constants.hpp"
#include "phantom_fx_settings.hpp"

#include <d3d11.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// The visibility bitmask march (docs/DEVELOPMENT.md, "Ambient occlusion", "Indirect lighting"): XeGTAO's
// depth prefilter, then for each pixel the sectors of the hemisphere that depth samples cover, then
// XeGTAO's edge-aware denoiser. GTVBAO (gtvbao.hpp) marches for the occlusion, SSILVB (ssilvb.hpp) for
// the light the sectors receive.
namespace phantom_fx::visibility_bitmask
{
// The compute shaders' constant buffer (PHANTOM_FX_MARCH_PARAMETERS in shaders/visibility_march.hlsli),
// which SSILVB's upsample and the global illumination's combine read too.
struct Parameters
{
    std::uint32_t size[2];
    float radius, maxRadiusPixels, occlusionStrength, lightStrength, diffuseScale, padding;
};

enum class Mode : std::uint8_t { Occlusion, Light };

using Target = ComputeTexture;

// The march writes results[0]; the denoise passes alternate between the two, and the last writes
// results[Result].
constexpr int DenoisePasses = 2;
constexpr std::size_t Result = DenoisePasses % 2;
// The game's compute slots the passes bind, saved around them.
constexpr ComputeSlots PassSlots{.buffers = 5, .views = 5, .accesses = 5};
// The march stops at this share of the render height, for surfaces close to the camera.
constexpr float MaxRadiusShare = 0.15f;
// The scene constants the march reads (b1): the game's cPSScene, in bytes.
constexpr UINT SceneConstantsSize = sizeof(game_constants::SceneConstants);

// When an effect last ran, and otherwise what it waits for, for the overlay.
struct Progress
{
    const char* name;
    std::uint64_t lastRun{}; // GetTickCount64
    std::string waitingFor;

    // Leaves the game's pass alone this frame: false.
    bool wait(std::string reason);
    void ran();
    // Unavailable when `failed` (its shaders did not compile), else waiting unless it ran within a second.
    Health health(bool failed) const;
};

// Makes `target` at the size on `device`, unless it has it; command lists that still use the previous
// texture hold their own references. True when it made one.
bool ensureTarget(ID3D11Device* device, Target& target, UINT width, UINT height, DXGI_FORMAT format, const char* what);

void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Makes the shaders while GTVBAO or SSILVB runs, before their first frame.
void configure(const Configuration& configuration) noexcept;
// Makes the shaders once, from their precompiled bytecode (compiled from source in a dev build); false when they are unavailable (see PhantomFX.log).
bool ready() noexcept;
bool failed() noexcept;

// Records the prefilter, the march and the denoise passes on `context`, within a SavedComputeState of
// PassSlots: `parameters` in b0 and the game's `scene` constants in b1, which stay bound for a pass
// that follows. From `depth` and `normals`, and for the light `diffuse` and `albedo`; `specular`
// (may be null) tells foliage. The march writes results[0] and `edges`, at their size; the denoised
// visibility or light ends in results[Result].
void march(ID3D11DeviceContext* context, Mode mode, const Parameters& parameters, ID3D11Buffer* scene, ID3D11ShaderResourceView* depth,
    ID3D11ShaderResourceView* normals, ID3D11ShaderResourceView* diffuse, ID3D11ShaderResourceView* albedo, ID3D11ShaderResourceView* specular,
    std::array<Target, 2>& results, Target& edges);
}
