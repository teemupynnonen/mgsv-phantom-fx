// The clouds back over the scene at full size (clouds.cpp): clouds.hlsl marched them at half the render size, each of its pixels
// for a 2 x 2 block, to the farthest surface in it. Each pixel here blends its four nearest, leaving out any whose cloud begins
// behind this pixel's own surface, so a ridge or a crane keeps a sharp edge against the sky behind it.
cbuffer cPSSystem : register(b0)
{
    float4 m_param, m_renderInfo, m_renderBuffer, m_dominantLightDir;
};

cbuffer cPSScene : register(b2)
{
    float4 m_scene[26]; // [4..7] the projection, [8..10] the view's axes in the world
};

Texture2D<float> SceneDepth : register(t13);
Texture2D<float4> LowColor : register(t19);   // The cloud, premultiplied, as the game's blend takes it
Texture2D<float> LowDistance : register(t20); // How far its near edge is, metres

static const float LowScale = 2;

float4 main(float4 color : COLOR0, float4 uv : TEXCOORD0, float4 world : TEXCOORD1, float4 position : SV_Position) : SV_Target
{
    float2 ndc = float2(position.x / m_renderInfo.x * 2 - 1, 1 - position.y / m_renderInfo.y * 2);
    float3 viewRay = m_scene[8].xyz * (ndc.x / m_scene[4].x) + m_scene[9].xyz * (ndc.y / m_scene[5].y) + m_scene[10].xyz;
    float depth = SceneDepth.Load(int3(position.xy, 0)).x;
    float sceneDistance = depth > 0 ? m_scene[6].w / (depth - m_scene[6].z) * length(viewRay) : 1e9;

    uint width, height;
    LowColor.GetDimensions(width, height);
    // A low pixel's centre is at LowScale * (i + 0.5) here.
    float2 low = position.xy / LowScale - 0.5;
    int2 base = int2(floor(low));
    float2 f = low - base;

    float4 sum = 0;
    float weights = 0;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        int2 offset = int2(k & 1, k >> 1);
        int2 at = clamp(base + offset, int2(0, 0), int2(width - 1, height - 1));
        float w = (offset.x ? f.x : 1 - f.x) * (offset.y ? f.y : 1 - f.y) + 1e-4;
        // A cloud that begins behind this pixel's surface is hidden by it; clear sky counts as clear.
        float4 cloud = LowColor.Load(int3(at, 0));
        if (cloud.a > 0 && LowDistance.Load(int3(at, 0)) > sceneDistance) w = 0;
        sum += cloud * w;
        weights += w;
    }

    return weights > 0 ? sum / weights : 0;
}
