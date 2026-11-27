// The gather: visibility-bitmask GI (Therrien et al.). A few slices through each pixel's view
// vector are marched over the distance mips (or the depth, without them); each slice keeps a
// 32-sector occlusion bitmask of the hemisphere, every sample covering the sectors its surface
// spans at the thickness given, and lights only the sectors it uncovers first, with the radiance
// it reflects (albedo x diffuse light + specular), the sectors it spans being its cosine. Equal
// sectors carry equal cosine weight (GT-VBAO's horizon remap), so the sum is the light the
// hemisphere receives over pi, in the units of the game's diffuse light. Alpha is validity. It
// reads the game's diffuse light and writes its own target, so reading neighbours while writing
// this pixel is no hazard.
#include "ssgi_common.hlsli"
Texture2D<float> Depth : register(t0);        // Reversed-Z, 0 on the sky
Texture2D<float4> Normals : register(t1);     // The G-buffer's view-space normals
Texture2D<float4> LightSource : register(t2); // The game's diffuse light, as the resolve reads it
Texture2D<float4> Albedo : register(t3);      // Through the resolve's sRGB view, so linear
Texture2D<float4> Specular : register(t4);    // The specular light the resolve adds (its inLightSpecular)
// The prefiltered distances and their smaller copies (visibility_bitmask's prefilter), negative on
// foliage; the march reads them when UseMips is set.
Texture2D<float> Distances : register(t5);
RWTexture2D<float4> Bounce : register(u0);

static const float Pi = 3.14159265;
static const uint Sectors = 32;
static const uint SliceLimit = 4;
static const uint StepLimit = 8;
// XeGTAO's DepthMIPSamplingOffset: a sample 14 pixels or more away reads the first copy, and each
// doubling of the distance the next.
static const uint DistanceMipCount = 5;
static const float DistanceMipOffset = 3.3;
static const float MinRadiusPixels = 6.0;  // The reach on screen never falls under this
static const float MaxRadiusShare = 0.3;   // ...or over this share of the height

