#pragma once

// HLSL for GTVBAO and GT-VBGI (indirect_light.hpp), compiled at runtime with d3dcompiler_47. The
// occlusion is ported from three-gtvbao (norio, MIT; GT-VBAO by Mirko Salm, CC0 / MIT; SSRT3 by
// Olivier Therrien, MIT), the light gathering and its slice sampling from GT-VBGI (Mirko Salm,
// https://www.shadertoy.com/view/lfdBWn, taken under CC0), the noise and the denoiser from XeGTAO
// (Intel, MIT). The MIT licenses are in licenses/.
namespace phantom_fx::indirect_light::shaders
{
// The constant buffer of every compute shader here; each reads the fields it needs.
#define PHANTOM_FX_PARAMETERS R"(
cbuffer Parameters : register(b0)
{
    uint2 Size;              // Render size, the depth buffer's
    float Radius;            // Metres
    float MaxRadiusPixels;   // Caps the march for surfaces close to the camera
    float OcclusionStrength; // GTVBAO: exponent on the visibility
    float LightStrength;     // GT-VBGI: multiplies the light
    float DiffuseScale;      // GT-VBGI: 0 leaves the game's diffuse light out, for the diagnostics
    float Padding;
};
)"

// XeGTAO's depth prefilter: the distances the march samples at the render size, and four smaller
// copies, each a 2 x 2 reduction of the one before (XeGTAO's depth MIPs). The march reads farther
// samples from smaller copies: across up to 15% of the render height, full-size reads missed the
// cache on nearly every sample. Each 8 x 8 group covers 16 x 16 pixels. The sign carries the
// foliage material, negative on foliage, so the march reads one texel a sample.
inline constexpr char PrefilterDepths[] = PHANTOM_FX_PARAMETERS R"(
cbuffer Scene : register(b1)
{
    float4 Scene[30];
};

Texture2D<float> Depth : register(t0);     // Reversed-Z, 0 on the sky
// The G-buffer's specular target, whose z is the material; unbound until a sun pass has bound it.
Texture2D<float4> Specular : register(t1);
RWTexture2D<float> OutDistance0 : register(u0);
RWTexture2D<float> OutDistance1 : register(u1);
RWTexture2D<float> OutDistance2 : register(u2);
RWTexture2D<float> OutDistance3 : register(u3);
RWTexture2D<float> OutDistance4 : register(u4);

// Fox Engine's material for grass, bushes and tree leaves, in 255ths.
static const float FoliageMaterial = 43;
// Far enough to occlude nothing, and finite, so the reduction can weigh it.
static const float SkyDistance = 1e6;

groupshared float scratch[8][8];

float signedDistanceAt(uint2 pixel)
{
    pixel = min(pixel, Size - 1);
    float depth = Depth[pixel];
    float distance = depth > 0 ? Scene[21].z / (depth - Scene[21].w) : SkyDistance;
    return round(Specular[pixel].z * 255) == FoliageMaterial ? -distance : distance;
}

// XeGTAO's DepthMIPFilter: the weighted mean of four distances, fading out those well in front of
// the farthest, so a near edge does not pull a coarse texel forward off the surface behind it. The
// texel is foliage where foliage has at least half the weight.
float reduce(float4 values)
{
    float4 distances = abs(values);
    float farthest = max(max(distances.x, distances.y), max(distances.z, distances.w));
    float falloffRange = 0.615 * 0.75 * Radius, falloffFrom = 0.75 * Radius - falloffRange;
    float4 weights = saturate(1 - (farthest - distances - falloffFrom) / falloffRange);
    float weightSum = dot(weights, 1);
    float distance = dot(weights, distances) / weightSum;
    return dot(weights, float4(values < 0)) * 2 >= weightSum ? -distance : distance;
}

// Reduces the 2 x 2 values `stride` apart in scratch, from the top left at `local`.
float reduceScratch(uint2 local, uint stride)
{
    return reduce(float4(scratch[local.x][local.y], scratch[local.x + stride][local.y], scratch[local.x][local.y + stride],
        scratch[local.x + stride][local.y + stride]));
}

