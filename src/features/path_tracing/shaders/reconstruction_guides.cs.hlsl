// Where a surface dominates, tells Ray Reconstruction of it, as its guide asks of a window: depth, normal and hit distance
// from the one surface. The normal and the hit distance go into its guides; `SurfaceDepth` is left with the depth of those
// pixels alone, for the upscaler's motion (upscaler_shaders.hpp, Inputs). Below this cover what is behind a pane dominates;
// water always does, as it hides or colours what is under it.
Texture2D<float4> SurfaceNormal : register(t0);
Texture2D<float4> SurfaceWeight : register(t1);
Texture2D<float4> SurfaceReflection : register(t2);
RWTexture2D<float> SurfaceDepth : register(u0);
RWTexture2D<float4> Normals : register(u1);
RWTexture2D<float> HitDistance : register(u2);

static const float DominantCover = 0.1;
static const float WaterRoughness = 0.05; // As the trace's (reflections.hlsl)

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const float4 normal = SurfaceNormal[pixel];
    const bool isWater = normal.w < 0;
    if (!isWater && SurfaceWeight[pixel].a < DominantCover)
    {
        SurfaceDepth[pixel] = 0;
        return;
    }

    Normals[pixel] = float4(normal.xyz, isWater ? WaterRoughness : normal.w);
    HitDistance[pixel] = SurfaceReflection[pixel].a;
}
