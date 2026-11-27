// Drawn with the game's upscale vertex shader right after it, with a multiplying blend into the
// diffuse light: the input signature is that vertex shader's output.
Texture2D<float> Visibility : register(t0);

float4 main(float4 texcoord0 : TEXCOORD0, float4 texcoord1 : TEXCOORD1, float4 position : SV_Position) : SV_Target
{
    return Visibility[uint2(position.xy)];
}
