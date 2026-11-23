// The clouds' pixel shader (docs/DEVELOPMENT.md, "Clouds"): drawn in place of tpp3dfw_generativeClouds with the game's vertex shader and bindings.
// Precompiled by fxc at build time; a dev build recompiles this file when it is saved (src/hot_shader.hpp).
#include "cloud_density.hlsli"

SamplerState LinearClamp : register(s11);
SamplerState PointWrap : register(s8);
Texture2D<float4> GameFog : register(t12);
Texture2D<float4> Dither : register(t15);
// The scene's depth (reversed: 0 is the far plane), bound by the game at its cloud draw.
Texture2D<float> SceneDepth : register(t13);
// The ground's cloud shadow (cloud_shadow.hlsl), drawn by clouds.cpp just before this draw.
Texture2D<float> CloudShadow : register(t16);

static const float Pi = 3.14159265;
static const int Steps = 128;

float HG(float mu, float g)
{
    float g2 = g * g;
    return (1 - g2) / (4 * Pi * pow(max(1 + g2 - 2 * g * mu, 1e-4), 1.5));
}

float3 Encode(float3 linearColor)
{
    float3 high = 1.055 * pow(max(linearColor, 0.00001), 1 / 2.4) - 0.055;
    return linearColor <= 0.0031308 ? linearColor * 12.92 : high;
}

