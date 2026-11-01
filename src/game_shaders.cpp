#include "game_shaders.hpp"

#include "log.hpp"
#include "shader_bytecode.hpp"

#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace phantom_fx::game_shaders
{
namespace
{
using Microsoft::WRL::ComPtr;

// The shaders the game created, by the object it binds, and the first of each row. Holding them keeps
// their addresses from being reused.
std::shared_mutex mutex;
std::unordered_map<IUnknown*, const GameShader*> rows;
std::array<ComPtr<ID3D11PixelShader>, std::size(GameShaders)> firsts;
std::unordered_map<IUnknown*, VertexShader> vertexShaders;
std::vector<ComPtr<IUnknown>> owners;

const GameShader* rowOf(std::string_view checksum)
{
    const auto found = std::ranges::find_if(GameShaders, [&](const GameShader& row) { return checksum == row.checksum; });

    return found != std::end(GameShaders) ? found : nullptr;
}

std::size_t indexOf(const GameShader& row)
{
    return static_cast<std::size_t>(&row - std::begin(GameShaders));
}

VertexShader kindOf(std::string_view code)
{
    if (draw2DSource(code, "_vs.hlsl")) return VertexShader::Draw2D;

    const auto name = sourceName(code, "_vs.hlsl");
    if (name.starts_with("NewTerrain_Depth") || name.starts_with("NewTerrain_Shadow")) return VertexShader::Terrain;
    if (name.starts_with("fox3ddf_") || name.starts_with("tpp3ddf_") || name.starts_with("ModelForward_Shadow")) return VertexShader::Model;

    return VertexShader::Other;
}
}

void pixelShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    const auto checksum = dxbcHashOf(code, size);
    if (!checksum || !shader) return;

    try
    {
        const auto* row = rowOf(toHex(*checksum));
        if (!row) return;

        bool isFirst{};
        {
            std::unique_lock lock(mutex);
            auto& first = firsts[indexOf(*row)];
            isFirst = !first;
            if (isFirst) shader->QueryInterface(IID_PPV_ARGS(first.GetAddressOf()));

            rows[shader] = row;
            owners.emplace_back(shader);
        }

        if (isFirst) log(std::string("Identified ") + row->name + ".");
    }
    catch (...)
    {
    }
}

void vertexShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    if (!code || !shader) return;

    const auto kind = kindOf(std::string_view(static_cast<const char*>(code), size));
    if (kind == VertexShader::Other) return;

    try
    {
        std::unique_lock lock(mutex);
        vertexShaders[shader] = kind;
        owners.emplace_back(shader);
    }
    catch (...)
    {
    }
}

VertexShader vertexShaderOf(IUnknown* shader) noexcept
{
    if (!shader) return VertexShader::Other;

    std::shared_lock lock(mutex);
    const auto found = vertexShaders.find(shader);

    return found != vertexShaders.end() ? found->second : VertexShader::Other;
}

ID3D11PixelShader* firstCreated(const GameShader& row) noexcept
{
    std::shared_lock lock(mutex);
    return firsts[indexOf(row)].Get();
}

void forget() noexcept
{
    std::unique_lock lock(mutex);
    rows.clear();
    vertexShaders.clear();
    for (auto& first : firsts) first.Reset();
    owners.clear();
}

const GameShader* DrawnShader::row() noexcept
{
    lookUp();
    return found;
}

ID3D11PixelShader* DrawnShader::object() noexcept
{
    lookUp();
    return shader.Get();
}

void DrawnShader::lookUp() noexcept
{
    if (isLookedUp) return;

    isLookedUp = true;
    context->PSGetShader(shader.GetAddressOf(), nullptr, nullptr);
    if (!shader) return;

    std::shared_lock lock(mutex);
    if (const auto known = rows.find(shader.Get()); known != rows.end()) found = known->second;
}
}
