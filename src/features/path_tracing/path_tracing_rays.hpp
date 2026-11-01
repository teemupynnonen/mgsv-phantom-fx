#pragma once

#include "passes/ray_tracing/gi_d3d12.hpp"

#include <d3d12.h>
#include <cstdint>

// Path tracing on the ray-traced scene's Direct3D 12 device (docs/DEVELOPMENT.md, "Path tracing"): the
// reflections' rays (reflections.hlsl) and the point and spot lights' (direct_light.hlsl).
namespace phantom_fx::path_tracing_rays
{
// The shared textures the passes read and write, as gi_renderer.cpp opened them; null for a pass not traced.
struct Textures
{
    ID3D12Resource* guideNormals{};
    ID3D12Resource* guideSpecular{};
    ID3D12Resource* guideMaterial{};
    ID3D12Resource* guideLobe{};
    ID3D12Resource* specularTable{};
    ID3D12Resource* lampMasks{}; // Null without masked spots
    ID3D12Resource* reflection{};
    ID3D12Resource* reflectionDistance{};
    ID3D12Resource* reflectionMap{};
    ID3D12Resource* lampDiffuse{};
    ID3D12Resource* lampSpecular{};
    ID3D12Resource* surfaceDepth{}; // Glass and mirrors (forward_surfaces.hpp); null while not traced
    ID3D12Resource* surfaceNormal{};
    ID3D12Resource* surfaceReflection{};
    // The rivers' and liquids' guides (forward_shadows) and their own sun shadow, a frame old; null without.
    ID3D12Resource* forwardShadow{};  // R16_FLOAT, the sun's shadow on the surface itself (u100)
};

// Their views in the frame's tables.
void describe(gi_d3d12::Descriptors& descriptors, const Textures& textures);
// The lamps' light at the render size. Needs the frame's scene built.
void recordLamps(gi_d3d12::Recorder& recorder, std::uint32_t width, std::uint32_t height);
// The reflections, which read the light's cache: after its passes.
void recordReflections(gi_d3d12::Recorder& recorder, std::uint32_t width, std::uint32_t height);
// Those of glass and mirrors: in the frame's tables, after the game drew them.
void recordSurfaceReflections(gi_d3d12::Recorder& recorder, std::uint32_t width, std::uint32_t height);
}
