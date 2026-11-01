// Reads the compiled kernels' bindings through DXC's reflection and writes the headers kernel_table.hpp describes.
//
// kernel_bindings <dxcompiler.dll> <folder> <kernel>...
//
// Each kernel's <folder>/<kernel>.dxil is read; the headers go into <folder>. The reflection API, not DXC's text listing:
// that one is for people, and runs its columns together (a unorm UAV reads "UAVunorm_f32").
#include "kernel_table.hpp"

#include <Windows.h>
#include <d3d12shader.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace phantom_fx::kernel_table;
using Microsoft::WRL::ComPtr;

void check(HRESULT result, const std::string& what)
{
    if (FAILED(result)) throw std::runtime_error(what + " failed.");
}

class Reflection
{
public:
    explicit Reflection(const std::filesystem::path& dxcompiler)
    {
        library = LoadLibraryW(dxcompiler.c_str());
        if (!library) throw std::runtime_error("Could not load " + dxcompiler.string() + ".");

        const auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(library, "DxcCreateInstance"));
        if (!create) throw std::runtime_error(dxcompiler.string() + " has no DxcCreateInstance.");

        check(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "Creating DXC's utilities");
    }

    std::vector<BoundResource> resourcesOf(const std::filesystem::path& kernel) const
    {
        std::ifstream file(kernel, std::ios::binary);
        if (!file) throw std::runtime_error("Could not read " + kernel.string() + ".");

        const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        const DxcBuffer buffer{bytes.data(), bytes.size(), DXC_CP_ACP};
        ComPtr<ID3D12ShaderReflection> reflection;
        check(utils->CreateReflection(&buffer, IID_PPV_ARGS(&reflection)), "Reflecting " + kernel.string());

        D3D12_SHADER_DESC shader{};
        check(reflection->GetDesc(&shader), "Reading " + kernel.string());
        std::vector<BoundResource> resources;
        for (UINT i = 0; i < shader.BoundResources; ++i)
        {
            D3D12_SHADER_INPUT_BIND_DESC bound{};
            check(reflection->GetResourceBindingDesc(i, &bound), "Reading " + kernel.string());
            resources.push_back({bound.Name, bound.Type, bound.Dimension, bound.BindPoint, bound.BindCount, bound.Space});
        }

        return resources;
    }

private:
    HMODULE library{};
    ComPtr<IDxcUtils> utils;
};

void write(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary);
    file << text;
    if (!file) throw std::runtime_error("Could not write " + path.string() + ".");
}
}

int main(int count, char** arguments)
{
    try
    {
        if (count < 4) throw std::runtime_error("Usage: kernel_bindings <dxcompiler.dll> <folder> <kernel>...");

        const std::filesystem::path folder = arguments[2];
        const std::vector<std::string> names(arguments + 3, arguments + count);
        const Reflection reflection(arguments[1]);
        std::vector<CompiledKernel> kernels;
        for (const auto& name : names) kernels.push_back({name, reflection.resourcesOf(folder / (name + ".dxil"))});

        write(folder / "gi_bindings.h", bindingsHeader(kernels));
        write(folder / "gi_kernel_list.h", kernelListHeader(names));
        write(folder / "gi_kernel_bytecode.h", kernelBytecodeHeader(names));

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "kernel_bindings: " << error.what() << '\n';

        return 1;
    }
}
