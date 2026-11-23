// Packs what the kernels read from the G-buffer: the depth, the normals in world space (0 where
// the sky is; alpha 1/3 on foliage, the specular target's material 43, else 1) and the albedo, linear
// (alpha 0 where the add-on's sea drew, else 1: IsSeaPixel in gi_common.hlsli).
Texture2D<float> g_Depth : register(t0);
Texture2D<float4> g_Normal : register(t1);
Texture2D<float4> g_Albedo : register(t2);
Texture2D<float4> g_Specular : register(t3);
Texture2D<float> g_SeaDepth : register(t4); // The sea's depth where it drew (sea::surfaceDepth)
RWTexture2D<float> g_DepthOut : register(u0);
RWTexture2D<float4> g_NormalOut : register(u1);
RWTexture2D<float4> g_AlbedoOut : register(u2);

cbuffer Parameters : register(b0)
{
    float4 g_View[3]; // World to view, rows
    uint2 g_Size;
    uint g_HasAlbedo;
    uint g_HasSpecular;
    uint g_HasSea;
};

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= g_Size)) return;

    float depth = g_Depth.Load(int3(id, 0));
    g_DepthOut[id] = depth;

    // DecodeViewSpaceNormal: z = 2 b^2 - 1, xy the direction of 2 rg - 1 at length sqrt(1 - z^2).
    float3 encoded = g_Normal.Load(int3(id, 0)).xyz;
    float2 xy = encoded.xy * 2 - 1;
    float z = encoded.z * encoded.z * 2 - 1;
    float rest = 1 - z * z;
    float3 view = float3(rsqrt(dot(xy, xy) * rest + 1e-7) * (rest * xy), z);
    float3 world = normalize(view.x * g_View[0].xyz + view.y * g_View[1].xyz + view.z * g_View[2].xyz);
    bool foliage = g_HasSpecular && round(g_Specular.Load(int3(id, 0)).z * 255) == 43;
    g_NormalOut[id] = depth > 0 ? float4(0.5 * world + 0.5, foliage ? 1.0 / 3 : 1) : 0;

    bool isSea = g_HasSea && depth > 0 && g_SeaDepth.Load(int3(id, 0)) == depth;
    g_AlbedoOut[id] = float4(g_HasAlbedo ? g_Albedo.Load(int3(id, 0)).rgb : 0.25, isSea ? 0 : 1);
}
