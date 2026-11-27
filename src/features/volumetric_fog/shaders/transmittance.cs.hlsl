// The transmittance from a height towards the atmosphere's top, by height and zenith angle.
#include "atmosphere.hlsli"

RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float2 uv = (id.xy + 0.5) / TransmittanceSize;
    float H = sqrt(TopRadius * TopRadius - BottomRadius * BottomRadius);
    float rho = H * uv.y;
    float viewHeight = sqrt(rho * rho + BottomRadius * BottomRadius);
    float dMin = TopRadius - viewHeight, dMax = rho + H;
    float d = dMin + uv.x * (dMax - dMin);
    float viewZenithCos = d == 0 ? 1 : clamp((H * H - rho * rho - d * d) / (2 * viewHeight * d), -1, 1);

    float3 position = float3(0, 0, viewHeight);
    float3 direction = float3(0, sqrt(1 - viewZenithCos * viewZenithCos), viewZenithCos);
    float tMax = RaySphereIntersectNearest(position, direction, 0, TopRadius);
    float tBottom = RaySphereIntersectNearest(position, direction, 0, BottomRadius);
    if (tBottom >= 0) tMax = min(tMax, tBottom);

    static const float Samples = 40;
    float3 opticalDepth = 0;
    float t = 0;
    for (float s = 0; s < Samples; s += 1)
    {
        float next = tMax * (s + 0.3) / Samples;
        opticalDepth += SampleMedium(length(position + next * direction) - BottomRadius).extinction * (next - t);
        t = next;
    }
    Output[id.xy] = float4(exp(-opticalDepth), 1);
}
