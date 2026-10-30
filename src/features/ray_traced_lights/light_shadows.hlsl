// The point and spot lights' ray-traced shadows (docs/DEVELOPMENT.md, "Ray-traced lights"), through the
// ray-traced scene, leaving each pixel's surface as the sun's shadow rays do (gi_shadow.hlsli).
#include "gi_shadow.hlsli"

// Soft shadows for all the frame's point and spot lights in one value a pixel: the share of their
// light that reaches it, the ratio estimator of Heitz, Hill and McGuire ("Combining Analytic Direct
// Illumination and Stochastic Shadows", I3D 2018) over the lights together. The game's light shaders
// give each light's light; rays give the share: g_Frame.localLightCount.y rays a pixel, each to a point
// on a light chosen in proportion to its light there without shadows (systematic sampling, so the rays
// share out over the lights), light that gets through over all the light. Every light's light times
// the share gives each light its own shadow where lights do not overlap, and the right diffuse light
// in total where they do. A spatial filter no wider than the penumbra (FilterLocalLightShadows) smooths
// the rays' noise and keeps contact shadows sharp; no history, so nothing trails.

RWTexture2D<float> g_LocalLightShadow : register(u89);   // What the light pass reads: the filtered share
RWTexture2D<float2> g_LocalShadowRays : register(u90);   // x: the share the rays found; y: the penumbra's width, pixels; then the second pass's
RWTexture2D<float2> g_LocalShadowFirst : register(u91);  // The first pass's

// The penumbra the filter may blur across, pixels: at most this wide.
static const float MaxPenumbraPixels = 64.0f;

[numthreads(8, 8, 1)]
void TraceLocalLightShadows(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_Frame.dimensions.xy)) return;

    float2 result = float2(1.0f, 0.0f);
    float depth = g_Depth.Load(int3(did, 0));
    if (depth > 0.0f)
    {
        float3 position = DepthPosition(did, depth);
        float3 normal = DecodeNormal(g_Normal.Load(int3(did, 0)));
        float3 toLight;
        float lightDistance;
        float total = 0.0f;
        for (uint i = 0; i < g_Frame.localLightCount.x; ++i) total += luminance(LocalLightAt(g_Frame.localLights[i], position, normal, toLight, lightDistance));

        if (total > 0.0f)
        {
            Random random = MakeRandom(g_Frame.dimensions.z, did.y * g_Frame.dimensions.x + did.x);
            ShadowSurface surface = ShadowSurfaceAt(position, normal);
            float2 cone = PixelCone(position, 0.0f);
            uint rays = max(g_Frame.localLightCount.y, 1u);
            float spacing = total / float(rays);
            // Neighbouring pixels start their picks and their points on the lights far apart (interleaved
            // gradient noise), and a pixel's rays cover each light's disc evenly (the R2 sequence from that
            // start), so the filter averages unlike samples; white noise clumped them. DLSS Ray Reconstruction
            // takes the rays without the filter and asks for white noise: no pattern across the screen.
            float2 start = g_Frame.reconstruction.x != 0u
                ? random.rand2()
                : float2(InterleavedGradientNoise(did, g_Frame.dimensions.z), InterleavedGradientNoise(did + uint2(47, 17), g_Frame.dimensions.z));
            float threshold = start.x * spacing;
            float running = 0.0f, penumbra = 0.0f, visible = 0.0f, shaded = 0.0f;
            uint traced = 0;
            for (uint i = 0; i < g_Frame.localLightCount.x && traced < rays; ++i)
            {
                GILocalLight light = g_Frame.localLights[i];
                running += luminance(LocalLightAt(light, position, normal, toLight, lightDistance));
                while (traced < rays && threshold < running)
                {
                    // A point on the light's disc facing the pixel, its radius in falloff.w.
                    float3 target = light.position.xyz + DiscOffset(toLight, light.falloff.w, frac(start + R2(traced)));
                    float3 offset = target - position;
                    float targetDistance = length(offset);
                    float3 direction = offset / targetDistance;

                    float3 origin, facing;
                    float tolerance;
                    ShadowRayStart(surface, direction, origin, facing, tolerance);
                    float rayLength = targetDistance - ClearanceTowards(light, direction);
                    float blocker = FLT_MAX, transmittance = 1.0f;
                    bool blocked = false;
                    if (rayLength > 0.0f)
                        blocked = SurfaceRayBlocked(origin, facing, tolerance, direction, rayLength, cone, true, 0.0f, ShadowRayTowards(RayTowardsLamp), random, blocker, transmittance);

                    float through = blocked ? 0.0f : transmittance;
                    visible += through;
                    if (through < 1.0f)
                    {
                        // The penumbra a blocker this far along casts from a light this wide, metres, by the light it takes.
                        penumbra += (1.0f - through) * 2.0f * light.falloff.w * blocker / max(targetDistance - blocker, 1e-3f);
                        shaded += 1.0f - through;
                    }

                    ++traced;
                    threshold += spacing;
                }
            }

            float footprint = max(distance(position, g_Frame.eye.xyz) * g_Frame.rayCone.x, 1e-6f);
            result = float2(visible / float(max(traced, 1u)), shaded > 0.0f ? min(penumbra / shaded / footprint, MaxPenumbraPixels) : 0.0f);
        }
    }

    g_LocalShadowRays[did] = result;
    // DLSS Ray Reconstruction denoises the image: the light pass reads the rays as they are.
    if (g_Frame.reconstruction.x != 0u) g_LocalLightShadow[did] = result.x;
}

