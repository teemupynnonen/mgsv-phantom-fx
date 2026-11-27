#pragma warning(disable : 3556)
// A point or spot light's screen-space shadow into the alpha of both light targets, drawn with the
// light's own volume; nothing reads that alpha after the lights.
//
// A march through the depth buffer towards the light, in the light's own draw: the shape of
// renodx-mgsv's screen-space term, which costs a handful of depth reads per pixel the light's volume
// covers, where Bend Studio's trace cost a compute dispatch over that volume's whole screen
// rectangle with 120 samples a pixel. The ray starts at the surface the pixel shows, steps a fraction
// of the contact shadow's reach towards the light, and counts the steps the depth buffer puts a
// surface in front of. A caster only counts within a separation that grows with the distance, so
// layered gear on a character is not read as a wall in front of its own surface. A pixel whose surface
// faces away from the light is skipped: the light adds nothing there, so its shadow is never read.
cbuffer cPSScene : register(b2)
{
    struct
    {
        float4x4 m_projectionView;
        float4x4 m_projection;
        float4x4 m_view;
        float4 m_shadowProjection[4];
        float4 m_cascades[4];
        float4 m_eyepos;
        float4 m_projectionParam;       // 1 / P00, 1 / P11, B, A
        float4 m_viewportSize;
        float4 m_unused[5];
        float4 m_cameraCenterOffset;    // The upscaler's jitter in NDC, which the game's shaders take out
    } g_psScene : packoffset(c0);
};

cbuffer cPSLight : register(b3)
{
    struct
    {
        float4 m_lightParams[11];
    } g_psLight : packoffset(c0);
};

// The add-on's own options, in the slot the game leaves unbound on this draw.
cbuffer cLampTrace : register(b6)
{
    float g_reachPixels;    // Pixels of the render size the march covers, along the screen
    float g_thickness;      // Metres: how far in front of the ray a surface must sit to count
    float g_contrast;       // How dark what the march finds comes out
    float g_fadeMetres;     // Metres: past this the contact shadow is gone
    float g_rangeMetres;    // Metres: how far the light reaches
    float g_steps;          // The march's steps
    float g_viewportWidth;
    float g_viewportHeight;
    float g_useNormals;     // 1 when the marking draw has the G-buffer's normals bound at t0
    float3 g_padding;
};

Texture2D<float> g_depth : register(t2);
Texture2D<float4> g_normals : register(t0);
// Bound by the add-on for the gather below, and put back afterwards.
SamplerState g_pointClamp : register(s0);

// The longest the march reaches into the world, whatever the reach in pixels works out to at range: a
// contact shadow is short, and past this the light's own shadow, or its absence, is the better answer.
static const float MaxReachMetres = 6.0;
static const int MaxPairs = 32;
// How far behind the ray a surface may sit and still count: a quarter of a metre close up, a centimetre
// a metre further out.
static const float CasterMaxMetres = 0.25;
static const float CasterPerMetre = 0.01;
// The contact shadows keep their strength out to this share of the fade distance.
static const float FadeStartShare = 0.6;

// renodx-mgsv's per-pixel rotation of the march's steps, so a step pattern reads as grain, not bands.
float interleavedGradientNoise(float2 position)
{
    const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);

    return frac(magic.z * frac(dot(position, magic.xy)));
}

// The G-buffer's normal, decoded as the game's own light shaders decode it, so the terminator the
// march is skipped on is the one the game's lighting has. The game takes 1 - z^2, unrooted, both as
// the scale and inside the root, which leaves xy sqrt(1 - z^2) long.
float3 decodeNormal(float3 encoded)
{
    const float2 slope = encoded.xy * 2 - 1;
    const float z = encoded.z * encoded.z * 2 - 1;
    const float rest = max(1 - z * z, 0);
    const float scale = rest * rsqrt(1e-7 + rest * dot(slope, slope));

    return float3(slope * scale, z);
}

