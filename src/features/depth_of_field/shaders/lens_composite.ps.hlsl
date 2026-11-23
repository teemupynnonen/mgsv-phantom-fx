// With the lens's far blur. Its half-size layer is stretched by a cubic B-spline in four bilinear taps
// (Sigg and Hadwiger 2005), as a bilinear stretch shows its texels. The cover is the pixel's own radius:
// sharp at SharpRadius, all blur a render pixel over it.
#include "dof_composite.hlsli"

float4 SampleBSpline(Texture2D<float4> layer, float2 uv)
{
    uint width, height;
    layer.GetDimensions(width, height);
    const float2 size = float2(width, height);

    const float2 position = uv * size - 0.5;
    const float2 base = floor(position);
    const float2 f = position - base;
    const float2 f2 = f * f;
    const float2 f3 = f2 * f;
    const float2 w0 = (1 - 3 * f + 3 * f2 - f3) / 6;
    const float2 w1 = (4 - 6 * f2 + 3 * f3) / 6;
    const float2 w2 = (1 + 3 * f + 3 * f2 - 3 * f3) / 6;
    const float2 w3 = f3 / 6;
    const float2 g0 = w0 + w1;
    const float2 g1 = w2 + w3;
    const float2 h0 = (base - 0.5 + w1 / g0) / size;
    const float2 h1 = (base + 1.5 + w3 / g1) / size;

    return g0.y * (g0.x * layer.SampleLevel(Linear, h0, 0) + g1.x * layer.SampleLevel(Linear, float2(h1.x, h0.y), 0))
         + g1.y * (g0.x * layer.SampleLevel(Linear, float2(h0.x, h1.y), 0) + g1.x * layer.SampleLevel(Linear, h1, 0));
}

float4 main(float4 position : SV_Position) : SV_Target
{
    return Composited(position, SampleBSpline(Far, UvOf(position)), saturate(Coverage.Load(int3(position.xy, 0)).y - SharpRadius));
}
