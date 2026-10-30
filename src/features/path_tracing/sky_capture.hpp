#pragma once

#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>

// The sky as the rays see it (docs/DEVELOPMENT.md, "Path tracing"): while path tracing runs, the game's own sky draws
// (the sky, its painted dome and its clouds, or the physical sky in their place) are drawn a second time, a cube face a
// frame, from the eye into a small cube, with the camera turned to that face. What a ray that gets away reads is then
// the sky the game shows, on every side: its horizon, clouds, sun's glow, moon and weather, which the sky's light
// (nine numbers) cannot hold.
namespace phantom_fx::sky_capture
{
void attach(ID3D11Device* device) noexcept;
void detach() noexcept;
void configure(const Configuration& configuration) noexcept;
void present() noexcept;

// Whether the game's sky draws are drawn here too, while path tracing runs.
bool isWanted() noexcept;

// Right after a sky draw, with all its bindings still set: draws it again into this frame's face, and puts them back.
void capture(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept;

// On the immediate context, before the frame's inputs are signalled to Direct3D 12: the last frame's face in the rays'
// light units, `lightScale` of the game's. The cube for Direct3D 12 (six slices, R16G16B16A16_FLOAT), or null until every
// face is drawn or while the game draws no sky: the rays then take the sky's light.
ID3D11Texture2D* update(ID3D11DeviceContext* immediate, float lightScale) noexcept;
}
