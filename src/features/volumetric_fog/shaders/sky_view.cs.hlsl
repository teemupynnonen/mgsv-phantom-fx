// The sky around the camera, per unit of sunlight (Hillaire's sky-view lookup table).
#include "atmosphere.hlsli"

RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= uint2(SkyViewSize))) return;

    float cameraHeight = SkyParameters.Load(int3(SunTexel, 0, 0)).a;
    float3 sunWorld = ToAtmosphere(SkyParameters.Load(int3(DirectionTexel, 0, 0)).xyz);
    float viewHeight = BottomRadius + cameraHeight;

    float2 uv = (id.xy + 0.5) / SkyViewSize;
    uv = float2(FromSubUvsToUnit(uv.x, SkyViewSize.x), FromSubUvsToUnit(uv.y, SkyViewSize.y));
    float beta = acos(sqrt(viewHeight * viewHeight - BottomRadius * BottomRadius) / viewHeight);
    float zenithHorizonAngle = Pi - beta;
    float viewZenithCos;
    if (uv.y < 0.5)
    {
        float coord = 1 - 2 * uv.y;
        viewZenithCos = cos(zenithHorizonAngle * (1 - coord * coord));
    }
    else
    {
        float coord = uv.y * 2 - 1;
        viewZenithCos = cos(zenithHorizonAngle + beta * coord * coord);
    }
    float lightViewCos = -(uv.x * uv.x * 2 - 1);

    float sunZenithCos = sunWorld.z;
    float3 towardsSun = normalize(float3(sqrt(saturate(1 - sunZenithCos * sunZenithCos)), 0, sunZenithCos));
    float viewZenithSin = sqrt(saturate(1 - viewZenithCos * viewZenithCos));
    float3 direction = float3(viewZenithSin * lightViewCos, viewZenithSin * sqrt(saturate(1 - lightViewCos * lightViewCos)), viewZenithCos);
    Output[id.xy] = float4(Integrate(float3(0, 0, viewHeight), direction, towardsSun, 30, true, true, true, false).light, 1);
}
