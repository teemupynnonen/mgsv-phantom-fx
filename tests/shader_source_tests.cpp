// A development build compiles the shaders from the checkout; the release build ships only fxc's bytecode.
#include "d3d11_helpers.hpp"
#include "features/path_tracing/surface_shaders.hpp"
#include "features/volumetric_fog/atmosphere_shaders.hpp"
#include "passes/sun_light_pass_shaders.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
void expect(bool ok, const char* what)
{
    if (ok) return;
    std::fprintf(stderr, "FAILED: %s\n", what);
    std::exit(1);
}

const phantom_fx::Shader* Shaders[]{
    &phantom_fx::surface_shaders::Glass,
    &phantom_fx::surface_shaders::Water[1][1][1],
    &phantom_fx::atmosphere::shaders::SkyDraw(true, true, true),
    &phantom_fx::sun_light_pass::shaders::SunLight[1][1],
    &phantom_fx::sun_light_pass::shaders::PathTraced[0],
};

bool isDxbc(const Microsoft::WRL::ComPtr<ID3DBlob>& code)
{
    return code && code->GetBufferSize() > 32 && std::memcmp(code->GetBufferPointer(), "DXBC", 4) == 0;
}
}

int main()
{
    for (const auto* shader : Shaders)
    {
        const auto code = phantom_fx::compileShaderFile(*shader);
        expect(isDxbc(code), shader->source);
    }

    std::puts("shader_source: ok");
    return 0;
}
