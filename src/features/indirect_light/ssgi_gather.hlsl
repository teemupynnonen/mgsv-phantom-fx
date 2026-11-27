// The gather: up to 16 taps around each pixel at the gather's resolution, each rejected or
// weighted by plane elevation, source facing and distance, returning the mean radiance
// (albedo x diffuse light + specular) of the taps that survive. Alpha is validity. It reads the
// game's diffuse light and writes its own target, so reading neighbours while writing this pixel
// is no hazard.
#include "ssgi_common.hlsli"
Texture2D<float> Depth : register(t0);        // Reversed-Z, 0 on the sky
Texture2D<float4> Normals : register(t1);     // The G-buffer's view-space normals
Texture2D<float4> LightSource : register(t2); // The game's diffuse light, as the resolve reads it
Texture2D<float4> Albedo : register(t3);      // Through the resolve's sRGB view, so linear
Texture2D<float4> Specular : register(t4);    // The specular light the resolve adds (its inLightSpecular)
// The prefiltered distances and their smaller copies (visibility_bitmask's prefilter), negative on
// foliage; the per-tap occlusion test marches them.
Texture2D<float> Distances : register(t5);
RWTexture2D<float4> Bounce : register(u0);

#define SSGI_MAX_SAMPLES 16

static const uint DistanceMipCount = 5;
static const float DistanceMipOffset = 3.3;
static const uint VisibilityStepLimit = 8;
static const float MinRadiusPixels = 6.0;  // The reach on screen never falls under this
static const float MaxRadiusShare = 0.3;   // ...or over this share of the height

// The distance at a sample `offset` pixels from the receiver, from a smaller copy the farther it
// lies; negative on foliage.
float signedDistanceAt(int2 texel, float offset)
{
    uint mip = uint(round(clamp(log2(max(offset, 1.0)) - DistanceMipOffset, 0, DistanceMipCount - 1)));
    uint2 mipSize = max(Size >> mip, 1);
    return Distances.Load(int3(min(uint2(max(texel, int2(0, 0))) >> mip, mipSize - 1), mip));
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint2 pixel = tid.xy;
    const float scale = max(Scale, 1.0);
    const int2 gs = (int2(Size) + (int)scale - 1) / (int)scale;
    if (any(pixel >= (uint2)gs)) return;

    // The gather may run below full resolution; its threads map onto full-resolution pixels, which
    // is where every G-buffer read and every sample offset lives.
    const int2 centre = (int2)floor((float2(pixel) + 0.5) * scale);
    if (any(centre >= int2(Size))) return;

    const float z = distanceOf(Depth.Load(int3(centre, 0)));
    if (!hasGeometry(z))
    {
        Bounce[pixel] = 0;
        return;
    }

    const float3 P = viewPosition(float2(centre), z);
    const float3 N = decodeNormal(Normals.Load(int3(centre, 0)).xyz);

    // The radius is fixed in metres and projected to pixels from this pixel's depth, so the
    // reach is the same distance everywhere instead of growing with it. The pixel clamp keeps far
    // pixels from collapsing onto themselves and near ones from costing the whole screen; the world
    // radius follows the clamped reach so the thickness and the falloff agree with the taps.
    const float metresPerPixel = 2.0 * z * Scene[21].y / Size.y;
    const float radiusPixels = clamp(max(Radius, 0.01) / metresPerPixel, MinRadiusPixels, MaxRadiusShare * Size.y);
    const float radiusWorld = radiusPixels * metresPerPixel;

    const uint sampleCount = (uint)clamp(Samples, 1.0, (float)SSGI_MAX_SAMPLES);
    const uint minSamples = (uint)clamp(MinSamples, 1.0, (float)SSGI_MAX_SAMPLES);
    const uint frameSalt = (uint)(Random * 65535.0);
    const float rotation = hash01(pixel, frameSalt) * 6.2831853;

    float3 sum = 0;
    float weightSum = 0;
    uint validSamples = 0;

    [loop]
    for (uint index = 0; index < sampleCount; ++index)
    {
        const float radiusJitter = hash01(pixel, index * 977u + frameSalt + 1u);
        const int2 samplePixel = (int2)round(float2(centre) + diskSample(index, sampleCount, rotation, radiusJitter) * radiusPixels);
        if (any(samplePixel < 0) || any(samplePixel >= int2(Size))) continue;

        const float sampleZ = distanceOf(Depth.Load(int3(samplePixel, 0)));
        if (!hasGeometry(sampleZ)) continue;

        const float3 S = viewPosition(float2(samplePixel), sampleZ);
        const float3 delta = S - P;
        const float distanceSq = dot(delta, delta);
        if (distanceSq < 1e-8) continue;

        const float distance = sqrt(distanceSq);
        const float3 direction = delta / distance;

        // Visibility: march the tap's ray over the prefiltered distances; a surface nearer than the
        // ray blocks the sample. The disk alone would not see an occluder and would bleed its light
        // through.
        const uint visibilitySteps = (uint)clamp(Visibility, 0.0, (float)VisibilityStepLimit);
        if (visibilitySteps > 0)
        {
            const float2 rayPixels = float2(samplePixel) - float2(centre);
            const float pixelLength = length(rayPixels);
            bool blocked = false;
            [loop]
            for (uint k = 0; k < visibilitySteps; ++k)
            {
                const float f = (k + 0.5) / visibilitySteps;
                const int2 q = int2(round(float2(centre) + rayPixels * f));
                const float surface = abs(signedDistanceAt(q, f * pixelLength));
                // The prefiltered distances are view depths, so the ray is compared by its view depth
                // too: its Euclidean length off the centre runs longer by sqrt(1 + x^2 + y^2), up to a
                // fifth at the corners, which would read as occluders.
                const float rayDepth = -(P + direction * (f * distance)).z;
                const float thickness = clamp(0.12 * rayDepth, 0.02, max(radiusWorld, 0.02));
                if (surface < rayDepth - thickness)
                {
                    blocked = true;
                    break;
                }
            }
            if (blocked) continue;
        }

        // A sample on the same surface is the pixel's own light: adding it back is what makes a
        // gather read as a halo rather than as bounce. The soft ramp is the cosine term too.
        const float planeWeight = smoothstep(0.02, 0.35, dot(N, direction));
        if (planeWeight <= 0) continue;

        const float sourceFacing = dot(decodeNormal(Normals.Load(int3(samplePixel, 0)).xyz), -direction);
        if (sourceFacing <= 0) continue;

        // A fixed inverse-square against the radius; the gather returns a normalized mean, so the
        // kernel's shape cancels wherever the neighbourhood is uniform.
        const float range = 1.0 + 4.0 * distanceSq / (radiusWorld * radiusWorld);
        const float weight = planeWeight * sourceFacing / range;

        // Source radiance: albedo times diffuse light turns the gather into colour bleeding, and
        // the specular is what lets a light fixture contribute more than a dim wall.
        const float3 radiance = Albedo.Load(int3(samplePixel, 0)).rgb * LightSource.Load(int3(samplePixel, 0)).rgb
                              + Specular.Load(int3(samplePixel, 0)).rgb;
        sum += radiance * weight;
        weightSum += weight;
        ++validSamples;
    }

    const bool valid = validSamples >= minSamples;
    Bounce[pixel] = float4(valid ? sum / max(weightSum, 1e-6) : 0.0, valid ? 1.0 : 0.0);
}
