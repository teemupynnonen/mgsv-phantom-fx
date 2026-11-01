// Path-traced reflections (docs/DEVELOPMENT.md, "Path tracing"): with DLSS Ray Reconstruction denoising,
// a ray from every render pixel along a reflection its surface's roughness spreads (GGX's visible normals).
// On a rough surface the reflection is a faint blur: its ray's hit takes its light from the light cache (CachedLight),
// one ray a pixel. On a glossy one its hit has its material (HitMaterialOf): the sun, a light and a self-lit mesh
// afresh, then a choice by their shares between its specular lobe, whose ray goes on the same way where it is glossy
// too (a reflection in a reflection), and its diffuse one, whose ray ends the path at the sky's light or the light
// cache. A ray that gets away takes the sky as the game draws it (SkySeen). The light goes out times the surface's
// specular albedo (the Ray Reconstruction guide) and the exposure, for the resolve to add to the game's specular
// light, and the first hit's distance goes to Ray Reconstruction to move the reflection with what it reflects. A ray
// that hit something also writes the game's reflection map, which its blend reads as it reads the screen-space one's.
// Glass and mirrors have their own pass, once the game has drawn them (TraceSurfaceReflections).
#include "gi_hit_light.hlsli"

Texture2D<float4> g_Guide_NormalRoughness : register(t18); // The world-space normal, the roughness in w (ray_tracing::ReconstructionGuides)
Texture2D<float4> g_Guide_SpecularAlbedo : register(t19);  // The share of light the surface reflects towards the eye
// The forward pass's rivers and liquids (forward_shadows), one frame old: where one drew, the trace below is
// its own surface, not the riverbed's behind it.
// The sun's shadow on a surface itself, for the forward pass's multiply and the glass composite.
RWTexture2D<float> g_ForwardShadow : register(u100);
RWTexture2D<float4> g_Reflection : register(u93);          // The reflected light times the specular albedo and the exposure; a: how far across the screen its hit is
RWTexture2D<float> g_ReflectionDistance : register(u94);   // The ray's hit distance; FP16's largest where it got away
RWTexture2D<float4> g_ReflectionMap : register(u98);       // The hit as the game's reflection map holds it (ReflectionBlend): rgb its light, a how much it blends
Texture2D<float> g_SurfaceDepth : register(t26);           // The nearest glass or mirror the game drew at each pixel (forward_surfaces.hpp); 0 without
Texture2D<float4> g_SurfaceNormal : register(t27);         // Its world-space normal, its roughness in w
RWTexture2D<float4> g_SurfaceReflection : register(u99);   // What it reflects, times the exposure; a: the ray's hit distance

// Water's reflectance (F0 0.02) at the eye's angle over GGX's lobe: Karis' fit of the split sum ("Physically
// Based Shading on Mobile", 2014), as the sea's guides use it. A water surface with its ripples' roughness,
// not a perfect mirror's.
float WaterReflectance(float toEyeN, float roughness)
{
    const float4 c0 = float4(-1, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * toEyeN)) * r.x + r.y;
    float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return 0.02 * ab.x + ab.y;
}

// What a river's reflected light keeps when it is added: the game's own water draws a sky reflection into its
// colour and ours is added over it, so the full reflectance read as a mirror.
static const float ForwardReflectionStrength = 0.3f;
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
// The game's screen-space reflections (LocalReflectionMask and LocalReflectionTPP's disassembly) leave out surfaces
// rougher than this (their cPSObject c11.z), and those their Fresnel takes less than this much from, and fade away
// over this far from the eye, in metres.
static const float MostReflectingRoughness = 0.9f, LeastGameFresnel = 0.1f, GameReflectionReach = 100.0f;
// The brightest hit the game's blend takes, the scene's value at the top of the game's tonemap: a ray's light is
// not filtered as the screen's is, and at the blend's strength one bright hit would be a firefly.
static const float BrightestHit = 4.0f;
// Past the blend's reach however far that is (ReflectionBlend).
static const float BehindTheEye = 1000.0f;
// The panes a ray goes on past at most: the two sides of a helicopter, a window behind a window.
static const uint MostPanes = 4;

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

// The Fresnel of the game's screen-space reflections, which goes by the roughness and not by the material's F0.
float GameFresnel(float roughness, float toEyeN)
{
    float rate = 1.0f - roughness;
    rate *= rate;
    rate *= rate;

    return rate + (1.0f - rate) * (1.0f - toEyeN);
}

