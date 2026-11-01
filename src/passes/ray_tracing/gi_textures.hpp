#pragma once

#include "passes/ray_tracing/gi_renderer.hpp"

#include <d3d11.h>
#include <cstdint>
#include <string>
#include <vector>

// The game's textures for the ray-traced scene (docs/DEVELOPMENT.md, "Ray-traced scene"): shared copies
// of the textures of its models' meshes, taken from the game's texture manager
// (engine_scene::loadedTextures) whether or not the game draws them. A copy keeps the texture's levels of
// detail from the first no larger than MaxCutOutSize (a cut-out mesh's texture: its alpha shapes the
// shadows) or MaxColourSize (the others: what the rays' hits show). The game streams finer levels in by
// replacing a texture, so copies are looked at again now and then.
namespace phantom_fx::gi_textures
{
constexpr std::uint32_t MaxCutOutSize = 512, MaxColourSize = 256;

// How a mesh uses a texture: for its colour, as sRGB; a cut-out mesh's too, larger; or for data (its SRM, its normal
// map), as it is stored.
enum class Use : std::uint8_t
{
    Colour,
    CutOut,
    Data
};

// A model's mesh uses the texture with this path hash.
void want(std::uint64_t path, Use use);
// The renderer let these go (gi_renderer::Result::releasedTextures).
void release(const std::vector<std::uint64_t>& paths);
// On the immediate context, before the frame's inputs are signalled: copies what is newly loaded or
// replaced, a few a call, into `made`.
void update(ID3D11Device* device, ID3D11DeviceContext* context, std::vector<gi_renderer::SceneTexture>& made);
// The renderer starts over: every copy goes.
void reset();

// For the log.
std::string status();
}