void main(float4 position : SV_Position, out float4 diffuse : SV_Target0, out float4 specular : SV_Target1)
{
    const int2 pixel = int2(position.xy);
    const float depth = g_depth.Load(int3(pixel, 0));
    const float4 projectionParam = g_psScene.m_projectionParam;
    // Fox Engine's depth is reversed: B / (depth - A) is the view depth in metres.
    const float viewZ = projectionParam.z / max(depth - projectionParam.w, 1e-6);

    // Lit: outside the fade, and for a pixel the light cannot reach at all. Both are most of a light's
    // volume on screen, which is why they come first.
    float shadow = 1;
    [branch] if (viewZ > 0 && viewZ < g_fadeMetres)
    {
        const float2 uv = (pixel + 0.5) / float2(g_viewportWidth, g_viewportHeight);
        // The depth was drawn jittered; positions are rebuilt without it, and put back on the way to the
        // screen, as the game's own light shaders do.
        const float2 centreOffset = g_psScene.m_cameraCenterOffset.xy;
        const float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2) - centreOffset;
        const float3 viewPosition = float3(ndc.x * viewZ * projectionParam.x, ndc.y * viewZ * projectionParam.y, viewZ);

        // m_lightParams[3].xyz is the light's own view-space position.
        const float3 toLight = g_psLight.m_lightParams[3].xyz - viewPosition;
        const float lightDistance = length(toLight);

        [branch] if (lightDistance < g_rangeMetres)
        {
            const int steps = (int)g_steps;
            // Two steps to a gather. The four texels one gather returns sit inside a two-pixel square,
            // which is the ground two steps cover, so the march reads the same span with half the
            // instructions and twice the comparisons: a gather is one memory request for four depths.
            const int pairs = max(steps / 2, 1);
            const int comparisons = pairs * 4;
            const float fade = saturate((g_fadeMetres - viewZ) / max(g_fadeMetres * (1 - FadeStartShare), 1e-3));
            const float strength = 2 * g_contrast;
            // A count of comparisons in shadow this dark is as dark as it gets: past it the march is spent.
            const int saturated = (int)ceil(comparisons / max(strength * fade, 1e-6));

            // The reach is in pixels of the screen, so the world it covers grows with the depth; the
            // step's own screen length is the same at every distance, which is what keeps a contact
            // shadow under a character's feet from sweeping the whole frame. It ends at the light:
            // nothing past it can shadow it.
            const float reachNdc = g_reachPixels * 2 / g_viewportHeight;
            const float reach = min(min(reachNdc * abs(viewZ * projectionParam.y), MaxReachMetres), lightDistance);
            const float stepLength = reach / max(steps, 1);
            const float3 direction = toLight / max(lightDistance, 1e-4);
            const float thickness = max(g_thickness, 1e-3);
            const float casterMax = max(CasterMaxMetres, viewZ * CasterPerMetre);
            const float jitter = interleavedGradientNoise(position.xy);

            // Facing away from the light, which the game clamps at zero and scales both its diffuse and
            // its specular by: this pixel takes no light from this one, so the alpha below is never
            // read and the march is spent for nothing. The G-buffer's normal is free here, where
            // recomputing the whole light to ask renodx-mgsv's question would not be.
            [branch] if (g_useNormals == 0 || dot(decodeNormal(g_normals.Load(int3(pixel, 0)).xyz), direction) > 0)
            {
                int occluded = 0;
                [loop] for (int pair = 0; pair < MaxPairs; ++pair)
                {
                    if (pair >= pairs || occluded >= saturated) break;

                    // Pairs crowd near the surface, where a contact is, and thin out with the distance.
                    const float u = ((float)pair + 0.5 + jitter) / pairs;
                    const float travelled = stepLength * (0.25 * u + 0.75 * u * u) * steps;
                    const float3 along = viewPosition + direction * travelled;
                    if (along.z <= 1e-4) break;

                    const float2 alongNdc = float2(along.x / (along.z * projectionParam.x), along.y / (along.z * projectionParam.y)) + centreOffset;
                    const float2 alongUv = float2(alongNdc.x * 0.5 + 0.5, 0.5 - alongNdc.y * 0.5);
                    if (any(alongUv < 0) || any(alongUv >= 1)) break;

                    // The window a caster may sit in, as two depths rather than as the view depth of each
                    // of the four texels: the depth buffer stores A + B / z, so the two view depths the
                    // window is between convert to the two the buffer holds, once for the four samples.
                    const float nearDepth = projectionParam.w + projectionParam.z / max(along.z - thickness, 1e-4);
                    const float farDepth = projectionParam.w + projectionParam.z / max(along.z - casterMax, 1e-4);
                    const float4 scene = g_depth.Gather(g_pointClamp, alongUv);
                    occluded += (scene.x > nearDepth && scene.x < farDepth ? 1 : 0) + (scene.y > nearDepth && scene.y < farDepth ? 1 : 0)
                        + (scene.z > nearDepth && scene.z < farDepth ? 1 : 0) + (scene.w > nearDepth && scene.w < farDepth ? 1 : 0);
                }

                shadow = saturate(1 - (float)occluded / max(comparisons, 1) * strength * fade);
            }
        }
    }

    diffuse = specular = shadow;
}
