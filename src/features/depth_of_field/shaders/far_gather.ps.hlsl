// A lens's disc, gathered: taps on a golden-angle spiral fill it evenly, and a tap counts if its own blur
// reaches this far (Jimenez 2014, "scatter as you gather"). The result is multiplied by how much of the
// disc had far light in it, which is what the fill and the composite weigh by.
Texture2D<float4> Prefiltered : register(t3);
SamplerState Linear : register(s11);

static const float LeastRadius = 2;     // So an in-focus texel still finds the light around it
static const float TapsPerArea = 3.2;   // Taps per squared radius, about one a texel
static const int MostTaps = 96;
static const float2 GoldenTurn = float2(-0.73736887, 0.67549029);

float4 main(float4 position : SV_Position) : SV_Target
{
    uint width, height;
    Prefiltered.GetDimensions(width, height);
    const float2 texelSize = 1 / float2(width, height);
    const float2 uv = position.xy * texelSize;

    const float radius = max(Prefiltered.Load(int3(position.xy, 0)).a, LeastRadius);
    const int taps = min(int(TapsPerArea * radius * radius), MostTaps);

    float4 sum = 0;
    float2 direction = float2(1, 0);
    for (int i = 0; i < taps; ++i)
    {
        const float distance = radius * sqrt((i + 0.5) / taps);
        const float4 tap = Prefiltered.SampleLevel(Linear, uv + direction * distance * texelSize, 0);
        const float weight = saturate(tap.a - distance + 1);
        sum += float4(tap.rgb / max(tap.a, 1e-4), 1) * weight;
        direction = float2(direction.x * GoldenTurn.x - direction.y * GoldenTurn.y, direction.x * GoldenTurn.y + direction.y * GoldenTurn.x);
    }

    return sum / taps;
}