// Writes past the edges are dropped, and reads clamp to them.
[numthreads(8, 8, 1)]
void main(uint2 thread : SV_DispatchThreadID, uint2 local : SV_GroupThreadID)
{
    uint2 pixel = thread * 2;
    float4 values = float4(signedDistanceAt(pixel), signedDistanceAt(pixel + uint2(1, 0)), signedDistanceAt(pixel + uint2(0, 1)),
        signedDistanceAt(pixel + uint2(1, 1)));
    OutDistance0[pixel] = values.x;
    OutDistance0[pixel + uint2(1, 0)] = values.y;
    OutDistance0[pixel + uint2(0, 1)] = values.z;
    OutDistance0[pixel + uint2(1, 1)] = values.w;

    float reduced = reduce(values);
    OutDistance1[thread] = reduced;
    scratch[local.x][local.y] = reduced;

    // Each level, one thread in four reduces the 2 x 2 block it tops. Only that thread reads its own
    // cell, so it may overwrite it without waiting.
    GroupMemoryBarrierWithGroupSync();
    if (all(local % 2 == 0))
    {
        reduced = reduceScratch(local, 1);
        OutDistance2[thread / 2] = reduced;
        scratch[local.x][local.y] = reduced;
    }

    GroupMemoryBarrierWithGroupSync();
    if (all(local % 4 == 0))
    {
        reduced = reduceScratch(local, 2);
        OutDistance3[thread / 4] = reduced;
        scratch[local.x][local.y] = reduced;
    }

    GroupMemoryBarrierWithGroupSync();
    if (all(local % 8 == 0)) OutDistance4[thread / 8] = reduceScratch(local, 4);
}
)";

// The visibility bitmask march, once per pixel at render resolution (once per 2 x 2 for GT-VBGI, see
// Scale): for each slice, 32 sectors of
// the hemisphere around the view vector, set where a depth sample, taken as `thickness` deep,
// covers them. The horizons go through the cosine-weighted measure's CDF first (GT-VBAO), so the
// share of set sectors is the cosine-weighted occlusion. Also writes XeGTAO's depth edges for the
// denoiser.
//
// With LIGHT 0 (GTVBAO) it writes the visibility. With LIGHT 1 (GT-VBGI) it writes the light the
// sectors receive: each sample lights the sectors it covers first, as nearer samples hide what lies
// behind them, and that see its front (frontSectors), with the diffuse light its surface reflects.
// Equal sectors carry equal cosine weight, so the sum is the irradiance over pi, in the units of the
// game's diffuse light, which the resolve multiplies by the albedo.
//
// Fox Engine's view space looks down +z and mirrors x (P00 < 0). Flipping z, and x when mirrored,
// turns it into the usual view space looking down -z with x right and y up, which the maths below
// assumes; positions and the G-buffer's view-space normals take the same flip.
inline constexpr char March[] = PHANTOM_FX_PARAMETERS R"(
// The game's cPSScene as the lighting passes bind it (docs/DEVELOPMENT.md, "Constant buffers"):
// c7 = (0, 0, 1, 0) for the main view's perspective, c21 = (1 / P00, 1 / P11, B, A), c28 the
// jitter in NDC, which the reconstruction takes off as the engine's own passes do.
cbuffer Scene : register(b1)
{
    float4 Scene[30];
};

Texture2D<float> Depth : register(t0);    // Reversed-Z, 0 on the sky
Texture2D<float4> Normals : register(t1); // G-buffer view-space normals
#if LIGHT
Texture2D<float4> Diffuse : register(t2); // The game's diffuse light, as the resolve reads it
Texture2D<float4> Albedo : register(t3);  // Through the resolve's sRGB view, so linear
RWTexture2D<float3> OutLight : register(u0);
#else
RWTexture2D<unorm float> OutVisibility : register(u0);
#endif
// shaders::PrefilterDepths' distances and their smaller copies, negative on foliage.
Texture2D<float> Distances : register(t4);
RWTexture2D<unorm float> OutEdges : register(u1);

