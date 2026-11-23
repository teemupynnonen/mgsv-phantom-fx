// SSILVB's light, denoised at half the render size, brought to the render size and added to the
// game's diffuse light, for the resolve to read in its place. Each pixel blends the four nearest
// half-size texels bilinearly, each also weighed by how close its depth is (joint bilateral
// upsampling), so the light of one surface stays off its neighbour across an edge.
#include "../../../shaders/visibility_march.hlsli"

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
