#pragma once

#include "game_shaders.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>

// Glass and mirrors under path tracing (docs/DEVELOPMENT.md, "Forward surfaces: rivers, liquids and glass"). They are
// drawn forward, after the G-buffer, so the kick's rays know nothing of them; and most are drawn among the particles,
// after the game's reflection blend. While the reflections are traced, each of their draws leaves out what the game
// reflects with - a small cube map or a mirror's painted picture - and is drawn again into the shared surface targets
// (forward_surfaces), which the second, short trace reflects after the scene.
//
// So whatever the game draws reflects, its shapes and normals its own: a character's visor needs no pose from the rays.
namespace phantom_fx::glass
{
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;

// A glass or mirror draw, drawn here while the reflections are traced; returns true when the game's draw must be skipped.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
}
