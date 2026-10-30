// Path-traced reflections (docs/DEVELOPMENT.md, "Path tracing"): with DLSS Ray Reconstruction denoising,
// a ray from every render pixel along a reflection its surface's roughness spreads (GGX's visible normals).
// On a rough surface the reflection is a faint blur: its ray's hit takes its light from the light cache (CachedLight),
// one ray a pixel. On a glossy one its hit has its material (HitMaterialOf): the sun, a light and a self-lit mesh
// afresh, then a choice by their shares between its specular lobe, whose ray goes on the same way where it is glossy
// too (a reflection in a reflection), and its diffuse one, whose ray ends the path at the sky's light or the light
// cache. A ray that gets away takes the sky as the game draws it (SkySeen). The light goes out times the surface's
// specular albedo (the Ray Reconstruction guide) and the exposure, for the resolve to add to the game's specular
// light, and the first hit's distance goes to Ray Reconstruction to move the reflection with what it reflects.
#include "gi_hit_light.hlsli"

Texture2D<float4> g_Guide_NormalRoughness : register(t18); // The world-space normal, the roughness in w (ray_tracing::ReconstructionGuides)
Texture2D<float4> g_Guide_SpecularAlbedo : register(t19);  // The share of light the surface reflects towards the eye
RWTexture2D<float4> g_Reflection : register(u93);          // The reflected light times the specular albedo and the exposure
RWTexture2D<float> g_ReflectionDistance : register(u94);   // The ray's hit distance; FP16's largest where it got away

// FP16's largest, which Ray Reconstruction takes as the sky.
static const float SkyHitDistance = 65504.0f;

// A visible normal of GGX of roughness `alpha` around +z, seen from `view` (Heitz, "Sampling the GGX
// Distribution of Visible Normals", JCGT 2018).
float3 SampleVisibleNormal(float3 view, float alpha, float2 u)
{
    float3 stretched = normalize(float3(alpha * view.xy, view.z));
    float lengthSquared = dot(stretched.xy, stretched.xy);
    float3 t1 = lengthSquared > 0.0f ? float3(-stretched.y, stretched.x, 0.0f) * rsqrt(lengthSquared) : float3(1.0f, 0.0f, 0.0f);
    float3 t2 = cross(stretched, t1);
    float r = sqrt(u.x);
    float sinPhi, cosPhi;
    sincos(TWO_PI * u.y, sinPhi, cosPhi);
    float p1 = r * cosPhi;
    float s = 0.5f * (1.0f + stretched.z);
    float p2 = (1.0f - s) * sqrt(1.0f - p1 * p1) + s * r * sinPhi;
    float3 normal = p1 * t1 + p2 * t2 + sqrt(max(0.0f, 1.0f - p1 * p1 - p2 * p2)) * stretched;
    return normalize(float3(alpha * normal.xy, max(normal.z, 0.0f)));
}

// The specular rays a path takes at most, the pixel's first among them: a reflection, in it a reflection, and in that one more.
static const uint SpecularRays = 3;
// From this roughness a surface's reflection is a blur (GGX's alpha 0.25 and up): a path leaving it needs no more than
// the cache gives, and a hit of it no specular ray of its own. Most of what the game draws is rougher than this.
static const float RoughSurface = 0.5f;

// Where a specular ray goes from a surface: GGX's visible normals of `alpha`, around `normal`, seen from `toEye`;
// `cosineToEye` is the chosen one's with the eye, for its Fresnel.
float3 SpecularDirection(float3 normal, float3 toEye, float alpha, float2 u, out float cosineToEye)
{
    // In the surface's frame, the eye above it: a normal map can tilt the normal away from the eye.
    float3 tangent, bitangent;
    GetOrthoVectors(normal, tangent, bitangent);
    float3 view = normalize(float3(dot(toEye, tangent), dot(toEye, bitangent), max(dot(toEye, normal), 1e-3f)));
    float3 halfway = SampleVisibleNormal(view, alpha, u);
    cosineToEye = saturate(dot(view, halfway));
    float3 reflected = reflect(-view, halfway);
    // Below the surface: the mirror direction in its place.
    if (reflected.z <= 0.0f) reflected = float3(-view.xy, view.z);

    return normalize(reflected.x * tangent + reflected.y * bitangent + reflected.z * normal);
}

// Smith's masking of a direction at `cosine` to the normal.
float SmithMasking(float cosine, float alpha)
{
    float a2 = alpha * alpha;
    return 2.0f * cosine / max(cosine + sqrt(a2 + (1.0f - a2) * cosine * cosine), 1e-4f);
}

// The light the cache holds for a hit, its own and what it reflects, diffuse; false where no ray has lit that cell yet.
bool CachedLight(float3 position, float3 direction, float hitDistance, out float3 light)
{
    light = 0.0f;
    uint tileIndex;
    bool isNewTile;
    uint cellIndex = HashGridCache_InsertCell(position, direction, hitDistance, tileIndex, isNewTile);
    if (cellIndex == kGI1_InvalidId) return false;

    HashGridCache_TouchCell(tileIndex, isNewTile);
    float4 direct = HashGridCache_FilteredRadiance(g_HashGridCache_ValueBuffer, cellIndex, HASH_MAX_SAMPLE_COUNT);
    if (!(direct.w > 0.0f)) return false;

    light = direct.rgb / direct.w + HashGridCache_FilteredIndirect(cellIndex);
    return true;
}

