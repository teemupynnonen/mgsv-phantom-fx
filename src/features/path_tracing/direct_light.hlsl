// Path-traced lamps (docs/DEVELOPMENT.md, "Path tracing"): with DLSS Ray Reconstruction denoising, every
// render pixel's light from the frame's point and spot lights, the flashlights among them, in place of the
// game's light shaders. Each light's light as those shaders work it out (its falloff, a spot's cone and
// mask, the game's volume for it, its diffuse and specular scales, the foliage's light through its leaves,
// its specular table). A pixel picks lights in proportion to their light there without shadows (weighted
// reservoir sampling over all of them, white noise) and traces a shadow ray to a point on each picked light:
// an unbiased estimate of all their shadowed light, which Ray Reconstruction denoises. The light goes out
// times the exposure, for the game's light targets.
#include "gi_shadow.hlsli"

Texture2D<float4> g_Guide_NormalRoughness : register(t18); // The world-space normal, the roughness in w (ray_tracing::ReconstructionGuides)
Texture2D<float4> g_LampMasks : register(t20);             // The masks: GI_LAMP_MASK_TILES tiles across
Texture2D<float4> g_Guide_Material : register(t21);        // rgb: the specular colour; a: the translucency
Texture2D<float4> g_Guide_Lobe : register(t22);            // x: the light through the leaves' floor, the translucency times the material's; y: F0; z: the anisotropy
Texture2D<float4> g_SpecularTable : register(t25);         // The game's g_lut2, its slices one below the other (ReconstructionGuides)
RWTexture2D<float4> g_LampDiffuse : register(u95);         // The lights' diffuse light, times the exposure
RWTexture2D<float4> g_LampSpecular : register(u96);        // Their specular light

// Lights a pixel picks, each with a shadow ray.
static const uint PickedLights = 2;
// g_lut2 has 16 rows of roughness in each of its 16 slices of anisotropy.
static const float TableRows = 16.0f, TableSlices = 16.0f;

// A masked spot's mask at `world`, as its shader projects and samples it (wrapped, bilinear); 1 for the others.
float3 MaskAt(GILocalLight light, float3 world)
{
    uint tile = light.maskTile.x;
    if (tile == 0u) return 1.0f;

    float4 position = float4(world, 1.0f);
    float3 projected = float3(dot(light.mask[0], position), dot(light.mask[1], position), dot(light.mask[2], position));
    if (projected.z <= 0.0f) return 0.0f;

    float2 uv = projected.xy / projected.z * 0.5f + 0.5f;
    uv = frac(float2(uv.x, 1.0f - uv.y));
    // Half a texel in from the tile's edges, so the bilinear taps stay in the tile.
    float inset = 0.5f / GI_LAMP_MASK_SIZE;
    uv = clamp(uv, inset, 1.0f - inset);
    return g_LampMasks.SampleLevel(g_LinearSampler, float2((float(tile - 1u) + uv.x) / GI_LAMP_MASK_TILES, uv.y), 0.0f).rgb;
}

// The surface a pixel's lights light.
struct LitSurface
{
    float3 world, normal, toEye, specularColor;
    float roughness, F0, anisotropy, translucency, translucencyFloor;
};

// g_lut2 at `uvw`, as a linear, clamped sampler reads the game's volume.
float2 SpecularTableAt(float3 uvw)
{
    float slice = clamp(uvw.z * TableSlices - 0.5f, 0.0f, TableSlices - 1.0f);
    float first = floor(slice);
    float row = clamp(uvw.y * TableRows, 0.5f, TableRows - 0.5f);
    float height = TableRows * TableSlices;
    float2 low = g_SpecularTable.SampleLevel(g_LinearSampler, float2(uvw.x, (first * TableRows + row) / height), 0.0f).xy;
    float2 high = g_SpecularTable.SampleLevel(g_LinearSampler, float2(uvw.x, (min(first + 1.0f, TableSlices - 1.0f) * TableRows + row) / height), 0.0f).xy;
    return lerp(low, high, slice - first);
}

// A light's specular light as the game's light shaders work it out (Lighting_Spot_NA's disassembly): its table of
// the halfway vector's angle, the roughness and the anisotropy, towards the light as a disc of its `size`.
float3 GameSpecular(float3 reaching, LitSurface surface, float3 toLight, float lightDistance, float size)
{
    float3 reflected = reflect(-surface.toEye, surface.normal);
    float3 towardsReflection = reflected - toLight;
    float rate = min(saturate(size / lightDistance * 0.9f) / max(length(towardsReflection), 1e-6f), 1.0f);
    float3 halfway = normalize(surface.toEye + normalize(toLight + towardsReflection * rate));
    float LH = saturate(dot(halfway, toLight));
    float fresnel = surface.F0 + (1.0f - surface.F0) * pow(1.0f - LH, 5.0f);

    float3 uvw = float3(saturate(dot(halfway, surface.normal)), surface.roughness, surface.anisotropy);
    float2 value = SpecularTableAt(uvw * uvw * float3(0.984375f, 0.9375f, 0.9375f) + float3(0.0078125f, 0.03125f, 0.03125f));
    value *= value;
    return reaching * surface.specularColor * (saturate(dot(toLight, surface.normal) * 4.0f) * value.x / max(value.y, 1e-6f) * fresnel);
}