static const float Pi = 3.14159265;
static const uint Sectors = 32;
static const uint SliceCount = 2;
static const uint StepCount = 8;
// GT-VBGI marches one pixel in each 2 x 2, the top left, which its texel at half the render size
// stands for: the bounce is soft, and the march is nearly all its cost. shaders::Upsample brings it
// back to the render size. LightScale in indirect_light.cpp matches it.
static const uint Scale = LIGHT ? 2 : 1;
// The smaller copies of the distances, and XeGTAO's DepthMIPSamplingOffset: a sample 14 pixels or
// more away reads the first, and each doubling of the distance the next.
static const uint DistanceMipCount = 5;
static const float DistanceMipOffset = 3.3;

float distanceAt(int2 pixel)
{
    float depth = Depth[clamp(pixel, 0, int2(Size) - 1)];
    return Scene[21].z / (depth - Scene[21].w);
}

// The distance at a sample `offset` pixels from the receiver, from a smaller copy the farther it
// lies; negative on foliage.
float signedDistanceAt(int2 texel, float offset)
{
    uint mip = uint(round(clamp(log2(offset) - DistanceMipOffset, 0, DistanceMipCount - 1)));
    uint2 mipSize = max(Size >> mip, 1);
    return Distances.Load(int3(min(uint2(texel) >> mip, mipSize - 1), mip));
}

float3 viewPosition(float2 pixelCentre, float distance)
{
    float2 ndc = pixelCentre / Size * float2(2, -2) + float2(-1, 1) - Scene[28].xy;
    return float3(ndc * float2(abs(Scene[21].x), Scene[21].y), -1) * distance;
}

// DecodeViewSpaceNormal, as the game's own ambient obscurance reads the G-buffer.
float3 viewNormal(uint2 pixel)
{
    float3 encoded = Normals[pixel].xyz;
    float z = encoded.z * encoded.z * 2 - 1;
    float2 xy = encoded.xy * 2 - 1;
    xy *= sqrt(saturate(1 - z * z)) * rsqrt(max(dot(xy, xy), 1e-8));
    return normalize(float3(sign(Scene[21].x) * xy.x, xy.y, -z));
}

// XeGTAO: Hilbert curve order over 64 x 64 pixels driving the R2 sequence, so the 3 x 3 neighbourhood
// the denoiser averages holds well spread values.
float2 spatialNoise(uint2 pixel)
{
    uint x = pixel.x % 64, y = pixel.y % 64, index = 0;
    for (uint level = 32; level > 0; level /= 2)
    {
        uint regionX = (x & level) > 0;
        uint regionY = (y & level) > 0;
        index += level * level * ((3 * regionX) ^ regionY);
        if (regionY == 0)
        {
            if (regionX == 1)
            {
                x = 63 - x;
                y = 63 - y;
            }

            uint swap = x;
            x = y;
            y = swap;
        }
    }

    return frac(0.5 + index * float2(0.75487766624669276005, 0.5698402909980532659114));
}

float interleavedGradientNoise(float2 position)
{
    return frac(52.9829189 * frac(dot(position, float2(0.06711056, 0.00583715))));
}

float4 calculateEdges(float centre, float left, float right, float top, float bottom)
{
    float4 edges = float4(left, right, top, bottom) - centre;
    float slopeLR = (edges.y - edges.x) * 0.5;
    float slopeTB = (edges.w - edges.z) * 0.5;
    float4 slopeAdjusted = edges + float4(slopeLR, -slopeLR, slopeTB, -slopeTB);
    edges = min(abs(edges), abs(slopeAdjusted));
    return saturate(1.25 - edges / (centre * 0.011));
}

// Two bits per edge: 0, 1/3, 2/3 or 1.
float packEdges(float4 edges)
{
    edges = round(saturate(edges) * 2.9);
    return dot(edges, float4(64.0, 16.0, 4.0, 1.0) / 255.0);
}

