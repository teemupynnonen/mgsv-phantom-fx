#pragma once

#include <dxgiformat.h>
#include <cstddef>
#include <filesystem>
#include <string>

struct ID3D11DeviceContext;
struct ID3D11Resource;

// Text and file forms of Direct3D 11 resources for the DevKit's frame log (devkit/frame_log.hpp).
namespace phantom_fx
{
std::string formatName(DXGI_FORMAT format);
// Constant buffer contents as "c<n>: x y z w" lines, one float4 register per line.
std::string constantRows(const void* data, std::size_t size);
// Copies the first mip of a 2D texture through a staging texture and saves it as a DDS file in
// `format` (the view's format, as typeless resources have no colour meaning). Waits for the GPU,
// so only for diagnostics. Returns false for textures it cannot save (multisampled, compressed).
bool saveTexture(ID3D11DeviceContext* context, ID3D11Resource* resource, DXGI_FORMAT format, const std::filesystem::path& file);
}
