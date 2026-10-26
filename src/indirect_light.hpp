#pragma once

#include "ui_redirect.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <cstddef>
#include <string>

// What the add-on does to the game's indirect light (docs/DEVELOPMENT.md, "Ambient occlusion",
// "GTVBAO" and "GT-VBGI"). Both share one visibility bitmask march.
//
// GTVBAO, ground-truth visibility bitmask ambient occlusion, replaces the game's SSAO. Fox Engine
// multiplies all of a pixel's light by its occlusion in the deferred resolve, sunlight included.
// GTVBAO multiplies the indirect light only: the diffuse light buffer right after the engine
// upscales its probe and sky lighting into it, before the sun and the local lights add to it. While
// it is on, the resolve reads a white texture in place of the game's occlusion, and the game's own
// SSAO passes are left out.
//
// GT-VBGI, ground-truth visibility bitmask global illumination (Mirko Salm's extension of GT-VBAO to
// diffuse bounces), adds the light that nearby surfaces on screen reflect onto each other: at the
// resolve, once the sun and the local lights are in the diffuse light, the resolve reads the diffuse
// light with that bounce added.
//
// The global illumination (global_illumination.hpp) replaces the game's probe light and SSAO: the
// upscale writes only its alpha, the SSAO passes are left out, and the resolve reads the diffuse
// light with the traced light added and white for the occlusion. GTVBAO and GT-VBGI are off then.
namespace phantom_fx::indirect_light
{
struct Options
{
    bool occlusion{};           // GTVBAO
    float occlusionRadius{1};   // Metres
    float occlusionStrength{1}; // Exponent on the visibility
    bool light{};               // GT-VBGI
    float lightRadius{2};       // Metres
    float lightStrength{1};     // Multiplies the light
    bool globalIllumination{};  // The traced light in place of the game's probes and SSAO
    float globalIlluminationStrength{1}; // Multiplies the traced light
    // The ray-traced scene runs, for the global illumination or the ray-traced shadows: the passes
    // tell it what it needs (global_illumination::noteNormals and the others).
    bool rayTracing{};
    bool showOcclusion{};       // Diagnostics: the resolve draws GTVBAO's occlusion alone
    bool showLight{};           // Diagnostics: the resolve draws GT-VBGI's light alone
    bool showGlobalIllumination{}; // Diagnostics: the resolve draws the global illumination's output alone
};

void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Options& options) noexcept;
// Empty while the effect is off or running; otherwise what it waits for, for the overlay.
std::string occlusionProblem();
std::string lightProblem();

// ---- Recording, on the game's render and worker threads ----
void shaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
// Before a draw. Returns true when the add-on made the draw itself, or left it out, so the game's
// draw must be skipped.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept;
// Each present. The game records a whole frame between presents, so every pass of a frame sees the
// same options.
void present() noexcept;

// The G-buffer's specular target as the last sun pass bound it (its material in .z, 43 for foliage), if
// it is `width` x `height`; null otherwise. The view is a frame old; the target is the frame's.
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> specularTarget(UINT width, UINT height) noexcept;
}
