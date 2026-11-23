// Development builds only: compiles a shader's .hlsl from the checkout, so an edit needs no rebuild.
// A release build uses the bytecode phantom_precompile_shader made at build time.
#include "d3d11_helpers.hpp"

#include "log.hpp"

#include <d3dcompiler.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace phantom_fx
{
namespace
{
namespace fs = std::filesystem;

// Opens a shader's #includes from its own directory, then from src/.
class CheckoutIncludes final : public ID3DInclude
{
public:
    explicit CheckoutIncludes(std::vector<fs::path> roots) : directories(std::move(roots)) {}

    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* size) override
    {
        for (std::size_t i = 0; i < directories.size(); ++i)
        {
            const auto path = (directories[i] / name).lexically_normal();
            std::ifstream in(path, std::ios::binary);
            if (!in) continue;

            auto text = std::make_unique<std::string>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            directories.push_back(path.parent_path());
            *data = text->data();
            *size = static_cast<UINT>(text->size());
            owned.push_back(std::move(text));

            return S_OK;
        }

        return E_FAIL;
    }

    HRESULT __stdcall Close(LPCVOID data) override
    {
        for (auto it = owned.begin(); it != owned.end(); ++it)
            if ((*it)->data() == data)
            {
                owned.erase(it);
                break;
            }

        return S_OK;
    }

private:
    std::vector<fs::path> directories;
    std::vector<std::unique_ptr<std::string>> owned;
};

std::string readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read the shader " + path.string());

    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}

Microsoft::WRL::ComPtr<ID3DBlob> compileShaderFile(const Shader& shader)
{
    const fs::path source = fs::path(PHANTOM_FX_SOURCE_DIR) / "src" / shader.source;
    const std::string text = readFile(source);

    CheckoutIncludes includes{{source.parent_path(), fs::path(PHANTOM_FX_SOURCE_DIR) / "src"}};
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    const auto name = source.filename().string();
    const auto result = D3DCompile(text.data(), text.size(), name.c_str(), nullptr, &includes, "main", shader.profile,
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result))
        throw std::runtime_error(std::string("Shader ") + shader.source + " did not compile: "
            + (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                      : hex(static_cast<std::uint32_t>(result))));

    return code;
}
}