// A light's diffuse and specular light at the surface without shadows; false where it gives none.
bool LampLightAt(GILocalLight light, LitSurface surface, out float3 diffuse, out float3 specular, out float3 toLight)
{
    float lightDistance;
    float3 reaching = LocalLightReaching(light, surface.world, toLight, lightDistance) * MaskAt(light, surface.world);
    diffuse = 0.0f;
    specular = 0.0f;
    if (all(reaching <= 0.0f)) return false;

    // The game's light through leaves (LN_trans): lit from behind as much as the translucency lets it.
    float LN = dot(surface.normal, toLight);
    float wrap = saturate(max(surface.translucencyFloor, surface.translucency * (1.0f - LN * LN) + LN));
    diffuse = reaching * (wrap * light.scales.y);
    specular = GameSpecular(reaching, surface, toLight, max(lightDistance, light.falloff.z), light.scales.z) * light.scales.x;
    return any(diffuse > 0.0f) || any(specular > 0.0f);
}

[numthreads(8, 8, 1)]
void TraceLampLight(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_Frame.dimensions.xy)) return;

    float depth = g_Depth.Load(int3(did, 0));
    uint count = g_Frame.localLightCount.x;
    if (depth <= 0.0f || count == 0u)
    {
        g_LampDiffuse[did] = 0.0f;
        g_LampSpecular[did] = 0.0f;
        return;
    }

    float4 normalRoughness = g_Guide_NormalRoughness.Load(int3(did, 0));
    float4 material = g_Guide_Material.Load(int3(did, 0));
    float4 lobe = g_Guide_Lobe.Load(int3(did, 0));
    LitSurface surface;
    surface.world = DepthPosition(did, depth);
    surface.normal = normalize(normalRoughness.xyz);
    surface.toEye = normalize(g_Frame.eye.xyz - surface.world);
    surface.roughness = normalRoughness.w;
    surface.specularColor = material.rgb;
    surface.F0 = lobe.y;
    surface.anisotropy = lobe.z;
    surface.translucency = material.a;
    surface.translucencyFloor = lobe.x;

    // Apart from the global illumination's and the reflections' rays from the same pixel.
    uint pixels = g_Frame.dimensions.x * g_Frame.dimensions.y;
    Random random = MakeRandom(did.x + did.y * g_Frame.dimensions.x + 2u * pixels, g_Frame.dimensions.z);

    // PickedLights reservoirs at once, each light's weight its light's luminance.
    float total = 0.0f;
    uint picked[PickedLights];
    float pickedWeight[PickedLights];
    [unroll] for (uint k = 0; k < PickedLights; ++k)
    {
        picked[k] = 0u;
        pickedWeight[k] = 0.0f;
    }

    for (uint i = 0; i < count; ++i)
    {
        float3 diffuse, specular, toLight;
        if (!LampLightAt(g_Frame.localLights[i], surface, diffuse, specular, toLight)) continue;

        float weight = luminance(diffuse + specular);
        total += weight;
        [unroll] for (uint k = 0; k < PickedLights; ++k)
            if (random.rand() * total < weight)
            {
                picked[k] = i;
                pickedWeight[k] = weight;
            }
    }

    float3 diffuseLight = 0.0f, specularLight = 0.0f;
    if (total > 0.0f)
    {
        ShadowSurface shadowSurface = ShadowSurfaceAt(surface.world, DecodeNormal(g_Normal.Load(int3(did, 0))));
        float2 cone = PixelCone(surface.world, 0.0f);
        [unroll] for (uint k = 0; k < PickedLights; ++k)
        {
            if (pickedWeight[k] <= 0.0f) continue;

            GILocalLight light = g_Frame.localLights[picked[k]];
            float3 diffuse, specular, toLight;
            LampLightAt(light, surface, diffuse, specular, toLight);

            // A point on the light's disc facing the pixel, its radius in falloff.w.
            float3 target = light.position.xyz + DiscOffset(toLight, light.falloff.w, random.rand2());
            float3 offset = target - surface.world;
            float targetDistance = length(offset);
            float3 direction = offset / targetDistance;
            float3 origin, facing;
            float tolerance;
            ShadowRayStart(shadowSurface, direction, origin, facing, tolerance);
            float rayLength = targetDistance - ClearanceTowards(light, direction);
            float blocker = FLT_MAX, transmittance = 1.0f;
            bool isBlocked = false;
            if (rayLength > 0.0f)
                isBlocked = SurfaceRayBlocked(origin, facing, tolerance, direction, rayLength, cone, true, 0.0f, ShadowRayTowards(RayTowardsLamp), random,
                    blocker, transmittance);

            float scale = (isBlocked ? 0.0f : transmittance) * total / (pickedWeight[k] * PickedLights);
            diffuseLight += diffuse * scale;
            specularLight += specular * scale;
        }
    }

    g_LampDiffuse[did] = float4(RemoveNaNs(diffuseLight) * g_Frame.nearFar.w, 0.0f);
    g_LampSpecular[did] = float4(RemoveNaNs(specularLight) * g_Frame.nearFar.w, 0.0f);
}
