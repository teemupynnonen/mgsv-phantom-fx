#pragma once

#include "game_shaders.hpp"
#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <wrl/client.h>

// The game's indirect light passes (docs/DEVELOPMENT.md, "Ambient occlusion"): the sky light pass binds
// the G-buffer normals, the upscale writes the indirect light into the diffuse light buffer, and the
// deferred resolve multiplies all light by the game's occlusion. The pass notes what the effects need
// from them and hands each the pass it changes:
//
// - GTVBAO's occlusion multiplies into the upscale, and the resolve reads white in place of the game's
//   SSAO, which is skipped (gtvbao.hpp).
// - SSGI's light joins the diffuse light the resolve reads (ssgi.hpp).
// - The global illumination's light takes the game's probe light's place in it (light_combine.hpp),
//   and its rays take the G-buffer and the frame's kick from these passes (ray_tracing.hpp).
namespace phantom_fx::indirect_light_pass
{
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Configuration& configuration) noexcept;
// The pass's own shaders did not compile (see PhantomFX.log).
bool failed() noexcept;
// The G-buffer's specular target (its material in .z, 43 for foliage), as the game last bound it, if it is
// `width` x `height`; null otherwise.
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> specularTarget(UINT width, UINT height) noexcept;
// The game's tables a sun pass reads the specular target's material by: the specular lobe by the halfway
// vector's angle, the roughness and the anisotropy (g_lut2), the Fresnel term by angle and F0 (g_lut1), and each
// material's specular colour and F0 (g_MaterialTexture). As the last sun pass bound them; null before one.
struct MaterialTables
{
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lobe, fresnel, materials;
};
MaterialTables materialTables() noexcept;

// ---- Recording, on the game's render and worker threads ----
// A context's render targets were bound: the G-buffer's gives its specular target.
void targetsBound(UINT count, ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* depth) noexcept;
// Before a draw, whoever draws it: what the ray-traced scene needs (ray_tracing::noteNormals and the
// others), the G-buffer's normals, and with the global illumination the game's probe light, drawn aside
// for the light to replace.
void note(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
// The upscale and the resolve, while an effect changes them, are drawn here instead, and the game's
// SSAO is skipped while one replaces it; returns true when the game's draw must be skipped.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
// Each present. The game records a whole frame between presents, so all of a frame's passes see the
// same effects.
void present() noexcept;
}
