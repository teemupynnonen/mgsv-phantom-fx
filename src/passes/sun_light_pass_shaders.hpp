#pragma once

// HLSL for the sun light pass (sun_light_pass.hpp), compiled at runtime with d3dcompiler_47.
namespace phantom_fx::sun_light_pass::shaders
{
// Fox Engine's sun light pass, as the game's SunLight_DL_CS_ES (SHADOW_MAP 1, inside the shadow
// range) and SunLight_DL_ES (SHADOW_MAP 0, beyond it) compute it, rebuilt from their disassembly
// with the same bindings; in rain the game draws SunLight_DL_CS and SunLight_DL, the same without
// the sky reflection (SKY_REFLECTION 0). Plus the add-on's options in b6: soft shadows filter the
// shadow map, the screen-space shadow in t7 multiplies the sun's shadow, and the ray-traced shadow in
// t6 takes the place of the game's. With all off they give the game's output bit for bit (checked on
// WARP with random inputs). For path tracing (PATH_TRACED 1, path_tracing.hpp), the rays' shadow alone,
// and the clouds' inside the shadow range (CLOUD_SHADOW 1). Split into literals, as MSVC limits each to 16 KB.
inline constexpr char SunLight[] = R"(
cbuffer cPSScene : register(b2)
{
    float4x4 m_projectionView;
    float4x4 m_projection;
    float4x4 m_view;
    float4 m_shadowProjection[4];   // Rows: view position to the first cascade
    float4 m_cascades[4];           // m_shadowProjection2: columns x, y, z place cascades 1 to 3 (offset, scale in row 3)
    float4 m_eyepos;
    float4 m_projectionParam;       // 1 / P00, 1 / P11, B, A
    float4 m_viewportSize;
    float4 m_exposure;
    float4 m_fogParam[3];
    float4 m_fogColor;
    float4 m_cameraCenterOffset;
    float4 m_shadowMapResolutions;
};

cbuffer cPSLight : register(b3)
{
    float4 m_lightParams[11];
};

cbuffer cPSObject : register(b5)
{
    float4 m_objectParams[13];
};

cbuffer cSunShadows : register(b6) // The add-on's options
{
    float g_softShadows;        // 1: contact-hardening soft shadows from the shadow map
    float g_sunTangent;         // Tangent of the sun's angular radius, for the soft shadows
    float g_frame;              // Turns the soft shadows' samples: the frame modulo 64 while DLSS accumulates frames, else 0
    float g_screenSpaceShadows; // 1: the screen-space shadow in t7 multiplies the sun's shadow
    float g_rayTraced;          // 1: the ray-traced shadow in t6 is the sun's shadow (see SunShadow)
    float3 g_padding;
};

SamplerState g_samplerPoint_Wrap : register(s8);
SamplerState g_samplerPoint_Clamp : register(s9);
SamplerState g_samplerLinear_Wrap : register(s10);
SamplerState g_samplerLinear_Clamp : register(s11);
SamplerComparisonState g_samplerComparisonLess_Linear_Clamp : register(s15);

Texture2D<float4> g_NormalTexture : register(t0);
Texture2D<float4> g_SpecularTexture : register(t1);
Texture2D<float4> g_DepthTexture : register(t2);
Texture2D<float> g_ShadowTexture : register(t3);
Texture2D<float4> g_CloudTexture : register(t4);
Texture2D<float> g_RayTracedShadow : register(t6);
Texture2D<float> g_ScreenSpaceShadow : register(t7);
Texture3D<float4> g_lut2 : register(t8);
Texture2D<float4> g_lut1 : register(t9);
Texture2D<float4> g_MaterialTexture : register(t10);
Texture2D<float4> g_RandomTexture : register(t12);
#if SKY_REFLECTION
Texture2D<float4> g_SkyReflectTexture : register(t15);
#endif

#if SHADOW_MAP
static const int BlockerSamples = 16;     // Gathers of four texels each
static const int FilterSamples = 24;      // Bilinear comparisons
static const float OccluderReach = 20;    // Metres: occluders this far towards the sun widen the penumbra
static const float MaxRadiusTexels = 128; // Keeps the search and the filter cache-friendly
static const float MaxSlope = 4;          // The receiver's steepest rise towards the sun, per unit across
static const float BiasTexels = 1;        // Depth bias, in texels of height, besides the slope across a bilinear footprint
static const float BiasPerOffset = 0.1;   // Depth bias per unit of offset, for curved receivers

// Point `index` of a Vogel disc of `count` points with radius 1, turned by `turn` radians.
float2 VogelDisc(int index, int count, float turn)
{
    float s, c;
    sincos(index * 2.39996323 + turn, s, c); // The golden angle
    return sqrt((index + 0.5) / count) * float2(c, s);
}

// Jorge Jimenez's interleaved gradient noise, in [0, 1).
float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// Contact-hardening soft shadows (Randima Fernando, "Percentage-Closer Soft Shadows", 2005) in the
// cascade the game chose. A cascade's x, y and z share one scale, z towards the sun, so all of it is
// in cascade units: an atlas uv is a quarter of a unit, and depth is shadowDepth x (z + 1), larger
// towards the sun. `slope` is the receiver's rise towards the sun per unit across; `tileMin` and
// `tileMax` keep the samples in the cascade's quarter of the atlas. Returns the share of the sun
// that reaches the receiver.
float SoftShadow(float2 uv, float receiverDepth, float2 tileMin, float2 tileMax, float2 slope, float unitsPerMetre, float turn)
{
    float shadowDepth = m_cascades[0].w;
    float texel = 4 / m_shadowMapResolutions.x;
    float searchRadius = clamp(OccluderReach * unitsPerMetre * g_sunTangent, texel, MaxRadiusTexels * texel);
    // The four texels a gather or a comparison reads lie up to a texel from the plane's point.
    float bias = (BiasTexels + abs(slope.x) + abs(slope.y)) * texel;

    // The mean height above the receiver's plane of what blocks the sun around it.
    float blockerHeight = 0, blockers = 0;
    [unroll] for (int i = 0; i < BlockerSamples; ++i)
    {
        float2 offset = VogelDisc(i, BlockerSamples, turn) * searchRadius;
        float plane = receiverDepth + shadowDepth * (dot(slope, offset) + bias + BiasPerOffset * length(offset));
        float4 depths = g_ShadowTexture.Gather(g_samplerPoint_Clamp, clamp(uv + float2(offset.x, -offset.y) * 0.25, tileMin, tileMax));
        float4 blocking = depths > plane ? 1.0 : 0.0;
        blockerHeight += dot(blocking, depths - plane);
        blockers += dot(blocking, 1);
    }

    if (blockers == 0) return 1;
    if (blockers == BlockerSamples * 4) return 0;

    // The penumbra's radius grows with the blockers' height, at the sun's angular radius.
    float radius = clamp(blockerHeight / (blockers * shadowDepth) * g_sunTangent, texel, searchRadius);
    float blocked = 0;
    [unroll] for (int j = 0; j < FilterSamples; ++j)
    {
        float2 offset = VogelDisc(j, FilterSamples, turn) * radius;
        float plane = receiverDepth + shadowDepth * (dot(slope, offset) + bias + BiasPerOffset * length(offset));
        float2 sampleUv = clamp(uv + float2(offset.x, -offset.y) * 0.25, tileMin, tileMax);
        blocked += g_ShadowTexture.SampleCmp(g_samplerComparisonLess_Linear_Clamp, sampleUv, plane);
    }

    return 1 - blocked / FilterSamples;
}
)" R"(
// The four cascades share one atlas, two by two. Returns 1 where the sun reaches, after the
// distance fade, the clouds and the height fade. `tangentX` and `tangentY` lie on the receiver, in
// view space, for the soft shadows. With ray-traced shadows, `rayTraced` is the rays' shadow, which
// leaves out the alpha-tested meshes (leaves, grass): the shadow map gives theirs, so the darker of
// the two counts, and the map fades to the rays alone towards the end of its range.
float SunShadow(float3 viewPosition, float2 pixel, float3 tangentX, float3 tangentY, float rayTraced)
{
    float4 position = float4(viewPosition, 1);
    float resolution = m_shadowMapResolutions.x;
    float2 halfPixelSize = 0.5 / resolution;
    float3 shadowPosition = float3(dot(position, m_shadowProjection[0]), dot(position, m_shadowProjection[1]),
        dot(position, m_shadowProjection[2]));

    // Cascade selection, dithered across the cascade borders when cascade blend is on.
    float texel = 1 / resolution;
    float area = 1 - texel * 8;
    float areaFirst = area * area;
    float areaOther = areaFirst;
    float randomValue = 0;
    if (m_cascades[3].w != 0)
    {
        float2 centerOffset = g_RandomTexture.Sample(g_samplerLinear_Wrap, pixel * 0.03125).xy;
        float random = max(centerOffset.y, centerOffset.x);
        randomValue = random * 0.0025;
        float shrunk = area - texel * (random * resolution / 4);
        areaOther = shrunk * shrunk * 1;
    }

    float shadowDepth = m_cascades[0].w;
    float4 level0 = float4(shadowPosition, 0);
    float4 level1 = float4(m_cascades[0].x, m_cascades[1].x, m_cascades[2].x, m_cascades[3].x);
    float4 level2 = float4(m_cascades[0].y, m_cascades[1].y, m_cascades[2].y, m_cascades[3].y);
    float4 level3 = float4(m_cascades[0].z, m_cascades[1].z, m_cascades[2].z, m_cascades[3].z);
    level1 = float4(level1.xyz + shadowPosition * level1.w, 1);
    level2 = float4(level2.xyz + shadowPosition * level2.w, 2);
    level3 = float4(level3.xyz + shadowPosition * level3.w, 3);
    level0.z = shadowDepth * (level0.z + 1);
    level1.z = shadowDepth * (level1.z + 1);
    level2.z = shadowDepth * (level2.z + 1);
    level3.z = shadowDepth * (level3.z + 1);

    float3 inside0 = float3(areaFirst >= level0.xy * level0.xy, level0.z >= randomValue);
    float3 inside1 = float3(areaOther >= level1.xy * level1.xy, level1.z >= randomValue);
    float3 inside2 = float3(areaOther >= level2.xy * level2.xy, level2.z >= randomValue);
    float mask0 = inside0.y * inside0.x * inside0.z;
    float hit = 1 - mask0;
    float mask1 = inside1.y * inside1.x * inside1.z * hit;
    hit = (1 - mask1) * hit;
    float mask2 = hit * (inside2.y * inside2.x * inside2.z);
    hit = hit * (1 - mask2);
    float4 cascade = level0 * mask0 + level1 * mask1 + level2 * mask2 + level3 * hit;

    float level = cascade.w;
    float compareDepth = max(cascade.z, 0.000001);
    float2 uv = float2(cascade.x * 0.5 + 0.5, 1 - (cascade.y * 0.5 + 0.5)) * 0.5;
    uv = min(uv, 1 - halfPixelSize * 2);
    float row = saturate(level - 1);
    uv += float2(level / 2 - row, row * 0.5);
    uv = halfPixelSize + uv;

    float shadow;
    [branch] if (g_softShadows)
    {
        float2 tileMin = float2(level / 2 - row, row * 0.5) + texel;
        float2 tileMax = tileMin + 0.5 - texel * 2;
        float unitsPerMetre = length(m_shadowProjection[0].xyz) * dot(float4(mask0, mask1, mask2, hit), float4(1, m_cascades[3].xyz));
        // The receiver's plane in cascade units, from its tangents; the cascade's scale cancels out.
        float3x3 toCascade = float3x3(m_shadowProjection[0].xyz, m_shadowProjection[1].xyz, m_shadowProjection[2].xyz);
        float3 normal = cross(mul(toCascade, tangentX), mul(toCascade, tangentY));
        float2 slope = clamp(-normal.xy * normal.z / max(normal.z * normal.z, 1e-12), -MaxSlope, MaxSlope);
        float turn = 6.2831853 * InterleavedGradientNoise(pixel + 5.588238 * g_frame);
        shadow = SoftShadow(uv, compareDepth, tileMin, tileMax, slope, unitsPerMetre, turn);
    }
    else
    {
        // Four comparisons around the position, with a 2x2 pixel dither.
        float4 dither = (frac(pixel.xyxy / 2) >= 0.3 ? 1.0 : 0.0) * 2 - 1;
        float4 offset = (dither * 0.125 + float4(0.5, 0.5, -0.5, -0.5)) * (1 / resolution);
        float sum = g_ShadowTexture.SampleCmp(g_samplerComparisonLess_Linear_Clamp, offset.xy + uv, compareDepth) + 0;
        sum = g_ShadowTexture.SampleCmp(g_samplerComparisonLess_Linear_Clamp, offset.zy + uv, compareDepth) + sum;
        sum = g_ShadowTexture.SampleCmp(g_samplerComparisonLess_Linear_Clamp, offset.xw + uv, compareDepth) + sum;
        sum = g_ShadowTexture.SampleCmp(g_samplerComparisonLess_Linear_Clamp, offset.zw + uv, compareDepth) + sum;
        shadow = 1 - sum * 0.25 * 1;
        shadow = shadow * shadow;
    }

    // Fades to lit towards the end of the shadow range.
    float3 fromMaxLevel = viewPosition - float3(0, 0, m_lightParams[8].x);
    float pastShadowStart = max(length(fromMaxLevel) - m_lightParams[8].y, 0);
    float inViewRange = saturate(pastShadowStart * m_lightParams[6].x) * (fromMaxLevel.z >= 0 ? 1.0 : 0.0);
    inViewRange *= inViewRange;
    [branch] if (g_rayTraced)
    {
        shadow = min(shadow, rayTraced);
        shadow = (rayTraced - shadow) * inViewRange + shadow;
    }
    else
        shadow = (1 - shadow) * inViewRange + shadow;
    shadow = saturate(shadow - m_lightParams[6].w);

    float2 cloudPosition = float2(dot(m_objectParams[9], position), dot(m_objectParams[10], position));
    float cloud = g_CloudTexture.Sample(g_samplerLinear_Wrap, cloudPosition).x;
    float cloudAmount = saturate((1 / (m_objectParams[12].z - 0)) * (cloud - 0));
    cloudAmount = cloudAmount * cloudAmount * (cloudAmount * -2 + 3);
    shadow = saturate(m_objectParams[12].w * cloudAmount) * shadow;

    float heightFade = 1 - saturate((viewPosition.z - m_objectParams[11].x) * m_objectParams[11].y);
    return heightFade * heightFade * shadow;
}

float3 ViewPosition(float2 screenPosition, float depth)
{
    float viewZ = m_projectionParam.z / (depth - m_projectionParam.w);
    return float3(screenPosition * m_projectionParam.xy * viewZ, viewZ);
}

// The surface's tangent along a screen axis, towards the neighbour nearer in depth, so a pixel at an
// edge takes the side it belongs to.
float3 Tangent(float3 center, float3 before, float3 after)
{
    return abs(after.z - center.z) < abs(center.z - before.z) ? after - center : center - before;
}
#endif

#if PATH_TRACED
// The rays' shadow, which path tracing leaves raw for Ray Reconstruction; inside the game's shadow range
// the clouds' shadow and the height fade as SunShadow gives them.
float PathTracedShadow(float3 viewPosition, float rayTraced)
{
    float shadow = rayTraced;
#if CLOUD_SHADOW
    float4 position = float4(viewPosition, 1);
    float2 cloudPosition = float2(dot(m_objectParams[9], position), dot(m_objectParams[10], position));
    float cloud = g_CloudTexture.Sample(g_samplerLinear_Wrap, cloudPosition).x;
    float cloudAmount = saturate((1 / (m_objectParams[12].z - 0)) * (cloud - 0));
    cloudAmount = cloudAmount * cloudAmount * (cloudAmount * -2 + 3);
    shadow = saturate(m_objectParams[12].w * cloudAmount) * shadow;

    float heightFade = 1 - saturate((viewPosition.z - m_objectParams[11].x) * m_objectParams[11].y);
    shadow *= heightFade * heightFade;
#endif
    return shadow;
}
#endif
)" R"(

void main(float4 position : SV_Position, float4 clipPosition : TEXCOORD0, float4 samplingPosition : TEXCOORD1,
    out float4 outDiffuse : SV_Target0, out float4 outSpecular : SV_Target1)
{
    // G-buffer
    float2 texcoord = samplingPosition.xy / samplingPosition.w;
    float3 encodedNormal = g_NormalTexture.Sample(g_samplerPoint_Wrap, texcoord).xyz;
    float4 specularMap = g_SpecularTexture.Sample(g_samplerPoint_Wrap, texcoord);
    float depth = g_DepthTexture.Sample(g_samplerPoint_Wrap, texcoord).x;
    float2 normalXY = encodedNormal.xy * 2 - 1;
    float normalZ = encodedNormal.z * encodedNormal.z * 2 - 1;
    float oneMinusZz = 1 - normalZ * normalZ;
    float3 normal = float3(rsqrt(dot(normalXY, normalXY) * oneMinusZz + 1e-7) * (oneMinusZz * normalXY), normalZ);
    float roughness = specularMap.x;
    float specularIntensity = specularMap.y;
    float materialIndex = specularMap.z;
    float translucent = specularMap.w;

    float2 screenPosition = clipPosition.xy / clipPosition.w - m_cameraCenterOffset.xy;
    float viewZ = m_projectionParam.z / (depth - m_projectionParam.w);
    float3 viewPosition = float3(screenPosition * m_projectionParam.xy * viewZ, viewZ);
    float3 eyeDirection = normalize(viewPosition);

    float3 lightDirection = m_lightParams[0].xyz;
    float4 lightColor = float4(m_exposure.z * m_lightParams[1].xyz, m_lightParams[1].w);

    float shadow = 1;
    [branch] if (g_screenSpaceShadows) shadow = g_ScreenSpaceShadow.Load(int3(position.xy, 0));
    float rayTraced = 1;
    [branch] if (g_rayTraced) rayTraced = g_RayTracedShadow.Load(int3(position.xy, 0));
#if SHADOW_MAP
    // The receiver's tangents for the soft shadows, from its neighbours' depth.
    float2 pixelStep = float2(ddx(screenPosition.x), ddy(screenPosition.y));
    float3 tangentX = 0, tangentY = 0;
    [branch] if (g_softShadows)
    {
        int2 pixel = int2(position.xy);
        float3 left = ViewPosition(screenPosition - float2(pixelStep.x, 0), g_DepthTexture.Load(int3(pixel - int2(1, 0), 0)).x);
        float3 right = ViewPosition(screenPosition + float2(pixelStep.x, 0), g_DepthTexture.Load(int3(pixel + int2(1, 0), 0)).x);
        float3 above = ViewPosition(screenPosition - float2(0, pixelStep.y), g_DepthTexture.Load(int3(pixel - int2(0, 1), 0)).x);
        float3 below = ViewPosition(screenPosition + float2(0, pixelStep.y), g_DepthTexture.Load(int3(pixel + int2(0, 1), 0)).x);
        tangentX = Tangent(viewPosition, left, right);
        tangentY = Tangent(viewPosition, above, below);
    }
    shadow *= SunShadow(viewPosition, position.xy - 0.5, tangentX, tangentY, rayTraced);
    float specularShadow = max(shadow, m_lightParams[7].x);
#else
    // The game's pass beyond the shadow range has 1 here; the rays give the shadow it lacks.
    shadow *= rayTraced;
    float specularShadow = shadow < 1 ? max(shadow, m_lightParams[7].x) : 1;
#endif
#if PATH_TRACED
    // The rays' shadow on the specular too: no floor.
    shadow = PathTracedShadow(viewPosition, rayTraced);
    specularShadow = shadow;
#endif

    // The sun as a disc: the light direction moves towards the reflection by its angular size.
    float LN = dot(lightDirection, normal);
    float3 toEye = -eyeDirection;
    float toEyeN = dot(toEye, normal);
    float3 reflection = -(-(toEyeN + toEyeN) * normal + toEye);
    float rate = min(max(lightColor.w / length(reflection - lightDirection), 0), 1);
    float3 discDirection = normalize((reflection - lightDirection) * rate + lightDirection);
    float3 halfway = normalize(toEye + discDirection);
    float HN = saturate(dot(halfway, normal));
    float LH = dot(halfway, lightDirection);

    float4 material1 = g_MaterialTexture.Sample(g_samplerPoint_Clamp, float2(materialIndex, 0.25));
    float4 material0 = g_MaterialTexture.Sample(g_samplerPoint_Clamp, float2(materialIndex, 0.75));
    float F0 = material0.x;
    float anisoRoughness = material0.w;
    float lookupFresnel = g_lut1.Sample(g_samplerLinear_Clamp, float2(LH, F0)).x;
    float LNTranslucent = saturate(max(translucent * material1.w, translucent * (1 - LN * LN) + LN));
    float3 specularColor = specularIntensity * material1.xyz;

    float3 lutCoordinate = float3(saturate(HN), roughness, anisoRoughness);
    lutCoordinate = lutCoordinate * lutCoordinate * float3(0.984375, 0.9375, 0.9375) + float3(0.0078125, 0.03125, 0.03125);
    float2 lutValue = g_lut2.Sample(g_samplerLinear_Clamp, lutCoordinate).xy;
    lutValue *= lutValue;
    float specularPower = saturate(LN * 4) * (lutValue.x / lutValue.y * lookupFresnel);
    float3 specular = specularPower * (lightColor.xyz * specularColor);
    float3 diffuse = lightColor.xyz * (LNTranslucent * 1 * 1);

#if SKY_REFLECTION
    float eyeN = dot(eyeDirection, normal);
    float3 reflected = -(eyeN + eyeN) * normal + eyeDirection;
    float EH = saturate(dot(toEye, normal));
    float fresnel = g_lut1.Sample(g_samplerLinear_Clamp, float2(EH, max(F0, m_lightParams[2].z))).x;
    float2 skyUv = (-reflected.xy * 0.96875 * 0.5 + 0.5) * float2(0.25, 0.125);
    skyUv += float2(reflected.z > 0 ? 0.25 : 0, 0);
    float2 skyRate = g_SkyReflectTexture.Sample(g_samplerLinear_Clamp, skyUv).xy * 4;
    float skyRoughness = saturate(roughness * 1.428 - 0.3);
    float3 skyColor = (skyRoughness * (skyRate.y - skyRate.x) + skyRate.x) * m_lightParams[3].xyz;
    float specularRate = specularIntensity * (fresnel * ((1 - roughness) * 0.5));
    float3 environmentSpecular = material1.xyz * (specularRate * skyColor);

    float environmentRate = max(shadow, m_lightParams[2].x) * m_lightParams[2].y;
    outDiffuse = float4(shadow * diffuse * 1 + -0.0, 1);
    outSpecular = float4(specularShadow * specular * 1 + environmentRate * environmentSpecular, 1);
#else
    outDiffuse = float4(shadow * diffuse * 1 + -0.0, 1);
    outSpecular = float4(specularShadow * specular * 1, 1);
#endif
}
)";
}
