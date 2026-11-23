// What the surfaces reflect added over the scene, once the game drew them all, as they would have blended it: the scene holds
// sRGB-encoded values here, past 1 too, and the game's glass adds its encoded colour by its alpha.
cbuffer Parameters : register(b0)
{
    float Strength; // The menu's
    float3 Padding;
};

Texture2D<float4> Scene : register(t0);
Texture2D<float4> Weight : register(t1);        // The surfaces' weights
Texture2D<float4> Reflection : register(t2);    // What they reflect, in the scene's units
Texture2D<float4> SurfaceNormal : register(t3); // Water's w is minus its share

float4 main(float4 position : SV_Position) : SV_Target
{
    const int3 pixel = int3(position.xy, 0);
    const float waterShare = -SurfaceNormal.Load(pixel).w;
    const float4 weight = waterShare > 0 ? float4(waterShare.xxx, 1) : Weight.Load(pixel);
    if (!(weight.a > 0)) discard;

    const float3 reflected = Reflection.Load(pixel).rgb * Strength * weight.rgb / weight.a;
    const float3 encoded = reflected <= 0.003131 ? reflected * 12.92 : 1.055 * pow(max(reflected, 0.00001), 1 / 2.4) - 0.055;
    return float4(Scene.Load(pixel).rgb + weight.a * encoded, 1);
}
