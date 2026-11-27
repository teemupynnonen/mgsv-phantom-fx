#include "game_light_volumes.hpp"

#include "d3d11_helpers.hpp"
#include "game_shaders.hpp"
#include "log.hpp"
#include "phantom_shaders/light_volume_jittered.hpp"
#include "shader_bytecode.hpp"

#include <wrl/client.h>
#include <algorithm>
#include <mutex>
#include <optional>
#include <string_view>

namespace phantom_fx::game_light_volumes
{
namespace
{
inline constexpr Shader JitteredVolume{g_light_volume_jittered, sizeof(g_light_volume_jittered),
    "passes/shaders/light_volume_jittered.vs.hlsl", "vs_5_0"};

std::once_flag compiled;
std::optional<std::vector<std::uint8_t>> code; // Empty when it did not compile or Direct3D refused it

bool isLightVolume(std::string_view bytes)
{
    const auto name = sourceName(bytes, "_vs.hlsl");
    if (name.empty()) return false;

    return std::ranges::any_of(game_shaders::GameShaders,
        [name](const game_shaders::GameShader& row) { return row.pass == game_shaders::GamePass::LocalLight && name == row.name; });
}
}

const std::vector<std::uint8_t>* jittered(ID3D11Device* device, const void* original, std::size_t size) noexcept
{
    try
    {
        if (!original || !isLightVolume({static_cast<const char*>(original), size})) return nullptr;

        std::call_once(compiled, [device] {
            try
            {
                const auto bytecode = bytecodeOf(JitteredVolume);
                const auto* bytes = static_cast<const std::uint8_t*>(bytecode.bytes);
                std::vector<std::uint8_t> made(bytes, bytes + bytecode.size);

                Microsoft::WRL::ComPtr<ID3D11VertexShader> shader;
                if (!device || FAILED(device->CreateVertexShader(made.data(), made.size(), nullptr, shader.GetAddressOf())))
                {
                    log("Direct3D refused the jittered light volume shader; the lights keep the game's.");
                    return;
                }

                code = std::move(made);
                log("The game's light volumes are drawn with the upscaler's jitter.");
            }
            catch (const std::exception& error)
            {
                log(std::string("The jittered light volume shader did not compile; the lights keep the game's: ") + error.what());
            }
        });

        return code ? &*code : nullptr;
    }
    catch (...)
    {
        return nullptr;
    }
}
}