// Three edge-avoiding a-trous passes (Dammertz et al., "Edge-Avoiding A-Trous Wavelet Transform for
// Fast Global Illumination Filtering", 2010), 1, 2 and 4 pixels apart: a neighbour counts by a 3 x 3
// B-spline's weight, as near as its depth and normal are. A pass blurs where the widest penumbra among
// the pixel's neighbours on its own surface is at least its step, so a contact shadow stays as sharp as
// the rays found it. The widest, not the pixel's own: a pixel inside a soft shadow whose rays all got
// through finds no blocker and no penumbra, and kept its full light amid smoothed neighbours (19% of a
// wall's soft shadow in frame-159975, 2026-10-24: speckles new every frame). Each pass hands on that
// widest penumbra.
// g_Dispatch: x the pass (0 to 2), y its step in pixels. The first reads the rays' result and writes
// g_LocalShadowFirst, the second writes back into g_LocalShadowRays, the third what the light pass reads.
[numthreads(8, 8, 1)]
void FilterLocalLightShadows(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_Frame.dimensions.xy)) return;

    uint pass = g_Dispatch.x;
    int step = int(g_Dispatch.y);
    float2 centre = pass == 1 ? g_LocalShadowFirst[did] : g_LocalShadowRays[did];
    float2 filtered = centre;
    float depth = g_Depth.Load(int3(did, 0));
    if (depth > 0.0f)
    {
        float centreDepth = toLinearDepth(depth, g_Frame.nearFar.xy);
        float3 centreNormal = DecodeNormal(g_Normal.Load(int3(did, 0)));
        float sum = 0.0f, weights = 0.0f, widest = 0.0f;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                int2 pixel = clamp(int2(did) + int2(x, y) * step, 0, int2(g_Frame.dimensions.xy) - 1);
                float neighbourDepth = g_Depth.Load(int3(pixel, 0));
                if (neighbourDepth <= 0.0f) continue;

                float2 value = pass == 1 ? g_LocalShadowFirst[pixel] : g_LocalShadowRays[pixel];
                float surface = exp(-abs(toLinearDepth(neighbourDepth, g_Frame.nearFar.xy) - centreDepth) / (0.02f * centreDepth + 0.01f));
                surface *= pow(saturate(dot(centreNormal, DecodeNormal(g_Normal.Load(int3(pixel, 0))))), 8.0f);
                if (surface > 0.25f) widest = max(widest, value.y);

                float weight = (x == 0 ? 0.5f : 0.25f) * (y == 0 ? 0.5f : 0.25f) * surface;
                sum += weight * value.x;
                weights += weight;
            }

        if (widest >= float(step) && weights > 0.0f) filtered.x = sum / weights;
        filtered.y = widest;
    }

    if (pass == 2) g_LocalLightShadow[did] = filtered.x;
    else if (pass == 0) g_LocalShadowFirst[did] = filtered;
    else g_LocalShadowRays[did] = filtered;
}