// The light along a ray from a surface: at each hit its own light (HitOwnLight), and what it reflects: its
// specular lobe's ray, or its diffuse lobe's, which ends at the sky's light or the cache as it stands. From a rough
// surface (`isRough`) the first hit's light comes from the cache where it has any. `cone` as PixelCone gives it, which
// reads the hits' textures as sharp as the ray shows them.
float3 ReflectedLight(float3 origin, float3 normal, float tolerance, float3 direction, float2 cone, bool isRough, inout Random random, out float hitDistance)
{
    hitDistance = SkyHitDistance;
    float3 light = 0.0f, throughput = 1.0f;
    float travelled = 0.0f;
    for (uint ray = 1; ray <= SpecularRays; ++ray)
    {
        SceneHit hit = TraceFromSurface(origin, normal, tolerance, direction, MAX_HIT_DISTANCE);
        if (!hit.hit) return light + throughput * SkySeen(direction);

        if (ray == 1) hitDistance = min(hit.distance, SkyHitDistance);
        travelled += hit.distance;
        uint hitFlags = g_Geometries[FirstGeometry(g_Instances[hit.instance]) + hit.geometry].flags;
        float3 cached;
        if (ray == 1 && isRough && !(hitFlags & GI_GEOMETRY_EMISSIVE) && CachedLight(origin + hit.distance * direction, direction, hit.distance, cached)) return cached;

        HashGridCache_Visibility visibility = VisibilityOf(hit, direction);
        HitMaterial material = HitMaterialOf(visibility, cone.x + travelled * cone.y, false);
        light += throughput * material.emission;
        if (material.isSelfLit) return light;

        float3 toEye = -direction;
        origin = OffsetFromTriangle(material.world, material.geometric);
        normal = material.geometric;
        tolerance = CopyTolerance(material.world);
        light += throughput * HitOwnLight(material, toEye, origin, tolerance, random);

        // Which lobe's ray goes on, by what each reflects.
        float cosine = saturate(dot(material.normal, toEye));
        float3 fresnel = material.specularColor + (1.0f - material.specularColor) * pow(1.0f - cosine, 5.0f);
        bool hasSpecular = any(material.specularColor > 0.0f) && material.roughness < RoughSurface;
        float specularShare = hasSpecular ? clamp(luminance(fresnel) / max(luminance(fresnel) + luminance(material.albedo), 1e-4f), 0.05f, 0.95f) : 0.0f;
        if (ray == SpecularRays) specularShare = 0.0f;

        if (random.rand() < specularShare)
        {
            float alpha = max(material.roughness * material.roughness, 1e-3f);
            float cosineToEye;
            direction = SpecularDirection(material.normal, toEye, alpha, random.rand2(), cosineToEye);
            float3 weight = material.specularColor + (1.0f - material.specularColor) * pow(1.0f - cosineToEye, 5.0f);
            throughput *= weight * (SmithMasking(saturate(dot(material.normal, direction)), alpha) / specularShare);
            if (dot(direction, material.geometric) <= 0.0f) return light;

            continue;
        }

        float weight;
        float3 side = BounceSide(material.geometric, material.isLeaf, random, weight);
        float3 bounceDirection = mapToCosineHemisphere(random.rand2(), side);
        float3 bounceOrigin = all(side == material.geometric) ? origin : OffsetFromTriangle(material.world, side);
        SceneHit bounce = TraceFromSurface(bounceOrigin, side, tolerance, bounceDirection, MAX_HIT_DISTANCE);
        throughput *= material.albedo * (weight / (1.0f - specularShare));
        if (!bounce.hit) return light + throughput * SkyRadiance(bounceDirection);

        float3 bounceHit = bounceOrigin + bounce.distance * bounceDirection;
        uint tileIndex;
        bool isNewTile;
        uint cellIndex = HashGridCache_InsertCell(bounceHit, bounceDirection, bounce.distance, tileIndex, isNewTile);
        if (cellIndex == kGI1_InvalidId) return light;

        HashGridCache_TouchCell(tileIndex, isNewTile);
        return light + throughput * (HashGridCache_FilteredDirect(cellIndex) + HashGridCache_FilteredIndirect(cellIndex));
    }

    return light;
}

[numthreads(8, 8, 1)]
void TraceReflections(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_BufferDimensions)) return;

    float depth = g_Depth.Load(int3(did, 0));
    float3 specularAlbedo = g_Guide_SpecularAlbedo.Load(int3(did, 0)).rgb;
    if (depth <= 0.0f || all(specularAlbedo < 1e-3f))
    {
        g_Reflection[did] = 0.0f;
        g_ReflectionDistance[did] = depth <= 0.0f ? SkyHitDistance : 0.0f;
        return;
    }

    float4 normalRoughness = g_Guide_NormalRoughness.Load(int3(did, 0));
    float3 normal = normalize(normalRoughness.xyz);
    float alpha = max(normalRoughness.w * normalRoughness.w, 1e-3f);
    float3 world = DepthPosition(did, depth);
    float3 toEye = normalize(g_Frame.eye.xyz - world);

    // Apart from the global illumination's rays from the same pixel (TraceLight seeds by the pixel alone).
    Random random = MakeRandom(did.x + did.y * g_BufferDimensions.x + g_BufferDimensions.x * g_BufferDimensions.y, g_FrameIndex);
    float cosineToEye;
    float3 direction = SpecularDirection(normal, toEye, alpha, random.rand2(), cosineToEye);
    float3 origin = OffsetFromScreenSurface(world, normal);
    float hitDistance;
    bool isRough = normalRoughness.w >= RoughSurface;
    float3 light = ReflectedLight(origin, normal, CopyTolerance(world), direction, PixelCone(world, g_Frame.rayCone.x + alpha), isRough, random, hitDistance);
    g_Reflection[did] = float4(RemoveNaNs(light) * specularAlbedo * g_Frame.nearFar.w, 1.0f);
    g_ReflectionDistance[did] = hitDistance;
}
