// The emblem over the scene where the scene is behind it, through the emblem's own quad (the game's
// vertex shader). Tested against the depth, the emblem stopped at a hard edge, while the upscaler had
// already blended a character's edge pixels with the backdrop behind it: a fringe of backdrop between
// the two. Here each pixel learns how much of it the scene in front covers, from the depth without the
// jitter, bilinearly over four places across the pixel; and where it is partly covered, the backdrop
// in it is found nearby where nothing covers it, and the emblem takes its place:
//   out = pixel + (1 - covered) * (emblem - emblem alpha * backdrop)
// Layer holds the emblem drawn alone, premultiplied; Backdrop the target before it.
Texture2D<float4> Layer : register(t0);
Texture2D<float4> Backdrop : register(t1);
Texture2D<float> Depth : register(t2);

cbuffer Composite : register(b0)
{
    float2 Jitter;   // Render pixels, x right and y down
    float Nearer;    // 1 where nearer is more (reversed depth), -1 where it is less
    float Spare;
    float2 ToRender; // Render pixels per target pixel
};

// How much the scene covers a surface at depth z around `place`, in target pixels.
float Covered(float2 place, float z)
{
    uint width, height;
    Depth.GetDimensions(width, height);
    float2 at = place * ToRender + Jitter - 0.5;
    int2 base = int2(floor(at));
    float2 weight = at - base;
    int2 last = int2(width, height) - 1;
    float c[2][2];
    [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i)
            c[j][i] = Nearer * Depth.Load(int3(clamp(base + int2(i, j), 0, last), 0)) >= Nearer * z ? 1 : 0;

    return lerp(lerp(c[0][0], c[0][1], weight.x), lerp(c[1][0], c[1][1], weight.x), weight.y);
}

float4 main(float4 position : SV_Position) : SV_Target
{
    int2 pixel = int2(position.xy);
    float4 layer = Layer.Load(int3(pixel, 0));
    float4 under = Backdrop.Load(int3(pixel, 0));
    float z = position.z;

    float covered = 0.25 * (Covered(position.xy + float2(-0.25, -0.25), z) + Covered(position.xy + float2(0.25, -0.25), z)
        + Covered(position.xy + float2(-0.25, 0.25), z) + Covered(position.xy + float2(0.25, 0.25), z));
    if (covered >= 1) return under;

    float3 behind = under.rgb;
    if (covered > 0)
    {
        uint width, height;
        Backdrop.GetDimensions(width, height);
        int2 last = int2(width, height) - 1;
        float3 sum = 0;
        float weights = 0;
        [unroll] for (int ring = 1; ring <= 2; ++ring)
            [unroll] for (int k = 0; k < 8; ++k)
            {
                float angle = (k + 0.5 * ring) * 0.785398;
                float2 place = position.xy + float2(cos(angle), sin(angle)) * (1.5 * ring + 0.5);
                float open = 1 - Covered(place, z);
                open *= open;
                sum += Backdrop.Load(int3(clamp(int2(place), 0, last), 0)).rgb * open;
                weights += open;
            }

        if (weights > 0.05) behind = sum / weights;
    }

    return float4(max(under.rgb + (1 - covered) * (layer.rgb - layer.a * behind), 0), under.a);
}
