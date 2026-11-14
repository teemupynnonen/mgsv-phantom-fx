#include "game_light_volumes.hpp"

#include "d3d11_helpers.hpp"
#include "game_shaders.hpp"
#include "log.hpp"
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
// The game's Lighting_*_vs and SSLighting2_*_vs, with the volume moved by the jitter first: the clip-space
// position twice, then where it samples the light targets.
constexpr char Source[] = R"(
cbuffer cVSSystem : register(b0)
{
    float4 m_param;
    float4 m_renderInfo;
    float4 m_renderBuffer;
    float4 m_dominantLightDir;
};

cbuffer cVSScene : register(b2)
{
    float4 m_registers[28];
    float4 m_cameraCenterOffset; // xy: the jitter in NDC (camera::applyJitter)
};

void main(float4 inPosition : POSITION, out float4 position : SV_Position, out float4 clip : TEXCOORD0, out float4 sampling : TEXCOORD1)
{
    position = inPosition;
    position.xy += m_cameraCenterOffset.xy * position.w;
    clip = position;
    float4 viewToUv = m_renderInfo * m_renderBuffer.zwzw;
    float2 screen = position.xy * float2(0.5, -0.5) + 0.5 * position.w;
    sampling = float4(screen * viewToUv.xy + viewToUv.zw * position.w, position.zw);
}
)";

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
                const auto blob = compile(Source, "light-volume-jittered", "vs_5_0");
                const auto* bytes = static_cast<const std::uint8_t*>(blob->GetBufferPointer());
                std::vector<std::uint8_t> made(bytes, bytes + blob->GetBufferSize());

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