// The game's sun is ~68 after exposure; these bring a lit top near 0.9 without clipping the shadows away.
static const float SunScale = 0.1, AmbientScale = 1.4;
// 0 the cloud, 1 the sun's transmittance only, 2 the alpha only, 3 the peak density, 4 the ground's cloud shadow.
static const int DebugView = 0;
float4 main(float4 color : COLOR0, float4 uv : TEXCOORD0, float4 world : TEXCOORD1, float4 position : SV_Position) : SV_Target
{
    float exposure = m_scene[23].z;
    float3 eye = m_scene[20].xyz;

    float3 ray;
    float domeAlpha = 1;
    float sceneDistance = 1e9;
    if (world.w < -0.5)
    {
        // The add-on's full-screen draw (clouds.cpp): the ray from the camera's own matrices, and how far the
        // nearest surface is. m_scene[4..7] is the projection, [8..10] the view's axes in the world.
        float2 ndc = float2(position.x / m_renderInfo.x * 2 - 1, 1 - position.y / m_renderInfo.y * 2);
        float3 viewRay = m_scene[8].xyz * (ndc.x / m_scene[4].x) + m_scene[9].xyz * (ndc.y / m_scene[5].y) + m_scene[10].xyz;
        ray = normalize(viewRay);
        float depth = SceneDepth.Load(int3(position.xy, 0)).x;
        // Reversed depth: sky is exactly 0, every surface a small positive value.
        sceneDistance = depth > 0 ? m_scene[6].w / (depth - m_scene[6].z) * length(viewRay) : 1e9;
    }
    else
    {
        // The game's dome: the sky the rays see (sky_capture.cpp), with the dome's own fade.
        ray = normalize(world.xyz - eye);
        domeAlpha = color.x;
    }

    if (DebugView == 5) return float4(CloudShadow.SampleLevel(LinearClamp, position.xy / m_renderInfo.xy, 0).xxx, 1);
    if (DebugView == 4 && sceneDistance < 1e8)
    {
        float2 shadowUV = CloudShadowUV(eye + ray * sceneDistance, normalize(m_materials[0].xyz), CloudShadowCentre(eye));
        return float4(CloudShadow.SampleLevel(LinearClamp, shadowUV, 0).xxx, 1);
    }
    float baseCover = CloudBaseCover(domeAlpha);

    float tEnter, tExit;
    if (abs(ray.y) < 1e-4)
    {
        tEnter = 0;
        tExit = (eye.y > LayerBase && eye.y < LayerTop) ? 12000 : -1;
    }
    else
    {
        float t0 = (LayerBase - eye.y) / ray.y, t1 = (LayerTop - eye.y) / ray.y;
        tEnter = max(min(t0, t1), 0);
        tExit = max(t0, t1);
    }
    tExit = min(min(tExit, tEnter + 12000), sceneDistance);
    float span = tExit - tEnter;
    float farFade = 1 - smoothstep(15000, 40000, tEnter);

    float2 windXZ = CloudWindXZ();
    float3 wind = CloudWind();

    float3 sunLight = m_object[10].xyz * m_materials[2].w * exposure;
    float3 ambient = m_materials[2].xyz * exposure + 0.00025 * sunLight;
    // The sky's blue, mostly greyed: a cloud's shaded side is a neutral grey, not lavender.
    ambient = lerp(ambient, dot(ambient, float3(0.2126, 0.7152, 0.0722)).xxx, 0.65);
    float3 towardsSun = normalize(m_materials[0].xyz);
    float mu = dot(ray, towardsSun);
    float phase = 0.7 * HG(mu, 0.6) + 0.2 * HG(mu, -0.2) + 0.1;

    float transmittance = 1;
    float3 light = 0;
    // How far the light actually scattered, weighted like it: a horizon cloud is kilometres away even though the ray met
    // the layer a few hundred metres out.
    float farWeight = 0;
    float sunSeen = 0, peak = 0;
    if (span > 1 && tEnter < 40000)
    {
        const int steps = max(16, (int)g_cloudSteps);
        float stepLength = span / steps;
        float jitter = Dither.SampleLevel(PointWrap, position.xy / 8, 0).x;
        const float lightStep = 350;

        // Adaptive: coarse steps through clear air, quarter steps inside a cloud, so a dense core is sampled finely
        // enough not to turn into a halftone.
        float t = tEnter + stepLength * jitter;
        int fineLeft = 0; // fine steps left before going back to coarse ones
        for (int i = 0; i < steps * 2 && t < tExit; ++i)
        {
            float3 where = eye + ray * t;
            float density = saturate(CloudDensity(where, windXZ, wind, baseCover) * 4) * farFade * smoothstep(30, 250, t);
            peak = max(peak, density);
            if (fineLeft == 0)
            {
                if (density <= 0.004) { t += stepLength; continue; }
                // A coarse step landed inside: go back and come in finely, so the edge is found at the same place
                // by every pixel and not wherever the dither put its coarse step.
                t = max(t - stepLength, tEnter);
                fineLeft = 8;
                continue;
            }
            float dt = stepLength * 0.25;
            t += dt;
            fineLeft = density > 0.004 ? 8 : fineLeft - 1;
            if (density <= 0.004) continue;
            // Towards the sun in world space, through the same density: six steps lengthening to ~6 km.
            float opticalDepth = 0, reach = 0;
            for (int k = 0; k < 6; ++k)
            {
                float len = 1 + 0.7 * k;
                float3 lightPos = where + towardsSun * lightStep * (reach + 0.5 * len);
                reach += len;
                opticalDepth += saturate(CloudDensity(lightPos, windXZ, wind, baseCover) * 4) * len * lightStep;
            }
            opticalDepth *= Extinction;
            // Multiple scattering, approximated: deep inside, light still arrives, softer and weaker.
            // Cloud is optically thick: light still reaches deep inside, scattered many times, and that is what keeps a shaded side
            // from going black. A floor too low reads as a storm cloud on a clear day.
            float sunT = max(exp(-opticalDepth), 0.7 * exp(-opticalDepth * 0.08));

            float height = saturate((where.y - LayerBase) / (LayerTop - LayerBase));
            float3 scatter = sunLight * phase * sunT * SunScale + ambient * lerp(0.75, 1.0, height) * AmbientScale;

            // Energy-conserving step: the light this slab scatters, dimmed by what is in front of it.
            float stepT = exp(-density * Extinction * dt);
            float taken = transmittance * (1 - stepT);
            light += taken * scatter;
            sunSeen += taken * sunT;
            farWeight += taken * t;
            transmittance *= stepT;
            if (transmittance < 0.01) break;
        }
    }

    // `light` is the radiance scattered along the ray, integrated through the march: the sun's part where it reaches, the sky's
    // where it does not. Multiplying it by the sun again, as this did, clipped every cloud to white or left only the ambient.
    float3 outColor = light / max(1 - transmittance, 1e-4);

    float alpha = saturate((1 - transmittance) * domeAlpha);
    if (DebugView == 1) outColor = (sunSeen / max(1 - transmittance, 1e-4)).xxx;
    if (DebugView == 2) return float4(alpha.xxx, 1);
    if (DebugView == 3) return float4(peak, saturate(peak * 10), 0, 1);

    // The far air's fog, a share of it that grows with distance: a distant cloud sinks into the horizon instead of
    // staying bright white. Only a share: the game's far fog is the air at the far plane, and taken whole it replaced
    // the cloud with flat grey.
    float2 pixel = position.xy - 0.5;
    float2 clip = pixel / m_renderInfo.xy * float2(2, -2) + float2(-1, 1);
    float4 fog = GameFog.SampleLevel(LinearClamp, clip * float2(0.014648, 0.123047) + float2(0.015625, 0.125) + float2(0.96875, 0.75), 0);
    // Aerial perspective, both ways: the colour goes towards the air and the cloud thins out, so a distant cloud melts into
    // the horizon instead of sitting on top of it as a cut-out.
    float cloudDistance = (1 - transmittance) > 1e-4 ? farWeight / (1 - transmittance) : 0;
    float distanceFade = smoothstep(9000, 32000, cloudDistance);
    outColor = lerp(outColor, fog.rgb * m_scene[25].y, distanceFade * 0.6);
    alpha *= 1 - distanceFade * 0.85;

    float3 encoded = Encode(max(outColor, 0));
    return float4(encoded * alpha, alpha);
}