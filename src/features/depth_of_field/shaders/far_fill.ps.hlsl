// Evens out what the gather's taps left between them (Jimenez 2014's second pass), on the gather's
// multiplied light, so each pass is a plain average. It runs twice, RING_WIDTH being the ring's width
// of the radius. Past the gather's most taps a small lamp's disc breaks into its taps. A ring as wide
// as they are apart lands on the neighbouring taps, so the first sits nearer, and the second, half as
// wide, evens out what that leaves: each tap spreads over 81 points for 18 reads a pixel, which holds
// up better than 96 more taps would (docs/DEVELOPMENT.md, "Depth of field").
Texture2D<float4> Layer : register(t3);
Texture2D<float4> Prefiltered : register(t4);
SamplerState Linear : register(s11);

static const float2 Ring[8] = {float2(0.92, 0.38), float2(0.38, 0.92), float2(-0.38, 0.92), float2(-0.92, 0.38),
                               float2(-0.92, -0.38), float2(-0.38, -0.92), float2(0.38, -0.92), float2(0.92, -0.38)};
static const float TapSpacing = 0.18;   // How far apart the gather's most taps are, of the radius: sqrt(pi / 96)
static const float LeastSpacing = 0.75; // Texels; bilinear taps closer than this leave nothing to even out

float4 main(float4 position : SV_Position) : SV_Target
{
    uint width, height;
    Layer.GetDimensions(width, height);
    const float2 texelSize = 1 / float2(width, height);
    const float2 uv = position.xy * texelSize;

    const float radius = Prefiltered.Load(int3(position.xy, 0)).a;
    float4 sum = Layer.Load(int3(position.xy, 0));

    if (radius * TapSpacing < LeastSpacing) return sum;

    for (int i = 0; i < 8; ++i)
        sum += Layer.SampleLevel(Linear, uv + Ring[i] * radius * RING_WIDTH * texelSize, 0);

    return sum / 9;
}
