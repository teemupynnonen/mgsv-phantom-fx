// Diagnostics: the ray-traced scene alone (the global illumination's scene view), in place of the
// game's resolve. That resolve would multiply it by the G-buffer's albedo and add the game's
// specular, so the picture was the game's surfaces with the scene's colours over them. The scene
// view's colours are display values, and the resolve writes through an sRGB view: they are
// linearised first, as the occlusion's visibility is, so the picture is the one the scene wrote.
Texture2D<float4> Scene : register(t9);

float4 main(float4 position : SV_Position) : SV_Target
{
    float3 colour = pow(saturate(Scene[uint2(position.xy)].rgb), 2.2);
    float luminance = dot(colour, float3(0.2125, 0.7154, 0.0721)) / 32;
    return float4(colour, luminance * rsqrt(max(luminance, 1.0 / 512)));
}