// acos(x) / pi, the polynomial three-gtvbao and XeGTAO use.
float2 acosOverPi(float2 x)
{
    float2 r = (abs(x) * (-0.156583 / Pi) + 0.5) * sqrt(1 - abs(x));
    return x >= 0 ? r : 1 - r;
}

// GT-VBAO's slice-relative CDF of the cosine-weighted measure, for the horizon cosines h of one march
// direction (d = 1 along the slice direction, -1 against it): equal sectors then carry equal weight.
float2 remapHorizons(float2 h, float d, float normalAngle, float cosN, float sinN, float invNormalization)
{
    float3 n = d > 0 ? float3(1, 1, 0) : float3(3, -1, 4);
    h = max(h, sinN * d);
    float2 sinTheta = sqrt(1 - h * h);
    float2 theta = acosOverPi(h) * Pi;
    float2 cosTwoPsiPlusN = (2 * h * h - 1) * cosN - 2 * d * h * sinTheta * sinN;
    float2 t = cosN * n.x + cosTwoPsiPlusN * n.y + (theta * 2 * n.y * d + normalAngle * (n.z + 2 * n.y) - Pi) * sinN;
    return saturate(t * invNormalization);
}

#if LIGHT
// GT-VBGI's slice sampling (Salm's SampleSliceDir, its cheaper approximation): a slice direction across
// the view vector from `random` in [0, 1), drawn with the density of the slices' cosine-weighted
// share of the hemisphere, so every slice weighs the same. `normal` is the receiver's normal across
// the view vector, in the slice frame. Against the exact density it is off by 8% at most (checked at
// normals 0 to 89 degrees from the view vector).
float2 sampleSliceDirection(float2 normal, float random)
{
    float angle = random * Pi;
    float2 even = float2(cos(angle), sin(angle));
    float l = length(normal);
    if (l == 0) return even;

    // Into the half circle on the normal's side, then warped towards the normal.
    even *= dot(even, normal) < 0 ? -1 : 1;
    float2 n = normal / l;
    float x = even.x * n.y - even.y * n.x;
    // The stretch of the arc cosine, with Salm's bias to match the density; k is his arc sine
    // approximation's parameter.
    float s = l + (l - l * l) * 0.15;
    const float k = 0.21545;
    float a = 0.5 + 0.5 / k, b = 0.5 - 0.5 / k, c = 4 / k;
    float stretch = 0.5 - 0.5 * s;
    x *= a - sqrt(b * b + c * stretch * stretch);
    float u = (abs(x) * k - k - 1) * abs(x) + 1;
    float y = abs((x > 0 ? 2 : 0) - sqrt(saturate(u)));

    float2 direction;
    direction.y = (1 - y) / s;
    direction.x = sqrt(saturate(1 - direction.y * direction.y));

    return float2(direction.x * n.x - direction.y * n.y, direction.y * n.x + direction.x * n.y);
}

// GT-VBGI's backface rejection: the sectors that see the front of a sample's surface. A diffuse surface
// sends the same light every way its front faces, so these sectors take the sample's full light and
// the rest none. In the slice the front faces the half circle of directions w with dot(w, n) < 0, n
// the sample's normal projected into the slice; its edge, perpendicular to n, cuts the receiver's
// hemisphere in two. Sector 0 lies at the hemisphere's edge on the tangent side, so the cut keeps the
// sectors past it when that edge looks onto the sample's back.
uint frontSectors(float3 sampleNormal, float3 planeNormal, float3 projectedNormal, float3 viewDir, float3 tangent, float normalAngle,
    float cosN, float sinN, float invNormalization, float dither)
{
    float3 n = sampleNormal - planeNormal * dot(sampleNormal, planeNormal);
    // A surface edge on to the slice: GT-VBGI keeps all its sectors.
    if (dot(n, n) < 1e-8) return 0xFFFFFFFFu;

    bool keepPast = dot(n, cross(projectedNormal, planeNormal)) >= 0;
    float3 cut = normalize(keepPast ? cross(planeNormal, n) : cross(n, planeNormal));
    float d = dot(cut, tangent) >= 0 ? 1 : -1;
    float position = remapHorizons(clamp(dot(cut, viewDir), -1, 1).xx, d, normalAngle, cosN, sinN, invNormalization).x;
    uint split = uint(clamp(floor(position * Sectors + dither), 0, Sectors));
    // Shifts by 32 are shifts by 0 in HLSL, hence the tests for the ends.
    uint before = split > 0 ? 0xFFFFFFFFu >> (Sectors - split) : 0;

    return keepPast ? ~before : before;
}
#endif

