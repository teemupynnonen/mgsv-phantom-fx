#pragma once

#include "ui_redirect.hpp"

#include <d3d11.h>
#include <filesystem>

// A debug case's reference for the rays' shadows (automation.hpp): while kept, each main sun pass also draws what the
// game's shadow map alone says of each pixel, R16_FLOAT, 1 lit, 0 blocked, -1 where the pass draws nothing or has no map.
namespace phantom_fx::game_shadow
{
void keep(bool isKept) noexcept;
bool isKept() noexcept;
// With the sun pass's state and inputs bound and its options all off, before its own draw, and with its resource lock
// held. Leaves the pixel shader changed. A failure only loses the reference, and turns keeping off.
void draw(ID3D11Device* device, ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, UINT width, UINT height) noexcept;
void save(ID3D11DeviceContext* context, const std::filesystem::path& file) noexcept;
// With the sun pass's resource lock held.
void detach() noexcept;
}
