Texture2D<float4> Scene : register(t0);
SamplerState Linear : register(s0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return Scene.SampleLevel(Linear, uv, 0);
}