[numthreads(8, 8, 1)]
void main(uint2 marchPixel : SV_DispatchThreadID)
{
    uint2 pixel = marchPixel * Scale;
    if (any(pixel >= Size)) return;

    int2 p = pixel;
    int step = Scale;
    float distance = distanceAt(p);
    OutEdges[marchPixel] = packEdges(calculateEdges(distance, distanceAt(p + int2(-step, 0)), distanceAt(p + int2(step, 0)),
        distanceAt(p + int2(0, -step)), distanceAt(p + int2(0, step))));

    // The sky, and a pass whose constants are not the main view's, stay unoccluded and unlit.
    if (Depth[pixel] <= 0 || any(Scene[7] != float4(0, 0, 1, 0)))
    {
#if LIGHT
        OutLight[marchPixel] = 0;
#else
        OutVisibility[marchPixel] = 1;
#endif
        return;
    }

    float2 centre = pixel + 0.5;
    float3 position = viewPosition(centre, distance);
    float3 normal = viewNormal(pixel);
    float3 viewDir = -position * rsqrt(dot(position, position));

    // The radius projected at this distance; each step moves at least a pixel.
    float radiusPixels = min(Radius * 0.5 * Size.y / (Scene[21].y * distance), MaxRadiusPixels);
    float marchPixels = max(radiusPixels, StepCount) / (StepCount + 1) * max(1, StepCount - 1);
    // Occluders are taken as this deep behind the depth buffer, more for distant ones (three-gtvbao's
    // linear thickness), never past the radius. Foliage is thin: as deep as the rest, a clump of
    // blades reads as a solid block and blacks out the ground and the blades behind it.
    float thickness = clamp(0.12 * distance, 0.05, Radius);
    float foliageThickness = clamp(0.02 * distance, 0.02, Radius);
    float2 noise = spatialNoise(marchPixel);

    // GT-VBAO spreads the slices evenly around the view vector, not in the image plane, and projects
    // them; GT-VBGI spreads them by their weight (sampleSliceDirection).
    float a = 1 / (1 + viewDir.z);
    float b = -viewDir.x * viewDir.y * a;
    float3 frameTangent = float3(1 - viewDir.x * viewDir.x * a, b, -viewDir.x);
    float3 frameBitangent = float3(b, 1 - viewDir.y * viewDir.y * a, -viewDir.y);
#if LIGHT
    float2 frameNormal = float2(dot(normal, frameTangent), dot(normal, frameBitangent));
#endif

    float occlusion = 0, weightSum = 0;
    float3 light = 0;
    [unroll]
    for (uint slice = 0; slice < SliceCount; ++slice)
    {
#if LIGHT
        float2 direction = sampleSliceDirection(frameNormal, (slice + noise.x) / SliceCount);
        float3 frameDirection = frameTangent * direction.x + frameBitangent * direction.y;
#else
        float angle = (slice + noise.x) * Pi / SliceCount;
        float3 frameDirection = frameTangent * cos(angle) + frameBitangent * sin(angle);
#endif
        float3 sliceDir = float3(normalize(frameDirection.xy * viewDir.z - viewDir.xy * frameDirection.z), 0);

        float3 planeNormal = normalize(cross(sliceDir, viewDir));
        float3 tangent = cross(viewDir, planeNormal);
        float3 projectedNormal = normal - planeNormal * dot(normal, planeNormal);
        float projectedLength = length(projectedNormal);
        bool degenerate = projectedLength < 1e-4;
        float cosN = clamp(dot(projectedNormal / max(projectedLength, 1e-4), viewDir), -1, 1);
        float normalSign = -sign(dot(projectedNormal, tangent));
        float normalAngle = degenerate ? 0 : normalSign * acos(cosN);
        float sinN = degenerate ? 0 : normalSign * sqrt(1 - cosN * cosN);
        cosN = degenerate ? 1 : cosN;
        float invNormalization = 0.25 / max(cosN + normalAngle * sinN, 1e-4);
        // Shifts both edges of every sample's sector range alike, so the occluded width stays unbiased.
        float dither = interleavedGradientNoise(marchPixel + 0.5 + normalAngle);

        uint occluded = 0;
        float3 sliceLight = 0;
        [unroll]
        for (uint side = 0; side < 2; ++side)
        {
            float d = side == 0 ? 1 : -1;
            float2 pixelDirection = sliceDir.xy * float2(1, -1) * d;
            [loop]
            for (uint i = 0; i < StepCount; ++i)
            {
                float t = (i + noise.y) / max(1, StepCount - 1);
                float offset = max(t * t * marchPixels, i + 1);
                float2 samplePixel = centre + pixelDirection * offset;
                if (any(samplePixel <= 0) || any(samplePixel >= float2(Size))) break;

                int2 texel = int2(samplePixel);
                float signedDistance = signedDistanceAt(texel, offset);
                bool foliage = signedDistance < 0;
                float3 toSample = viewPosition(texel + 0.5, abs(signedDistance)) - position;
                float lengthSq = dot(toSample, toSample);
                float invLength = rsqrt(max(lengthSq, 1e-8));
                float sampleDistance = lengthSq * invLength;
                // Beyond the radius, a sample does not occlude: steps forced to a pixel on distant
                // surfaces, and foreground objects far in front.
                if (sampleDistance > Radius) continue;

                float projected = dot(toSample, viewDir);
                float sampleThickness = min(foliage ? foliageThickness : thickness, sampleDistance * 0.8);
                float front = projected * invLength;
                float back = (projected - sampleThickness)
                    * rsqrt(max(lengthSq - 2 * sampleThickness * projected + sampleThickness * sampleThickness, 1e-8));
                float2 horizons = remapHorizons(clamp(float2(front, back), -1, 1), d, normalAngle, cosN, sinN, invNormalization);
                uint start = uint(clamp(floor(min(horizons.x, horizons.y) * Sectors + dither), 0, Sectors));
                uint end = uint(clamp(floor(max(horizons.x, horizons.y) * Sectors + dither), 0, Sectors));
                uint width = end - start;
                // A shift by 32 is a shift by 0 in HLSL, hence the test for an empty range.
                uint covered = width > 0 ? (0xFFFFFFFFu >> (Sectors - width)) << start : 0;
#if LIGHT
                uint lit = covered & ~occluded;
                if (lit != 0)
                    lit &= frontSectors(viewNormal(texel), planeNormal, projectedNormal, viewDir, tangent, normalAngle, cosN, sinN,
                        invNormalization, dither);
                if (lit != 0) sliceLight += countbits(lit) * Diffuse[texel].rgb * Albedo[texel].rgb;
#endif
                occluded |= covered;
                if (occluded == 0xFFFFFFFFu) break;
            }
        }

        // The slice's cosine-weighted integral weighs it; a degenerate slice drops out, and so does one
        // whose normal faces away from the camera, as normal maps make some do on surfaces seen edge
        // on: its negative weight could cancel the other slice's, and the light divided by the near
        // zero sum would flare into white specks. GT-VBGI drew the slice by that integral, so it weighs 1.
#if LIGHT
        float sliceWeight = !degenerate && cosN + normalAngle * sinN > 0 ? 1 : 0;
#else
        float sliceWeight = max(projectedLength * (cosN + normalAngle * sinN), 0);
#endif
        occlusion += countbits(occluded) / float(Sectors) * sliceWeight;
        light += sliceLight / Sectors * sliceWeight;
        weightSum += sliceWeight;
    }

#if LIGHT
    OutLight[marchPixel] = light / max(weightSum, 1e-4);
#else
    float visibility = 1 - saturate(occlusion / max(weightSum, 1e-4));
    OutVisibility[marchPixel] = OcclusionStrength > 0 ? pow(visibility, OcclusionStrength) : 1;
#endif
}
)";

