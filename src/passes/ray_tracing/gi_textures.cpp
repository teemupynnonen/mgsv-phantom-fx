// The game's textures for the ray-traced scene; gi_textures.hpp describes them.
#include "passes/ray_tracing/gi_textures.hpp"

#include "passes/ray_tracing/engine_scene.hpp"

#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <unordered_map>
#include <utility>

namespace phantom_fx::gi_textures
{
namespace
{
using Microsoft::WRL::ComPtr;

// The game's textures are looked up every LookFrames calls: the wanted ones not copied yet, and each
// copied one every RecheckFrames for a replacement. A call copies CopiesPerCall at most.
constexpr std::uint32_t LookFrames = 10, RecheckFrames = 300, CopiesPerCall = 96;

struct Copy
{
    bool cutOut{};
    bool isData{}; // Viewed as stored, not as sRGB
    std::uintptr_t source{}; // The game's texture it was copied from, to tell its replacement
    ComPtr<ID3D11Texture2D> texture;
    std::uint64_t bytes{};
    std::uint32_t nextLook{};
};

// On the render thread.
std::unordered_map<std::uint64_t, Copy> copies;
std::uint32_t calls{};

// Diagnostics, for the log on another thread.
std::atomic<std::uint64_t> copiedBytes{}, copiesMade{}, replacements{}, unusable{};
std::atomic<std::uint32_t> held{}, waiting{};
// The copies by format: BC1, BC2, BC3, BC4, BC5, BC7, 8-bit RGBA.
std::array<std::atomic<std::uint32_t>, 7> byFormat{};

// The typeless format a copy takes, so Direct3D 12 can view a base texture as sRGB as the game does,
// and that view's format.
std::pair<DXGI_FORMAT, DXGI_FORMAT> formatsOf(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB: return {DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM_SRGB};
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB: return {DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM_SRGB};
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB: return {DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM_SRGB};
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB: return {DXGI_FORMAT_BC7_TYPELESS, DXGI_FORMAT_BC7_UNORM_SRGB};
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return {DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    default: return {format, format};
    }
}

// The view of a texture holding data: the typeless format's plain one.
DXGI_FORMAT dataViewOf(DXGI_FORMAT format)
{
    switch (formatsOf(format).first)
    {
    case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC7_TYPELESS: return DXGI_FORMAT_BC7_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return format;
    }
}

// Bits a texel of a format takes; 0 for formats a copy cannot take.
std::uint32_t bitsOf(DXGI_FORMAT format)
{
    switch (formatsOf(format).first)
    {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM: return 4;
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC7_TYPELESS: return 8;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return 32;
    default: return 0;
    }
}

bool blockCompressed(DXGI_FORMAT format)
{
    return bitsOf(format) && bitsOf(format) <= 8;
}

// The source's levels of detail from the first no larger than the copy's limit into a shared copy;
// nullopt for a texture a copy cannot take (an array, a format the scene does not read).
std::optional<gi_renderer::SceneTexture> copyOf(ID3D11Device* device, ID3D11DeviceContext* context, std::uint64_t path, Copy& copy, ID3D11Texture2D* source)
{
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    const auto bits = bitsOf(desc.Format);
    if (desc.ArraySize != 1 || desc.SampleDesc.Count != 1 || !desc.MipLevels || !bits) return std::nullopt;

    const auto limit = copy.cutOut ? MaxCutOutSize : MaxColourSize;
    UINT first = 0;
    while (first + 1 < desc.MipLevels && std::max(desc.Width >> first, desc.Height >> first) > limit) ++first;

    // A block-compressed texture's most detailed level is whole blocks.
    const bool blocks = blockCompressed(desc.Format);
    while (first > 0 && blocks && (((desc.Width >> first) % 4) || ((desc.Height >> first) % 4))) --first;

    D3D11_TEXTURE2D_DESC wanted{};
    wanted.Width = std::max(desc.Width >> first, 1u);
    wanted.Height = std::max(desc.Height >> first, 1u);
    wanted.MipLevels = desc.MipLevels - first;
    wanted.ArraySize = 1;
    wanted.Format = formatsOf(desc.Format).first;
    wanted.SampleDesc.Count = 1;
    wanted.Usage = D3D11_USAGE_DEFAULT;
    wanted.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    wanted.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if (blocks && ((wanted.Width % 4) || (wanted.Height % 4))) return std::nullopt;

    ComPtr<ID3D11Texture2D> made;
    if (FAILED(device->CreateTexture2D(&wanted, nullptr, made.GetAddressOf()))) return std::nullopt;

    std::uint64_t bytes{};
    for (UINT mip = 0; mip < wanted.MipLevels; ++mip)
    {
        context->CopySubresourceRegion(made.Get(), mip, 0, 0, 0, source, first + mip, nullptr);
        bytes += std::uint64_t{std::max(wanted.Width >> mip, 1u)} * std::max(wanted.Height >> mip, 1u) * bits / 8;
    }

    copiedBytes += bytes;
    copiedBytes -= copy.bytes;
    switch (wanted.Format)
    {
    case DXGI_FORMAT_BC1_TYPELESS: ++byFormat[0]; break;
    case DXGI_FORMAT_BC2_TYPELESS: ++byFormat[1]; break;
    case DXGI_FORMAT_BC3_TYPELESS: ++byFormat[2]; break;
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM: ++byFormat[3]; break;
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM: ++byFormat[4]; break;
    case DXGI_FORMAT_BC7_TYPELESS: ++byFormat[5]; break;
    default: ++byFormat[6]; break;
    }
    copy.texture = made;
    copy.bytes = bytes;
    copy.source = reinterpret_cast<std::uintptr_t>(source);
    return gi_renderer::SceneTexture{path, made.Get(), copy.isData ? dataViewOf(desc.Format) : formatsOf(desc.Format).second, wanted.MipLevels};
}
}

void want(std::uint64_t path, Use use)
{
    if (!path) return;

    const bool isCutOut = use == Use::CutOut;
    auto [it, added] = copies.try_emplace(path);
    auto& copy = it->second;
    if (added) copy.isData = use == Use::Data;
    if (added || (isCutOut && !copy.cutOut))
    {
        // A cut-out mesh wants a larger copy than one only for colour.
        copy.cutOut |= isCutOut;
        copy.source = 0;
        copy.nextLook = calls;
    }
}

void release(const std::vector<std::uint64_t>& paths)
{
    for (const auto path : paths)
        if (const auto found = copies.find(path); found != copies.end())
        {
            copiedBytes -= found->second.bytes;
            copies.erase(found);
        }
}

void update(ID3D11Device* device, ID3D11DeviceContext* context, std::vector<gi_renderer::SceneTexture>& made)
{
    if (++calls % LookFrames) return;

    std::vector<std::uint64_t> due;
    std::uint32_t uncopied{};
    for (const auto& [path, copy] : copies)
    {
        if (!copy.texture) ++uncopied;
        if (copy.nextLook <= calls) due.push_back(path);
    }

    waiting = uncopied;
    held = static_cast<std::uint32_t>(copies.size() - uncopied);
    if (due.empty()) return;

    std::uint32_t copied{};
    for (const auto& loaded : engine_scene::loadedTextures(due))
    {
        auto& copy = copies[loaded.path];
        const auto source = reinterpret_cast<std::uintptr_t>(loaded.texture.Get());
        copy.nextLook = calls + RecheckFrames;
        if (source == copy.source) continue;

        // The rest wait for the next look.
        if (copied >= CopiesPerCall)
        {
            copy.nextLook = calls + LookFrames;
            continue;
        }

        const bool replacing = copy.texture != nullptr;
        if (const auto texture = copyOf(device, context, loaded.path, copy, loaded.texture.Get()))
        {
            made.push_back(*texture);
            ++copied;
            ++(replacing ? replacements : copiesMade);
        }
        else
        {
            // Never copied: looked at again only when replaced.
            copy.source = source;
            ++unusable;
        }
    }

    // Not loaded: looked up again soon.
    for (const auto path : due)
        if (auto& copy = copies[path]; copy.nextLook <= calls) copy.nextLook = calls + LookFrames;
}

void reset()
{
    copies.clear();
    copiedBytes = 0;
}

std::string status()
{
    return "the game's textures: " + std::to_string(held.load()) + " copied (" + std::to_string(copiedBytes.load() >> 20) + " MB), " + std::to_string(waiting.load())
        + " not loaded yet, " + std::to_string(copiesMade.load()) + " copies and " + std::to_string(replacements.load()) + " replacements made, "
        + std::to_string(unusable.load()) + " in formats the scene does not read; copies made by format: BC1 " + std::to_string(byFormat[0].load()) + ", BC2 "
        + std::to_string(byFormat[1].load()) + ", BC3 " + std::to_string(byFormat[2].load()) + ", BC4 " + std::to_string(byFormat[3].load()) + ", BC5 "
        + std::to_string(byFormat[4].load()) + ", BC7 " + std::to_string(byFormat[5].load()) + ", RGBA " + std::to_string(byFormat[6].load());
}
}
