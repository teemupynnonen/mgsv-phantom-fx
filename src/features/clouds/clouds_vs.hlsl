// The clouds' full-screen vertex shader (clouds.cpp): one triangle over the render target. It marks its draw with a
// negative w in TEXCOORD1, which clouds.hlsl reads as "build the ray from the camera and read the scene's depth".
struct Output
{
    float4 color : COLOR0;
    float4 uv : TEXCOORD0;
    float4 world : TEXCOORD1;
    float4 position : SV_Position;
};

Output main(uint id : SV_VertexID)
{
    float2 corner = float2((id << 1) & 2, id & 2);
    Output output;
    output.position = float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
    output.color = 1;
    output.uv = float4(corner, 0, 0);
    output.world = float4(0, 0, 0, -1);

    return output;
}