// XeGTAO's edge-aware 3 x 3 denoiser, one pixel per thread at the march's size, on the visibility
// (LIGHT 0) or the light (LIGHT 1). The passes before the last weigh the centre less (beta / 5), so
// they blur more, as XeGTAO's do.
inline constexpr char Denoise[] = R"(
#if LIGHT
#define Value float3
#define Stored float3
#else
#define Value float
#define Stored unorm float
#endif

Texture2D<Value> Source : register(t0);
Texture2D<float> Edges : register(t1);
RWTexture2D<Stored> Output : register(u0);

static const float BlurAmount = FINAL ? 1.2 : 1.2 / 5;
static const float DiagonalWeight = 0.85 * 0.5;

float4 unpackEdges(float packed)
{
    uint value = uint(packed * 255.5);
    return saturate(float4((value >> 6) & 3, (value >> 4) & 3, (value >> 2) & 3, value & 3) / 3.0);
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    uint2 size;
    Source.GetDimensions(size.x, size.y);
    if (any(pixel >= size)) return;

    int2 p = pixel, last = int2(size) - 1;
    int2 left = clamp(p + int2(-1, 0), 0, last), right = clamp(p + int2(1, 0), 0, last);
    int2 top = clamp(p + int2(0, -1), 0, last), bottom = clamp(p + int2(0, 1), 0, last);
    float4 edgesL = unpackEdges(Edges[left]), edgesR = unpackEdges(Edges[right]);
    float4 edgesT = unpackEdges(Edges[top]), edgesB = unpackEdges(Edges[bottom]);

    // Both sides of an edge must agree, then a little leaks through where three or four edges meet,
    // which keeps thin features from aliasing.
    float4 edges = unpackEdges(Edges[p]) * float4(edgesL.y, edgesR.x, edgesT.w, edgesB.z);
    float edginess = saturate(4 - 2.5 - dot(edges, 1)) / (4 - 2.5) * 0.5;
    edges = saturate(edges + edginess);

    float weightTL = DiagonalWeight * (edges.x * edgesL.z + edges.z * edgesT.x);
    float weightTR = DiagonalWeight * (edges.z * edgesT.y + edges.y * edgesR.z);
    float weightBL = DiagonalWeight * (edges.w * edgesB.x + edges.x * edgesL.w);
    float weightBR = DiagonalWeight * (edges.y * edgesR.w + edges.w * edgesB.y);

    Value sum = Source[p] * BlurAmount;
    float weight = BlurAmount;
    sum += Source[left] * edges.x + Source[right] * edges.y + Source[top] * edges.z + Source[bottom] * edges.w;
    weight += dot(edges, 1);
    sum += Source[clamp(p + int2(-1, -1), 0, last)] * weightTL + Source[clamp(p + int2(1, -1), 0, last)] * weightTR;
    sum += Source[clamp(p + int2(-1, 1), 0, last)] * weightBL + Source[clamp(p + int2(1, 1), 0, last)] * weightBR;
    weight += weightTL + weightTR + weightBL + weightBR;

    Output[pixel] = sum / weight;
}
)";

