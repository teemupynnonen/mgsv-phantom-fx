#include "hot_shader.hpp"

#include "log.hpp"

#include <d3dcompiler.h>

#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <system_error>

namespace phantom_fx
{
namespace
{
using Stamp = std::filesystem::file_time_type;
using Watched = std::vector<std::pair<std::filesystem::path, Stamp>>;

Stamp stampOf(const std::filesystem::path& file)
{
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(file, error);

    return error ? Stamp::min() : stamp;
}

// Opens `#include`s from disk, next to the shader, and remembers each file it opened.
class DiskIncludes final : public ID3DInclude
{
public:
    explicit DiskIncludes(std::filesystem::path directory) : directory(std::move(directory)) {}

    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* size) override
    {
        for (const auto& base : {directory, lastDirectory})
        {
            auto path = (base / name).lexically_normal();
            std::ifstream in(path, std::ios::binary);
            if (!in) continue;

            auto* text = new std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            opened.push_back(path);
            lastDirectory = path.parent_path();
            *data = text->data();
            *size = static_cast<UINT>(text->size());
            owned.push_back(text);

            return S_OK;
        }

        return E_FAIL;
    }

    HRESULT __stdcall Close(LPCVOID data) override
    {
        for (auto it = owned.begin(); it != owned.end(); ++it)
            if ((*it)->data() == data)
            {
                delete *it;
                owned.erase(it);
                break;
            }

        return S_OK;
    }

    std::vector<std::filesystem::path> opened;

private:
    std::filesystem::path directory, lastDirectory;
    std::vector<std::string*> owned;
};

struct Compiled
{
    Microsoft::WRL::ComPtr<ID3DBlob> code; // null when the compile failed
    std::vector<std::filesystem::path> includes;
};

Compiled compileFile(const std::filesystem::path& source, const std::string& profile, const HotShader::Defines& defines)
{
    std::ifstream in(source, std::ios::binary);
    if (!in)
    {
        log("Hot shader: cannot read " + source.string());
        return {};
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    std::vector<D3D_SHADER_MACRO> macros;
    for (const auto& [name, value] : defines) macros.push_back({name.c_str(), value.c_str()});
    macros.push_back({nullptr, nullptr});

    DiskIncludes includes{source.parent_path()};
    Compiled result;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const auto name = source.filename().string();
    const auto hr = D3DCompile(text.data(), text.size(), name.c_str(), macros.data(), &includes, "main", profile.c_str(),
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, result.code.GetAddressOf(), errors.GetAddressOf());
    result.includes = std::move(includes.opened);
    if (FAILED(hr))
    {
        result.code.Reset();
        log("Hot shader: " + name + " did not compile, the shader in use stays: "
            + (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : hex(static_cast<std::uint32_t>(hr))));
    }

    return result;
}

Watched watchList(const std::filesystem::path& source, const std::vector<std::filesystem::path>& includes)
{
    Watched list{{source, stampOf(source)}};
    for (const auto& file : includes) list.emplace_back(file, stampOf(file));

    return list;
}

bool changed(const Watched& list)
{
    for (const auto& [file, stamp] : list)
        if (stampOf(file) != stamp) return true;

    return false;
}
}

HotShader::HotShader(std::filesystem::path source, std::string profile, Defines defines, std::chrono::milliseconds interval)
    : source(std::move(source)), profile(std::move(profile)), defines(std::move(defines)), interval(interval),
      worker([this](std::stop_token stop) { run(stop); })
{
}

void HotShader::run(std::stop_token stop)
{
    // The first pass only learns the includes: the code in use is the build's, and is not replaced by a copy of itself.
    auto watched = watchList(source, compileFile(source, profile, defines).includes);
    started = true;
    log("Hot shader: watching " + source.string() + " and " + std::to_string(watched.size() - 1) + " include(s)");

    while (!stop.stop_requested())
    {
        {
            std::unique_lock lock{wait};
            wake.wait_for(lock, stop, interval, [] { return false; });
        }
        if (stop.stop_requested()) break;
        if (!changed(watched)) continue;

        // Files are stamped before the compile, so an edit that lands during it is seen next time. Stamps move on
        // after a failure too: the same broken file is not compiled and logged again until it changes.
        for (auto& [file, stamp] : watched) stamp = stampOf(file);
        const auto stamped = watched;
        auto compiled = compileFile(source, profile, defines);
        watched = watchList(source, compiled.includes);
        for (auto& [file, stamp] : watched)
            for (const auto& [seen, seenStamp] : stamped)
                if (seen == file) stamp = seenStamp;
        if (!compiled.code) continue;

        log("Hot shader: " + source.filename().string() + " reloaded");
        std::scoped_lock lock{mutex};
        published = std::move(compiled.code);
    }
}

Microsoft::WRL::ComPtr<ID3DBlob> HotShader::takeCode()
{
    std::scoped_lock lock{mutex};

    return std::exchange(published, nullptr);
}

Microsoft::WRL::ComPtr<ID3D11PixelShader> HotShader::takePixelShader(ID3D11Device* device)
{
    const auto code = takeCode();
    if (!code) return nullptr;

    try
    {
        Microsoft::WRL::ComPtr<ID3D11PixelShader> shader;
        if (FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader.GetAddressOf()))) throw std::runtime_error("CreatePixelShader failed");

        return shader;
    }
    catch (const std::exception& error)
    {
        log(std::string("Hot shader: the device refused the new pixel shader: ") + error.what());
        return nullptr;
    }
}

Microsoft::WRL::ComPtr<ID3D11ComputeShader> HotShader::takeComputeShader(ID3D11Device* device)
{
    const auto code = takeCode();
    if (!code) return nullptr;

    try
    {
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
        if (FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader.GetAddressOf()))) throw std::runtime_error("CreateComputeShader failed");

        return shader;
    }
    catch (const std::exception& error)
    {
        log(std::string("Hot shader: the device refused the new compute shader: ") + error.what());
        return nullptr;
    }
}
}
