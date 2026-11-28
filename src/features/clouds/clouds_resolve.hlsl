// The clouds' history at half the render size (clouds.cpp): this frame's march blended into what the clouds looked like before.
// The march's dither turns every frame, so one frame alone is noisy and, at half size, flickers in 2 x 2 blocks; the history
// averages it into a steady cloud. Clouds are kilometres away, so the camera's turn alone says where a cloud was on the screen:
// the ray is taken into last frame's view (ViewHistory, copied by clouds.cpp after each frame).
cbuffer cPSSystem : register(b0)
{
    float4 m_param, m_renderInfo, m_renderBuffer, m_dominantLightDir;
};

cbuffer cPSScene : register(b2)
{
    float4 m_scene[26]; // [4].x and [5].y the projection's scale, [8..10] the view's axes in the world
};

SamplerState LinearClamp : register(s11);
Texture2D<float4> Current : register(t19);  // This frame's march
Texture2D<float4> History : register(t21);  // The resolved clouds of the frame before
// Last frame's [0] m_scene[4], [1] m_scene[5], [2..4] m_scene[8..10]; [5].x 1 once it holds a frame.
StructuredBuffer<float4> ViewHistory : register(t22);

static const float LowScale = 2;
// How much of this frame a pixel takes: about the last ten frames make the cloud.
static const float Blend = 0.1;

float3 RayOf(float2 pixel)
{
    float2 ndc = float2(pixel.x / m_renderInfo.x * 2 - 1, 1 - pixel.y / m_renderInfo.y * 2);
    return normalize(m_scene[8].xyz * (ndc.x / m_scene[4].x) + m_scene[9].xyz * (ndc.y / m_scene[5].y) + m_scene[10].xyz);
}

float4 main(float4 color : COLOR0, float4 uv : TEXCOORD0, float4 world : TEXCOORD1, float4 position : SV_Position) : SV_Target
{
    int2 at = int2(position.xy);
    float4 now = Current.Load(int3(at, 0));
    if (ViewHistory[5].x < 0.5) return now;

    // The neighbourhood's range: a history outside it is a cloud that moved or a view that changed, and is pulled back into it
    // rather than left to smear.
    float4 low = now, high = now;
    [unroll] for (int k = 0; k < 9; ++k)
    {
        if (k == 4) continue;
        float4 near = Current.Load(int3(at + int2(k % 3 - 1, k / 3 - 1), 0));
        low = min(low, near);
        high = max(high, near);
    }

    float3 ray = RayOf((floor(position.xy) + 0.5) * LowScale);
    float3 inLast = float3(dot(ray, ViewHistory[2].xyz), dot(ray, ViewHistory[3].xyz), dot(ray, ViewHistory[4].xyz));
    if (inLast.z <= 0) return now;
    float2 ndc = inLast.xy / inLast.z * float2(ViewHistory[0].x, ViewHistory[1].y);
    float2 lastUV = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(lastUV < 0) || any(lastUV > 1)) return now;

    float4 before = clamp(History.SampleLevel(LinearClamp, lastUV, 0), low, high);
    return lerp(before, now, Blend);
}