// GT-VBGI's light, denoised at half the render size, brought to the render size and added to the
// game's diffuse light, for the resolve to read in its place. Each pixel blends the four nearest
// half-size texels bilinearly, each also weighed by how close its depth is (joint bilateral
// upsampling), so the light of one surface stays off its neighbour across an edge.
inline constexpr char Upsample[] = PHANTOM_FX_PARAMETERS R"(
cbuffer Scene : register(b1)
{
    float4 Scene[30];
};

Texture2D<float3> Light : register(t0);   // Texel h stands for pixel 2h (shaders::March's Scale)
Texture2D<float> Depth : register(t1);    // Reversed-Z, 0 on the sky
Texture2D<float4> Diffuse : register(t2); // The game's diffuse light, as the resolve reads it
RWTexture2D<float4> OutDiffuse : register(u0);

float distanceOf(float depth)
{
    return Scene[21].z / (depth - Scene[21].w);
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;

    float depth = Depth[pixel];
    float3 light = 0;
    if (depth > 0)
    {
        float distance = distanceOf(depth);
        int2 base = pixel / 2, last = (Size - 1) / 2;
        // A pixel on a texel's own spot takes that texel alone; one between two takes half of each.
        float2 between = (pixel % 2) * 0.5;
        float3 sum = 0;
        float weightSum = 0;
        [unroll]
        for (uint i = 0; i < 4; ++i)
        {
            int2 offset = int2(i % 2, i / 2);
            int2 texel = min(base + offset, last);
            float2 bilinear = offset ? between : 1 - between;
            // The relative depth difference; texels on the sky have an infinite one and drop out.
            float gap = abs(distanceOf(Depth[texel * 2]) - distance) / distance;
            float weight = bilinear.x * bilinear.y / (gap + 1e-3);
            sum += Light[texel] * weight;
            weightSum += weight;
        }

        light = sum / max(weightSum, 1e-8);
    }

    float4 diffuse = Diffuse[pixel];
    OutDiffuse[pixel] = float4(diffuse.rgb * DiffuseScale + light * LightStrength, diffuse.a);
}
)";

