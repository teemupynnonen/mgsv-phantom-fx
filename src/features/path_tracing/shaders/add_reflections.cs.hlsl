// The traced reflections added to the game's specular light, as the resolve reads it, times the menu's strength.
cbuffer Parameters : register(b0)
{
    uint2 Size;
    float Strength;
    float Padding;
};

Texture2D<float4> Specular : register(t0);   // The game's specular light
Texture2D<float4> Reflection : register(t1); // The traced reflections, in the same units (ray_tracing::reflection)
RWTexture2D<float4> OutSpecular : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;

    float4 specular = Specular[pixel];
    OutSpecular[pixel] = float4(specular.rgb + Reflection[pixel].rgb * Strength, specular.a);
}
