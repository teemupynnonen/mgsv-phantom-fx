#pragma once

#include "game_lights.hpp"
#include "phantom_fx_settings.hpp"

#include <d3d11.h>
#include <optional>

// The lamps' soft shadows (docs/DEVELOPMENT.md, "Lamp soft shadows"), drawn by the local light pass
// (local_light_pass.hpp). A point or spot light with a shadow map lights with a rebuilt copy of the
// game's shader, which filters that map with contact-hardening soft shadows (PCSS) in place of the
// game's four evenly sharp comparisons. Only the quartic, specular, shadow-mapped lights; every other
// keeps the game's shader.
namespace phantom_fx::lamp_soft_shadows
{
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Makes the shaders (precompiled; compiled from source in a dev build) while the effect runs, before its first frame.
void configure(const Configuration& configuration) noexcept;
// For the budget: unavailable when its shaders did not compile (see PhantomFX.log).
Health health();

// What a lighting draw binds in place of the game's shader: the replacement, and its options.
struct Replacement
{
    static constexpr UINT OptionsSlot = 6; // b6 of the replacement shaders

    ID3D11PixelShader* shader;
    ID3D11Buffer* options;
};

// ---- Recording, on the game's render and worker threads ----
// The replacement of `light`'s shader; none when the soft shadows do not replace it, or are unavailable.
std::optional<Replacement> replacementFor(const game_lights::GameLight& light);
}