// The distance at a sample `offset` pixels from the receiver, from a smaller copy the farther it
// lies; negative on foliage. Without the mips, the depth at full size and no foliage.
float signedDistanceAt(int2 texel, float offset)
{
    const uint mip = UseMips < 0.5 ? 0 : uint(round(clamp(log2(max(offset, 1.0)) - DistanceMipOffset, 0, DistanceMipCount - 1)));
    const uint2 mipSize = max(Size >> mip, 1);
    const float mipped = Distances.Load(int3(min(uint2(texel) >> mip, mipSize - 1), mip));
    return UseMips < 0.5 ? distanceOf(Depth.Load(int3(texel, 0))) : mipped;
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
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint2 pixel = tid.xy;
    const float scale = max(Scale, 1.0);
    const int2 gs = (int2(Size) + (int)scale - 1) / (int)scale;
    if (any(pixel >= (uint2)gs)) return;

    // The gather may run below full resolution; its threads map onto full-resolution pixels, which
    // is where every G-buffer read and every sample offset lives. Each frame a different pixel of
    // the pixel's block stands for it (every cell once per block, in a per-pixel order), so the
    // accumulation converges on the full-resolution result rather than on one fixed subsample.
    // 65536 is a multiple of every block's cell count, so the order stays even when the index wraps.
    const uint frame = (uint)Random;
    const uint blockSide = (uint)scale;
    const uint cells = blockSide * blockSide;
    const uint cell = (frame * 7u + (noiseBits(pixel, 0, 2) >> 24)) % cells;
    const int2 centre = min((int2)(pixel * blockSide + uint2(cell % blockSide, cell / blockSide)), int2(Size) - 1);

    const float z = distanceOf(Depth.Load(int3(centre, 0)));
    if (!hasGeometry(z))
    {
        Bounce[pixel] = 0;
        return;
    }

    const float2 centreF = float2(centre) + 0.5;
    const float3 P = viewPosition(centreF, z);
    const float3 N = decodeNormal(Normals.Load(int3(centre, 0)).xyz);
    const float3 viewDir = -P * rsqrt(dot(P, P));

    // The radius is fixed in metres and projected to pixels from this pixel's depth, so the
    // reach is the same distance everywhere instead of growing with it. The pixel clamp keeps far
    // pixels from collapsing onto themselves and near ones from costing the whole screen; the world
    // radius follows the clamped reach so the thickness and the falloff agree with the taps.
    const float metresPerPixel = 2.0 * z * Scene[21].y / Size.y;
    const float radiusPixels = clamp(max(Radius, 0.01) / metresPerPixel, MinRadiusPixels, MaxRadiusShare * Size.y);
    const float radiusWorld = radiusPixels * metresPerPixel;

    // Samples sets the slices, Visibility the steps along each side of one. Each step moves at least
    // a pixel; the spacing grows quadratically, so the near field is dense.
    const uint sliceCount = (uint)clamp(round(Samples * 0.25), 1.0, (float)SliceLimit);
    const uint stepCount = (uint)clamp(Visibility, 2.0, (float)StepLimit);
    const float marchPixels = max(radiusPixels, (float)stepCount) / (stepCount + 1) * max(1, stepCount - 1);

    // Occluders are taken as this deep behind the depth buffer, more for distant ones, never past the
    // radius. Foliage is thin: a clump of blades read as a solid block would black out what lies
    // behind it, and its light with it.
    const float thickness = clamp(Thickness * z, 0.05, radiusWorld);
    const float foliageThickness = clamp(Thickness * 0.17 * z, 0.02, radiusWorld);

    const float sliceNoise = noise01(pixel, frame, 0);
    const float stepNoise = noise01(pixel, frame, 1);
    const float ditherNoise = noise01(pixel, frame, 3);

    // GT-VBAO spreads the slices evenly around the view vector, not in the image plane, and projects them.
    const float a = 1 / (1 + viewDir.z);
    const float b = -viewDir.x * viewDir.y * a;
    const float3 frameTangent = float3(1 - viewDir.x * viewDir.x * a, b, -viewDir.x);
    const float3 frameBitangent = float3(b, 1 - viewDir.y * viewDir.y * a, -viewDir.y);

    float3 light = 0;
    float weightSum = 0;

    [loop]
    for (uint slice = 0; slice < sliceCount; ++slice)
    {
        const float angle = (slice + sliceNoise) * Pi / sliceCount;
        const float3 frameDirection = frameTangent * cos(angle) + frameBitangent * sin(angle);
        const float3 sliceDir = float3(normalize(frameDirection.xy * viewDir.z - viewDir.xy * frameDirection.z), 0);

        const float3 planeNormal = normalize(cross(sliceDir, viewDir));
        const float3 tangent = cross(viewDir, planeNormal);
        const float3 projectedNormal = N - planeNormal * dot(N, planeNormal);
        const float projectedLength = length(projectedNormal);
        const bool degenerate = projectedLength < 1e-4;
        float cosN = clamp(dot(projectedNormal / max(projectedLength, 1e-4), viewDir), -1, 1);
        const float normalSign = -sign(dot(projectedNormal, tangent));
        const float normalAngle = degenerate ? 0 : normalSign * acos(cosN);
        const float sinN = degenerate ? 0 : normalSign * sqrt(1 - cosN * cosN);
        cosN = degenerate ? 1 : cosN;
        const float invNormalization = 0.25 / max(cosN + normalAngle * sinN, 1e-4);
        // Shifts both edges of every sample's sector range alike, so the covered width stays unbiased.
        const float dither = frac(ditherNoise + slice * 0.61803399);

        uint occluded = 0;
        float3 sliceLight = 0;

        [unroll]
        for (uint side = 0; side < 2; ++side)
        {
            const float d = side == 0 ? 1 : -1;
            const float2 pixelDirection = sliceDir.xy * float2(1, -1) * d;
            [loop]
            for (uint i = 0; i < stepCount; ++i)
            {
                const float t = (i + stepNoise) / max(1, stepCount - 1);
                const float offset = max(t * t * marchPixels, i + 1);
                const float2 samplePixel = centreF + pixelDirection * offset;
                if (any(samplePixel <= 0) || any(samplePixel >= float2(Size))) break;

                const int2 texel = int2(samplePixel);
                const float signedDistance = signedDistanceAt(texel, offset);
                const float sampleZ = abs(signedDistance);
                if (!hasGeometry(sampleZ)) continue;

                const float3 toSample = viewPosition(texel + 0.5, sampleZ) - P;
                const float lengthSq = dot(toSample, toSample);
                const float invLength = rsqrt(max(lengthSq, 1e-8));
                const float sampleDistance = lengthSq * invLength;
                // Beyond the radius a sample neither occludes nor lights: steps forced to a pixel on
                // distant surfaces, and foreground objects far in front.
                if (sampleDistance > radiusWorld) continue;

                const float projected = dot(toSample, viewDir);
                const float sampleThickness = min(signedDistance < 0 ? foliageThickness : thickness, sampleDistance * 0.8);
                const float front = projected * invLength;
                const float back = (projected - sampleThickness)
                    * rsqrt(max(lengthSq - 2 * sampleThickness * projected + sampleThickness * sampleThickness, 1e-8));
                const float2 horizons = remapHorizons(clamp(float2(front, back), -1, 1), d, normalAngle, cosN, sinN, invNormalization);
                const uint start = uint(clamp(floor(min(horizons.x, horizons.y) * Sectors + dither), 0, Sectors));
                const uint end = uint(clamp(floor(max(horizons.x, horizons.y) * Sectors + dither), 0, Sectors));
                const uint width = end - start;
                // A shift by 32 is a shift by 0 in HLSL, hence the test for an empty range.
                const uint covered = width > 0 ? (0xFFFFFFFFu >> (Sectors - width)) << start : 0;

                // Only the sectors this sample uncovers first take its light: nearer samples hide
                // what lies behind them. Its surface sends light this way over the sectors it
                // spans; its back sends none.
                const uint lit = covered & ~occluded;
                if (lit != 0)
                {
                    // The sectors already measure how much of the hemisphere the source spans, so its
                    // cosine is only a back-face test: weighting by it too darkened every bounce.
                    const float facing = dot(decodeNormal(Normals.Load(int3(texel, 0)).xyz), -toSample * invLength) > 0 ? 1.0 : 0.0;
                    if (facing > 0)
                    {
                        // A fade over the outer quarter of the radius, so a source does not pop in
                        // at its edge as the camera moves; the occlusion above stays hard.
                        const float fade = saturate(4 - 4 * sampleDistance / radiusWorld);
                        const float3 radiance = Albedo.Load(int3(texel, 0)).rgb * LightSource.Load(int3(texel, 0)).rgb
                                              + Specular.Load(int3(texel, 0)).rgb;
                        sliceLight += countbits(lit) * facing * fade * radiance;
                    }
                }
                occluded |= covered;
                if (occluded == 0xFFFFFFFFu) break;
            }
        }

        // The slice's cosine-weighted integral weighs it; a degenerate slice drops out, and so does one
        // whose normal faces away from the camera, as normal maps make some do on surfaces seen edge
        // on: its negative weight could cancel the other slice's, and the light divided by the near
        // zero sum would flare into white specks.
        const float sliceWeight = max(projectedLength * (cosN + normalAngle * sinN), 0);
        light += sliceLight / Sectors * sliceWeight;
        weightSum += sliceWeight;
    }

    // Zero light with weight is a real result (open surroundings); only a pixel no slice could weigh
    // is invalid, which the denoiser fills.
    const bool valid = weightSum > 1e-4;
    Bounce[pixel] = float4(valid ? light / weightSum : 0.0, valid ? 1.0 : 0.0);
}