// How much of a ray's hit the game's blend takes at a surface: nothing where its screen-space reflections leave the
// surface out, and fading with the surface's distance and as the ray turns towards the eye.
float GameReflectionFade(float3 world, float3 normal, float3 direction, float roughness, float fresnel)
{
    if (roughness > MostReflectingRoughness || fresnel < LeastGameFresnel) return 0.0f;

    float3 forward = normalize(g_Frame.viewProjection[3].xyz);
    float distanceFade = 1.0f - saturate(dot(world - g_Frame.eye.xyz, forward) / GameReflectionReach);
    return distanceFade * saturate(1.0f + 2.0f * min(dot(direction + 0.05f * normal, forward), 0.0f));
}

// How far across the screen a ray went, as a share of the screen's width and height.
float ScreenTravel(float3 from, float3 to)
{
    float4 start = mul(g_Frame.viewProjection, float4(from, 1.0f));
    float4 end = mul(g_Frame.viewProjection, float4(to, 1.0f));
    if (end.w <= 0.0f) return BehindTheEye;

    return length((end.xy / end.w - start.xy / start.w) * float2(0.5f, -0.5f));
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

// The closest hit along a specular ray, past glass (GI_GEOMETRY_GLASS). A pane reflects the ray by chance, as much of it
// as HitMaterialOf says, and lets it through otherwise: `origin`, `normal`, `tolerance` and `direction` are left as the
// ray goes on from the last pane, and `travelled` grows by the way to it.
SceneHit TraceReflectionRay(inout float3 origin, inout float3 normal, inout float tolerance, inout float3 direction, float2 cone, inout float travelled, inout Random random)
{
    SceneHit hit;
    for (uint pane = 0; pane <= MostPanes; ++pane)
    {
        uint copies;
        hit = TraceFromSurface(origin, normal, tolerance, direction, MAX_HIT_DISTANCE, GI_MASK_SCENE, true, LightCone(origin), RayAlongReflection, copies);
        if (!hit.hit || !(g_Geometries[FirstGeometry(g_Instances[hit.instance]) + hit.geometry].flags & GI_GEOMETRY_GLASS)) return hit;

        travelled += hit.distance;
        HitMaterial glass = HitMaterialOf(VisibilityOf(hit, direction), cone.x + travelled * cone.y, false);
        bool isReflected = random.rand() < luminance(glass.specularColor);
        normal = isReflected ? glass.geometric : -glass.geometric;
        if (isReflected) direction = reflect(direction, glass.normal);
        origin = OffsetFromTriangle(glass.world, normal);
        // Going on, a wall behind the pane is no copy of it: a mirror's would be skipped, and the room behind it shown.
        tolerance = isReflected ? CopyTolerance(glass.world) : 0.0f;
    }

    hit.hit = false;
    return hit;
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
        SceneHit hit = TraceReflectionRay(origin, normal, tolerance, direction, cone, travelled, random);
        if (!hit.hit) return light + throughput * SkySeen(direction);

        travelled += hit.distance;
        if (ray == 1) hitDistance = min(travelled, SkyHitDistance);
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
    float4 specularAlbedo = g_Guide_SpecularAlbedo.Load(int3(did, 0));
    float4 normalRoughness = g_Guide_NormalRoughness.Load(int3(did, 0));
    if (depth <= 0.0f || all(specularAlbedo.rgb < 1e-3f))
    {
        g_Reflection[did] = 0.0f;
        g_ReflectionMap[did] = 0.0f;
        g_ReflectionDistance[did] = depth <= 0.0f ? SkyHitDistance : 0.0f;
        g_ForwardShadow[did] = 1.0f;
        return;
    }

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
    float3 exposed = RemoveNaNs(light) * g_Frame.nearFar.w;
    bool isGameBlended = hitDistance < SkyHitDistance && specularAlbedo.a > 0.0f;
    float fresnel = GameFresnel(normalRoughness.w, saturate(dot(normal, toEye)));
    g_Reflection[did] = float4(exposed * specularAlbedo.rgb, isGameBlended ? ScreenTravel(world, origin + hitDistance * direction) : BehindTheEye);
    g_ReflectionMap[did] = isGameBlended ? float4(min(exposed, BrightestHit) * fresnel, GameReflectionFade(world, normal, direction, normalRoughness.w, fresnel)) : 0.0f;
    g_ReflectionDistance[did] = hitDistance;

    // The sun's shadow on a surface itself is the surface trace's (TraceSurfaceReflections): here only the
    // pixels it does not cover get the lit value, which the forward multiply and the glass composite skip.
    g_ForwardShadow[did] = 1.0f;
}

// What glass and mirrors reflect, from the surfaces the game drew in front of the rest (forward_surfaces.hpp): as a mirror does, spread
// by the material's roughness, a ray a pixel. Nothing to the game's blend or to the resolve: their weights are the game's
// own, in the surface's draw, and this frame's scene is built.
[numthreads(8, 8, 1)]
void TraceSurfaceReflections(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_BufferDimensions)) return;

    float depth = g_SurfaceDepth.Load(int3(did, 0));
    if (depth <= 0.0f) return;

    float4 normalRoughness = g_SurfaceNormal.Load(int3(did, 0));
    // A river or a liquid (WaterSurface, surface_shaders.hpp) marks itself with a negative roughness: its normal is its
    // ripples' as the game's shader builds them, and what it reflects is scaled by water's reflectance. Without one
    // (xyz 0: the draw had no tangent frame) the normal comes from its depth's slope.
    const bool isWater = normalRoughness.w < 0.0f;
    normalRoughness.w = abs(normalRoughness.w);
    float3 world = DepthPosition(did, depth);
    if (isWater && all(normalRoughness.xyz == 0.0f))
    {
        const uint2 last = g_BufferDimensions - 1;
        const uint2 rightPixel = min(did + uint2(1, 0), last);
        const uint2 downPixel = min(did + uint2(0, 1), last);
        const float rightDepth = g_SurfaceDepth.Load(int3(rightPixel, 0));
        const float downDepth = g_SurfaceDepth.Load(int3(downPixel, 0));
        float3 slope = float3(0, 0, 1);
        if (rightDepth > 0.0f && downDepth > 0.0f)
            slope = cross(DepthPosition(rightPixel, rightDepth) - world, DepthPosition(downPixel, downDepth) - world);
        if (!all(slope == 0.0f)) slope = normalize(slope);
        const float3 toEyeNow = normalize(g_Frame.eye.xyz - world);
        if (dot(slope, toEyeNow) < 0.0f) slope = -slope;
        normalRoughness = float4(slope, normalRoughness.w);
    }
    float3 normal = normalize(normalRoughness.xyz);
    float alpha = max(normalRoughness.w * normalRoughness.w, 1e-3f);
    float3 toEye = normalize(g_Frame.eye.xyz - world);

    // Apart from the rays from the surface behind it (TraceReflections).
    Random random = MakeRandom(did.x + did.y * g_BufferDimensions.x + 2 * g_BufferDimensions.x * g_BufferDimensions.y, g_FrameIndex);
    float cosineToEye;
    float3 direction = SpecularDirection(normal, toEye, alpha, random.rand2(), cosineToEye);
    float3 origin = OffsetFromScreenSurface(world, normal);
    float hitDistance;
    bool isRough = normalRoughness.w >= RoughSurface;
    float3 light = ReflectedLight(origin, normal, CopyTolerance(world), direction, PixelCone(world, g_Frame.rayCone.x + alpha), isRough, random, hitDistance);
    // A river or a liquid reflects as water does, not as glass: its reflectance at the eye's angle scales what it
    // reflected (its weight, recorded at the draw, is the menu's share).
    if (isWater) light *= WaterReflectance(saturate(dot(normal, toEye)), normalRoughness.w);
    g_SurfaceReflection[did] = float4(min(RemoveNaNs(light) * g_Frame.nearFar.w, BrightestHit), hitDistance);

    // the surface that is can be seen (menu, "Water: fallback normals").

    // The surface's own sun shadow, for the composite to dim what it covers (surface_shaders.hpp, Composite): a ray
    // from the surface towards a point on the sun's disc, one sample, seeded by the pixel alone. Water takes no sun
    // shadow (Composite reads 1 at its pixels), so it traces none: no ray there is ever read.
    if (isWater)
    {
        g_ForwardShadow[did] = 1.0f;
        return;
    }
    Random sunRandom = MakeRandom(did.x + did.y * g_BufferDimensions.x, 0u);
    g_ForwardShadow[did] = VisibilityFromSurface(origin, normal, CopyTolerance(world), SunSample(sunRandom.rand2()), 1e4f,
        ShadowRayTowards(RayTowardsSun));
}
