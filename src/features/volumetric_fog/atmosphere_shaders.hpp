#pragma once

#include "d3d11_helpers.hpp"
#include "phantom_shaders/fog_integrate.hpp"
#include "phantom_shaders/fog_media.hpp"
#include "phantom_shaders/fog_multiple_scattering.hpp"
#include "phantom_shaders/fog_sky_ambient.hpp"
#include "phantom_shaders/fog_sky_parameters.hpp"
#include "phantom_shaders/fog_sky_view.hpp"
#include "phantom_shaders/fog_transmittance.hpp"
#include "phantom_shaders/fog_visibility.hpp"
#include "phantom_shaders/sky_draw_0_0_0.hpp"
#include "phantom_shaders/sky_draw_0_0_1.hpp"
#include "phantom_shaders/sky_draw_0_1_0.hpp"
#include "phantom_shaders/sky_draw_0_1_1.hpp"
#include "phantom_shaders/sky_draw_1_0_0.hpp"
#include "phantom_shaders/sky_draw_1_0_1.hpp"
#include "phantom_shaders/sky_draw_1_1_0.hpp"
#include "phantom_shaders/sky_draw_1_1_1.hpp"

// HLSL for the volumetric fog and the physical sky (atmosphere.hpp), precompiled at build time
// (shaders/). The physical atmosphere is ported from Sébastien Hillaire's reference for "A Scalable
// and Production Ready Sky and Atmosphere Rendering Technique" (Epic Games, 2020, MIT; its license is
// in licenses/).
namespace phantom_fx::atmosphere::shaders
{
inline constexpr Shader FogIntegrate{g_fog_integrate, sizeof(g_fog_integrate), "features/volumetric_fog/shaders/fog_integrate.cs.hlsl", "cs_5_0"};
inline constexpr Shader FogVisibility{g_fog_visibility, sizeof(g_fog_visibility), "features/volumetric_fog/shaders/fog_visibility.cs.hlsl", "cs_5_0"};
inline constexpr Shader AtmosphereTransmittance{g_fog_transmittance, sizeof(g_fog_transmittance), "features/volumetric_fog/shaders/transmittance.cs.hlsl",
    "cs_5_0"};
inline constexpr Shader AtmosphereMultipleScattering{g_fog_multiple_scattering, sizeof(g_fog_multiple_scattering),
    "features/volumetric_fog/shaders/multiple_scattering.cs.hlsl", "cs_5_0"};
inline constexpr Shader SkyParameterPass{g_fog_sky_parameters, sizeof(g_fog_sky_parameters), "features/volumetric_fog/shaders/sky_parameters.cs.hlsl",
    "cs_5_0"};
inline constexpr Shader SkyView{g_fog_sky_view, sizeof(g_fog_sky_view), "features/volumetric_fog/shaders/sky_view.cs.hlsl", "cs_5_0"};
inline constexpr Shader SkyAmbient{g_fog_sky_ambient, sizeof(g_fog_sky_ambient), "features/volumetric_fog/shaders/sky_ambient.cs.hlsl", "cs_5_0"};
inline constexpr Shader FogMedia{g_fog_media, sizeof(g_fog_media), "features/volumetric_fog/shaders/fog_media.cs.hlsl", "cs_5_0"};

// MOON, FLOAT_SCENE and HDR_OUTPUT are fixed when the shader is made, as the session's HDR settings are.
inline const Shader& SkyDraw(bool moon, bool floatScene, bool hdrOutput)
{
    static constexpr Shader variants[2][2][2]{
        {{Shader{g_sky_draw_0_0_0, sizeof(g_sky_draw_0_0_0), "features/volumetric_fog/shaders/sky_draw_0_0_0.ps.hlsl", "ps_5_0"},
             Shader{g_sky_draw_0_0_1, sizeof(g_sky_draw_0_0_1), "features/volumetric_fog/shaders/sky_draw_0_0_1.ps.hlsl", "ps_5_0"}},
            {Shader{g_sky_draw_0_1_0, sizeof(g_sky_draw_0_1_0), "features/volumetric_fog/shaders/sky_draw_0_1_0.ps.hlsl", "ps_5_0"},
                Shader{g_sky_draw_0_1_1, sizeof(g_sky_draw_0_1_1), "features/volumetric_fog/shaders/sky_draw_0_1_1.ps.hlsl", "ps_5_0"}}},
        {{Shader{g_sky_draw_1_0_0, sizeof(g_sky_draw_1_0_0), "features/volumetric_fog/shaders/sky_draw_1_0_0.ps.hlsl", "ps_5_0"},
             Shader{g_sky_draw_1_0_1, sizeof(g_sky_draw_1_0_1), "features/volumetric_fog/shaders/sky_draw_1_0_1.ps.hlsl", "ps_5_0"}},
            {Shader{g_sky_draw_1_1_0, sizeof(g_sky_draw_1_1_0), "features/volumetric_fog/shaders/sky_draw_1_1_0.ps.hlsl", "ps_5_0"},
                Shader{g_sky_draw_1_1_1, sizeof(g_sky_draw_1_1_1), "features/volumetric_fog/shaders/sky_draw_1_1_1.ps.hlsl", "ps_5_0"}}}};

    return variants[moon][floatScene][hdrOutput];
}
}