RWStructuredBuffer<GILightProbe> g_LightProbe : register(u92);

// The lamp probe, one thread: every light at the centre pixel, and a ray to each one's centre from where
// its shadow rays leave, as TraceLocalLightShadows traces them; the strongest kept, most light first.
// Needs this frame's lights' shadow passes done.
[numthreads(1, 1, 1)]
void ProbeLocalLights()
{
    GILightProbe probe = (GILightProbe)0;
    uint2 centre = g_Frame.shadowDiagnostics.zw;
    float depth = g_Depth.Load(int3(centre, 0));
    if (depth > 0.0f)
    {
        float3 position = DepthPosition(centre, depth);
        float3 normal = DecodeNormal(g_Normal.Load(int3(centre, 0)));
        ShadowSurface surface = ShadowSurfaceAt(position, normal);
        float2 cone = PixelCone(position, 0.0f);
        Random random = MakeRandom(g_Frame.dimensions.z, centre.y * g_Frame.dimensions.x + centre.x);
        probe.receiver = float4(position, distance(position, g_Frame.eye.xyz));
        probe.start = float4(surface.position - position, surface.exact ? 1.0f : 0.0f);
        probe.startHit = surface.exact ? ProbeHit(surface.hit) : uint4(0u, 0u, 0u, 0u);
        probe.view = ProbeHit(TraceClosest(g_Frame.eye.xyz, normalize(DepthPosition(centre, 1e-6f) - g_Frame.eye.xyz), 1e5f));
        float reaching = 0.0f;
        for (uint i = 0; i < g_Frame.localLightCount.x; ++i)
        {
            float3 toLight;
            float lightDistance;
            GILocalLight light = g_Frame.localLights[i];
            float amount = luminance(LocalLightAt(light, position, normal, toLight, lightDistance));
            if (!(amount > 0.0f)) continue;

            float3 origin, facing;
            float tolerance;
            ShadowRayStart(surface, toLight, origin, facing, tolerance);
            float rayLength = lightDistance - ClearanceTowards(light, toLight);
            float blocker = FLT_MAX, transmittance = 1.0f;
            bool blocked = false;
            if (rayLength > 0.0f)
                blocked = SurfaceRayBlocked(origin, facing, tolerance, toLight, rayLength, cone, true, 0.0f, ShadowRayTowards(RayTowardsLamp), random, blocker, transmittance);

            float through = blocked ? 0.0f : transmittance;
            probe.share.z += amount;
            probe.share.w += 1.0f;
            reaching += amount * through;

            uint slot = GI_PROBE_LIGHTS;
            for (uint k = GI_PROBE_LIGHTS; k > 0; --k)
                if (amount > probe.lights[k - 1].y) slot = k - 1;
            if (slot == GI_PROBE_LIGHTS) continue;

            for (uint k = GI_PROBE_LIGHTS - 1; k > slot; --k)
            {
                probe.lights[k] = probe.lights[k - 1];
                probe.through[k] = probe.through[k - 1];
                for (uint h = 0; h < GI_PROBE_LIGHT_HITS; ++h) probe.hits[k * GI_PROBE_LIGHT_HITS + h] = probe.hits[(k - 1) * GI_PROBE_LIGHT_HITS + h];
            }

            // The blocker or the nearest hair, then whatever lies past it on the way to the light.
            bool met = blocker != FLT_MAX;
            probe.lights[slot] = float4(i + 1, amount, lightDistance, met ? blocker : -1.0f);
            probe.through[slot] = float4(through, 0.0f, 0.0f, 0.0f);
            float from = max(blocker - 0.01f, 0.0f);
            for (uint h = 0; h < GI_PROBE_LIGHT_HITS; ++h)
            {
                SceneHit hit = (SceneHit)0;
                if (met && from < rayLength) hit = TraceClosest(origin, toLight, from, rayLength, GI_MASK_SCENE, true, false, cone);
                probe.hits[slot * GI_PROBE_LIGHT_HITS + h] = ProbeHit(hit);
                from = hit.hit ? hit.distance + 0.001f : rayLength;
            }
        }

        probe.share.x = probe.share.z > 0.0f ? reaching / probe.share.z : 1.0f;
        probe.share.y = g_LocalLightShadow[centre];
    }

    g_LightProbe[0] = probe;
}
