// DLSS Ray Reconstruction's guides from the G-buffer, as its integration guide asks for them: the
// world-space normal with the roughness in w, the diffuse albedo, and the specular albedo, the share of
// light the surface reflects towards the eye. That share is the game's for its sky reflection
// (SKY_REFLECTION in sun_light_pass_shaders.hpp): its specular colour (the material table's, times the
// specular target's intensity) through its Fresnel table at the eye's angle, from at least the sun pass's
// least F0 (cPSLight c2.z), times half the smoothness. The game's materials are tuned to it: with a
// physical one (NVIDIA's EnvBRDFApprox2), Snake's red arm, an 11% white specular of roughness 0.59,
// reflected 3 to 4 times as much and the sky's light turned it grey. The sea (sea.hpp) is not one of the
// game's materials: it reflects as water does, all of its light at grazing angles. Where the sky is: no
// normal and roughness, albedo 0.5, no specular albedo. The specular albedo's alpha is 0 on the sea, which the game's
// reflection blend leaves to its own reflectance (reflections.hlsl), and on the sky.
Texture2D<float> g_Depth : register(t0);
Texture2D<float4> g_Normal : register(t1);
Texture2D<float4> g_Albedo : register(t2);    // Through an sRGB view: linear
Texture2D<float4> g_Specular : register(t3);  // Roughness, intensity, material, translucency
Texture2D<float4> g_Fresnel : register(t4);   // g_lut1: u the cosine, v F0
Texture2D<float4> g_Materials : register(t5); // g_MaterialTexture: u the material, row 0.25 its colour, 0.75 its F0
Texture2D<float> g_SeaDepth : register(t6);   // The sea's depth where it drew (sea::surfaceDepth)
Texture3D<float4> g_SpecularLobe : register(t7); // g_lut2: x the halfway vector's angle, y the roughness, z the anisotropy
RWTexture2D<float4> g_NormalRoughness : register(u0);
RWTexture2D<float4> g_DiffuseAlbedo : register(u1);
RWTexture2D<float4> g_SpecularAlbedo : register(u2);
RWTexture2D<float4> g_Material : register(u3); // Path tracing's: rgb the specular colour; a the translucency
RWTexture2D<float4> g_Lobe : register(u4);     // x the light through the leaves' floor, the translucency times the material's; y F0; z the anisotropy
RWTexture2D<float4> g_MaterialSpecular : register(u5); // Each material's specular colour at normal incidence, for the rays' hits (HitMaterialOf)
RWTexture2D<float4> g_SpecularTable : register(u6); // g_lut2's slices one below the other, which Direct3D 12's lamps read (direct_light.hlsl)

cbuffer Parameters : register(b0)
{
    float4 g_View[3];          // World to view, rows
    float4 g_ProjectionParam;  // cPSScene c21: 1 / P00, 1 / P11, B, A
    float2 g_Jitter;           // cPSScene c28.xy, in NDC
    uint2 g_Size;
    uint g_HasAlbedo;
    uint g_HasSpecular;
    uint g_HasTables;
    float g_SkyReflectionFloor; // The sun pass's cPSLight c2.z, the frame before
    uint g_HasSea;
};

// The texel a point sampler takes at (u, v), as the sun pass reads the material table.
float4 PointAt(Texture2D<float4> table, float u, float v)
{
    uint width, height;
    table.GetDimensions(width, height);
    return table.Load(int3(min(uint2(float2(u, v) * float2(width, height)), uint2(width, height) - 1), 0));
}

// The Fresnel table at (cosine, F0), as a linear clamped sampler takes it.
float FresnelAt(float cosine, float F0)
{
    uint width, height;
    g_Fresnel.GetDimensions(width, height);
    float2 texel = clamp(float2(cosine * width, F0 * height) - 0.5, 0, float2(width, height) - 1);
    uint2 low = uint2(texel);
    uint2 high = min(low + 1, uint2(width, height) - 1);
    float2 t = frac(texel);
    float above = lerp(g_Fresnel.Load(int3(low.x, low.y, 0)).x, g_Fresnel.Load(int3(high.x, low.y, 0)).x, t.x);
    float below = lerp(g_Fresnel.Load(int3(low.x, high.y, 0)).x, g_Fresnel.Load(int3(high.x, high.y, 0)).x, t.x);
    return lerp(above, below, t.y);
}

