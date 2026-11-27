// The occlusion is ported from three-gtvbao (norio, MIT; GT-VBAO by Mirko Salm, CC0 / MIT; SSRT3 by
// Olivier Therrien, MIT), the noise from XeGTAO (Intel, MIT). Their licenses are in licenses/.
// The visibility bitmask march, once per pixel at render resolution: for each slice, 32 sectors of
// the hemisphere around the view vector, set where a depth sample, taken as `thickness` deep,
// covers them. The horizons go through the cosine-weighted measure's CDF first (GT-VBAO), so the
// share of set sectors is the cosine-weighted occlusion. Also writes XeGTAO's depth edges for the
// denoiser.
//
// Fox Engine's view space looks down +z and mirrors x (P00 < 0). Flipping z, and x when mirrored,
// turns it into the usual view space looking down -z with x right and y up, which the maths below
// assumes; positions and the G-buffer's view-space normals take the same flip.
#include "../../shaders/visibility_march.hlsli"

// The game's cPSScene as the lighting passes bind it (docs/DEVELOPMENT.md, "Constant buffers"):
// c7 = (0, 0, 1, 0) for the main view's perspective, c21 = (1 / P00, 1 / P11, B, A), c28 the
// jitter in NDC, which the reconstruction takes off as the engine's own passes do.
cbuffer Scene : register(b1)
{
    float4 Scene[30];
};

Texture2D<float> Depth : register(t0);    // Reversed-Z, 0 on the sky
Texture2D<float4> Normals : register(t1); // G-buffer view-space normals
RWTexture2D<unorm float> OutVisibility : register(u0);
// shaders::PrefilterDepths' distances and their smaller copies, negative on foliage.
Texture2D<float> Distances : register(t2);
RWTexture2D<unorm float> OutEdges : register(u1);

static const float Pi = 3.14159265;
static const uint Sectors = 32;
static const uint SliceCount = 2;
static const uint StepCount = 8;
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

[numthreads(8, 8, 1)]
void main(uint2 marchPixel : SV_DispatchThreadID)
{
    const uint2 pixel = marchPixel;
    if (any(pixel >= Size)) return;

    const int2 p = int2(pixel);
    float distance = distanceAt(p);
    OutEdges[marchPixel] = packEdges(calculateEdges(distance, distanceAt(p + int2(-1, 0)), distanceAt(p + int2(1, 0)),
        distanceAt(p + int2(0, -1)), distanceAt(p + int2(0, 1))));

    // The sky, and a pass whose constants are not the main view's, stay unoccluded.
    if (Depth[pixel] <= 0 || any(Scene[7] != float4(0, 0, 1, 0)))
    {
        OutVisibility[marchPixel] = 1;
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

    // GT-VBAO spreads the slices evenly around the view vector, not in the image plane, and projects them.
    float a = 1 / (1 + viewDir.z);
    float b = -viewDir.x * viewDir.y * a;
    float3 frameTangent = float3(1 - viewDir.x * viewDir.x * a, b, -viewDir.x);
    float3 frameBitangent = float3(b, 1 - viewDir.y * viewDir.y * a, -viewDir.y);

    float occlusion = 0, weightSum = 0;
    [unroll]
    for (uint slice = 0; slice < SliceCount; ++slice)
    {
        float angle = (slice + noise.x) * Pi / SliceCount;
        float3 frameDirection = frameTangent * cos(angle) + frameBitangent * sin(angle);
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
                occluded |= covered;
                if (occluded == 0xFFFFFFFFu) break;
            }
        }

        // The slice's cosine-weighted integral weighs it; a degenerate slice drops out, and so does one
        // whose normal faces away from the camera, as normal maps make some do on surfaces seen edge
        // on: its negative weight could cancel the other slice's, and the visibility divided by the near
        // zero sum would flare into white specks.
        float sliceWeight = max(projectedLength * (cosN + normalAngle * sinN), 0);
        occlusion += countbits(occluded) / float(Sectors) * sliceWeight;
        weightSum += sliceWeight;
    }

    float visibility = 1 - saturate(occlusion / max(weightSum, 1e-4));
    OutVisibility[marchPixel] = OcclusionStrength > 0 ? pow(visibility, OcclusionStrength) : 1;
}