// The global illumination's light in place of the game's probe light in its diffuse light. The
// game's light target keeps the probe light, as its exposure measures that target; the upscale
// draws the probe light aside as well, for this to take it out.
inline constexpr char AddLight[] = PHANTOM_FX_PARAMETERS R"(
Texture2D<float4> Diffuse : register(t0); // The game's diffuse light, as the resolve reads it
Texture2D<float4> Light : register(t1);   // The traced light, in the same units
Texture2D<float4> Ambient : register(t2); // The game's probe light, which the upscale drew aside
RWTexture2D<float4> OutDiffuse : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;

    float4 diffuse = Diffuse[pixel];
    float3 direct = max(diffuse.rgb - Ambient[pixel].rgb, 0.0f);
    OutDiffuse[pixel] = float4(direct * DiffuseScale + Light[pixel].rgb * LightStrength, diffuse.a);
}
)";

// Drawn with the game's upscale vertex shader right after it, with a multiplying blend into the
// diffuse light: the input signature is that vertex shader's output.
inline constexpr char Apply[] = R"(
Texture2D<float> Visibility : register(t0);

float4 main(float4 texcoord0 : TEXCOORD0, float4 texcoord1 : TEXCOORD1, float4 position : SV_Position) : SV_Target
{
    return Visibility[uint2(position.xy)];
}
)";

// Diagnostics: drawn in place of the deferred resolve, GTVBAO's occlusion (LIGHT 0) or GT-VBGI's light
// (LIGHT 1) alone. Every resolve vertex shader outputs the position first. The resolve writes through
// an sRGB view: the visibility is linearised first and stored as it is, rather than brightened by
// the encoding; GT-VBGI's light, which the last denoise pass writes without the game's diffuse light,
// is linear already.
inline constexpr char Show[] = R"(
#if LIGHT
Texture2D<float4> Light : register(t9);
#else
Texture2D<float> Visibility : register(t13);
#endif

float4 main(float4 position : SV_Position) : SV_Target
{
#if LIGHT
    float3 colour = Light[uint2(position.xy)].rgb;
#else
    float3 colour = pow(saturate(Visibility[uint2(position.xy)]), 2.2).xxx;
#endif
    // Alpha as the resolve writes it: a luminance code that exposure, bloom and depth of field read.
    // With 1 they took every pixel for a bright light and blurred the whole view.
    float luminance = dot(colour, float3(0.2125, 0.7154, 0.0721)) / 32;
    return float4(colour, luminance * rsqrt(max(luminance, 1.0 / 512)));
}
)";

#undef PHANTOM_FX_PARAMETERS
}
