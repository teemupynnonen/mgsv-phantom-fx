#include "features/path_tracing/surface_shaders.hpp"
#include "features/water_caustics/caustics_map.hpp"
#include "features/water_caustics/water_caustics_shaders.hpp"
#include "passes/forward_scene.hpp"

#include <Windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

// A shader these features compile at run time that fails only turns its feature off, with a line in the log.
namespace
{
using namespace phantom_fx;
using Microsoft::WRL::ComPtr;

void expect(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void compiles(const std::string& source, const char* target, const std::vector<D3D_SHADER_MACRO>& defines = {{}})
{
    ComPtr<ID3DBlob> code, errors;
    const auto result = D3DCompile(source.data(), source.size(), "test", defines.data(), nullptr, "main", target, D3DCOMPILE_WARNINGS_ARE_ERRORS, 0,
        code.GetAddressOf(), errors.GetAddressOf());
    expect(SUCCEEDED(result), errors ? static_cast<const char*>(errors->GetBufferPointer()) : "Compile a shader.");
}

void surfaceShadersCompile()
{
    const auto common = std::string(forward_scene::Common) + surface_shaders::SurfaceOutput;

    compiles(common + surface_shaders::GlassSurface, "ps_5_0");
    compiles(common + surface_shaders::MirrorSurface, "ps_5_0");
    compiles(surface_shaders::Composite, "ps_5_0");
    compiles(surface_shaders::ReconstructionGuides, "cs_5_0");
    for (const auto* layered : {"0", "1"})
        for (const auto* twoChannel : {"0", "1"})
            for (const auto* ripples : {"0", "1"})
                compiles(common + surface_shaders::WaterSurface, "ps_5_0", {{"LAYERED", layered}, {"TWO_CHANNEL", twoChannel}, {"RIPPLES", ripples}, {}});
}

void causticsCompile()
{
    compiles(std::string(forward_scene::Common) + water_caustics::shaders::Caustics, "ps_5_0");
}

// How much `map` changes over a step between neighbouring rows, on average, and across its seam.
void causticsMapTiles()
{
    const auto map = water_caustics::makeCausticsMap();
    constexpr int Size = water_caustics::CausticsMapSize;
    expect(map.size() == Size * Size, "A map of its size.");

    const auto stepBetween = [&](int rowA, int rowB) {
        long long total = 0;
        for (int x = 0; x < Size; ++x) total += std::abs(int{map[rowA * Size + x]} - int{map[rowB * Size + x]});

        return static_cast<double>(total) / Size;
    };

    double inside = 0;
    for (int y = 0; y + 1 < Size; ++y) inside += stepBetween(y, y + 1);
    inside /= Size - 1;
    expect(stepBetween(Size - 1, 0) < 2 * inside, "The map wraps with no seam.");

    const auto mean = std::accumulate(map.begin(), map.end(), 0.0) / static_cast<double>(map.size());
    expect(mean > 30 && mean < 80, "Dark cells, with thin bright edges.");
}
}

int main()
{
    try
    {
        surfaceShadersCompile();
        causticsCompile();
        causticsMapTiles();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "Water tests passed.\n";
    return 0;
}
