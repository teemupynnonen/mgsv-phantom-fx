// The motion blur's start at half the output size: DLSS's image while upscaling, or else the scene the
// game halved, halved as the game halves it, with the alpha of the game's own half-size copy, which
// scales how much of the blur shows. The scene at the output size has the composite's alpha, not the
// game's.
Texture2D<float4> Scene : register(t0);
Texture2D<float4> GameCopy : register(t1);
SamplerState Linear : register(s0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return float4(Scene.SampleLevel(Linear, uv, 0).rgb, GameCopy.SampleLevel(Linear, uv, 0).a);
}
