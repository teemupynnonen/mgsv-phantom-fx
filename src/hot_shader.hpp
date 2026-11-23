#pragma once

#if !defined(PHANTOM_FX_DEV_TOOLS) || !PHANTOM_FX_DEV_TOOLS
#error "hot_shader.hpp is for development builds: use the dev preset."
#endif

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Shader hot reload, for development. A worker thread watches an .hlsl file and the files it includes; when one
// changes it compiles the source and publishes the bytecode. The thread that draws collects it (takePixelShader,
// takeComputeShader) once per frame and swaps its pointer: no compile runs on a draw thread, and the device is
// only touched from the caller's.
//
//     phantom_fx::HotShader hot{"D:/src/shaders/pool_ps.hlsl", "ps_5_0"};
//     ...
//     // each frame, on the render thread:
//     if (auto fresh = hot.takePixelShader(device)) pool = std::move(fresh);
//
// A failed compile is logged and the shader in use stays. Only edits to the shader's body reload: a changed
// constant-buffer layout or binding still needs a rebuild of the code that fills it.
namespace phantom_fx
{
class HotShader
{
public:
    using Defines = std::vector<std::pair<std::string, std::string>>;

    // Watches `source`, compiled for `profile` (such as "ps_5_0") with entry point `main`.
    HotShader(std::filesystem::path source, std::string profile, Defines defines = {},
        std::chrono::milliseconds interval = std::chrono::milliseconds{250});

    HotShader(const HotShader&) = delete;
    HotShader& operator=(const HotShader&) = delete;

    // True once the first pass has found which files the shader includes. Edits before then are not seen.
    [[nodiscard]] bool watching() const noexcept { return started.load(); }

    // The bytecode compiled since the last call; null when there is none. Newer results replace older ones.
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> takeCode();
    // The same, made into a shader on the calling thread; null when there is none or the device refused it.
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D11PixelShader> takePixelShader(ID3D11Device* device);
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D11ComputeShader> takeComputeShader(ID3D11Device* device);

private:
    void run(std::stop_token stop);

    std::filesystem::path source;
    std::string profile;
    Defines defines;
    std::chrono::milliseconds interval;

    std::atomic<bool> started{false};
    std::mutex mutex;
    Microsoft::WRL::ComPtr<ID3DBlob> published;

    std::mutex wait;
    std::condition_variable_any wake;
    // Last, so it is joined before the members it uses go.
    std::jthread worker;
};
}