// Water's reflectance (F0 0.02) at the eye's angle, over GGX's lobe: Karis' fit of the split sum ("Physically
// Based Shading on Mobile", 2014).
float WaterReflectance(float toEyeN, float roughness)
{
    const float4 c0 = float4(-1, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * toEyeN)) * r.x + r.y;
    float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return 0.02 * ab.x + ab.y;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= g_Size)) return;

    if (g_HasTables && id.y == 0 && id.x < 256)
        g_MaterialSpecular[uint2(id.x, 0)] = float4(g_Materials.Load(int3(id.x, 0, 0)).rgb * FresnelAt(1, g_Materials.Load(int3(id.x, 1, 0)).x), 1);
    if (g_HasTables && id.x < 64 && id.y < 256) g_SpecularTable[id] = g_SpecularLobe.Load(int4(id.x, id.y % 16, id.y / 16, 0));

    float depth = g_Depth.Load(int3(id, 0));
    if (depth <= 0)
    {
        g_NormalRoughness[id] = 0;
        g_DiffuseAlbedo[id] = float4(0.5, 0.5, 0.5, 1);
        g_SpecularAlbedo[id] = 0;
        g_Material[id] = 0;
        g_Lobe[id] = 0;
        return;
    }

    // DecodeViewSpaceNormal, as the pack above.
    float3 encoded = g_Normal.Load(int3(id, 0)).xyz;
    float2 xy = encoded.xy * 2 - 1;
    float z = encoded.z * encoded.z * 2 - 1;
    float rest = 1 - z * z;
    float3 normal = float3(rsqrt(dot(xy, xy) * rest + 1e-7) * (rest * xy), z);
    float3 world = normalize(normal.x * g_View[0].xyz + normal.y * g_View[1].xyz + normal.z * g_View[2].xyz);

    // The eye's direction to the pixel in view space, as the sun pass works it out.
    float2 ndc = float2((id.x + 0.5) / g_Size.x * 2 - 1, 1 - (id.y + 0.5) / g_Size.y * 2) - g_Jitter;
    float viewZ = g_ProjectionParam.z / (depth - g_ProjectionParam.w);
    float3 eye = normalize(float3(ndc * g_ProjectionParam.xy * viewZ, viewZ));

    float4 specular = g_HasSpecular ? g_Specular.Load(int3(id, 0)) : float4(1, 0, 0, 0);
    float roughness = specular.x;
    float toEyeN = saturate(-dot(normal, eye));
    float3 specularColor = specular.y;
    float3 reflected = specular.y * max(0.04, g_SkyReflectionFloor);
    float F0 = 0.04, anisotropy = 0;
    float translucencyScale = 0; // The material's (its table's row 0.25, w)
    if (g_HasTables)
    {
        float4 color = PointAt(g_Materials, specular.z, 0.25);
        float4 row = PointAt(g_Materials, specular.z, 0.75);
        F0 = row.x;
        anisotropy = row.w;
        specularColor = specular.y * color.rgb;
        reflected = specularColor * FresnelAt(toEyeN, max(F0, g_SkyReflectionFloor));
        translucencyScale = color.w;
    }

    g_NormalRoughness[id] = float4(world, roughness);
    g_DiffuseAlbedo[id] = float4(g_HasAlbedo ? g_Albedo.Load(int3(id, 0)).rgb : 0.5, 1);
    bool isSea = g_HasSea && g_SeaDepth.Load(int3(id, 0)) == depth;
    g_SpecularAlbedo[id] = float4(isSea ? WaterReflectance(toEyeN, roughness) : saturate(reflected * ((1 - roughness) * 0.5)), isSea ? 0 : 1);
    g_Material[id] = float4(saturate(specularColor), specular.w);
    g_Lobe[id] = float4(saturate(specular.w * translucencyScale), F0, anisotropy, 0);
}
