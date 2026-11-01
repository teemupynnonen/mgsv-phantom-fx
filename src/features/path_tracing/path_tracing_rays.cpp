// Path tracing on Direct3D 12; path_tracing_rays.hpp describes it.
#include "features/path_tracing/path_tracing_rays.hpp"

#include "performance.hpp"

namespace phantom_fx::path_tracing_rays
{
using gi_d3d12::groups;
using gi_d3d12::Kernel;

void describe(gi_d3d12::Descriptors& d, const Textures& textures)
{
    if (textures.guideNormals) d.texture(18, textures.guideNormals, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (textures.guideSpecular) d.texture(19, textures.guideSpecular, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (textures.lampMasks) d.texture(20, textures.lampMasks, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (textures.guideMaterial) d.texture(21, textures.guideMaterial, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (textures.guideLobe) d.texture(22, textures.guideLobe, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (textures.specularTable) d.texture(25, textures.specularTable, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (textures.reflection) d.outputTexture(93, textures.reflection, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (textures.reflectionDistance) d.outputTexture(94, textures.reflectionDistance, DXGI_FORMAT_R16_FLOAT);
    if (textures.reflectionMap) d.outputTexture(98, textures.reflectionMap, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (textures.lampDiffuse) d.outputTexture(95, textures.lampDiffuse, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (textures.lampSpecular) d.outputTexture(96, textures.lampSpecular, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (textures.surfaceDepth) d.texture(26, textures.surfaceDepth, DXGI_FORMAT_R32_FLOAT);
    if (textures.surfaceNormal) d.texture(27, textures.surfaceNormal, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (textures.surfaceReflection) d.outputTexture(99, textures.surfaceReflection, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (textures.forwardShadow) d.outputTexture(97, textures.forwardShadow, DXGI_FORMAT_R16_FLOAT);
}

void recordLamps(gi_d3d12::Recorder& r, std::uint32_t width, std::uint32_t height)
{
    r.mark("path-traced lamps", performance::Pass::LampLight);
    r.dispatch(Kernel::TraceLampLight, groups(width, 8), groups(height, 8));
    r.barrier();
}

void recordReflections(gi_d3d12::Recorder& r, std::uint32_t width, std::uint32_t height)
{
    r.mark("path-traced reflections", performance::Pass::Reflections);
    r.dispatch(Kernel::TraceReflections, groups(width, 8), groups(height, 8));
    r.barrier();
}

void recordSurfaceReflections(gi_d3d12::Recorder& r, std::uint32_t width, std::uint32_t height)
{
    r.mark("glass reflections", performance::Pass::Reflections);
    r.dispatch(Kernel::TraceSurfaceReflections, groups(width, 8), groups(height, 8));
    r.barrier();
}
}
