#pragma once

#include <d3d11.h>
#include <cstddef>
#include <cstdint>
#include <vector>

// The game's light volumes reach its 24 light vertex shaders (one code, a name each) already in clip
// space, worked out on the CPU from its own camera, so the upscaler's jitter, which the add-on puts into
// the scene constants only, missed them while the depth they test against had it. A wall lamp whose
// volume lies on the wall then lost or kept its light by the frame's jitter: a flicker that grew as the
// render scale fell. Those shaders are made with the jitter (cameraCenterOffset, b2 c28) added to the
// volume. docs/DEVELOPMENT.md, "Ray-traced lights".
namespace phantom_fx::game_light_volumes
{
// A vertex shader about to be made: the code to make it from instead, kept for the session, or null to
// keep the code given.
const std::vector<std::uint8_t>* jittered(ID3D11Device* device, const void* code, std::size_t size) noexcept;
}
