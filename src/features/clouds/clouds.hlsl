// The clouds' pixel shader (docs/DEVELOPMENT.md, "Clouds"): drawn in place of tpp3dfw_generativeClouds with the game's vertex shader and bindings.
// Precompiled by fxc at build time; a dev build recompiles this file when it is saved (src/hot_shader.hpp).
#include "cloud_density.hlsli"

SamplerState LinearClamp : register(s11);
SamplerState PointWrap : register(s8);
Texture2D<float4> GameFog : register(t12);
Texture2D<float4> Dither : register(t15);
// The scene's depth (reversed: 0 is the far plane), bound by the game at its cloud draw.
Texture2D<float> SceneDepth : register(t13);
// The ground's cloud shadow (cloud_shadow.hlsl), drawn by clouds.cpp just before this draw.
Texture2D<float> CloudShadow : register(t16);
// The physical sky's parameters (volumetric_fog/shaders/atmosphere.hlsli), or nothing while it is off: then every texel reads 0.
Texture2D<float4> SkyParameters : register(t18);
static const int SkyDirectionTexel = 1, SkyAmbientTexel = 2, SkyFogTexel = 3, SkyFogShapeTexel = 4, CloudSunTexel = 7;
// The physical sky's light is in the game's exposed units already, and physical: a thick cloud's lit side gives back about what a
// white wall in the sun does (albedo / pi of the sun, with the phase's mean about 0.17), and its shaded side about the sky's own light.
// Measured in Africa at noon: at 2.5 and 3 every cloud clipped to a flat white, and at 1 a lit side still reached 1.0 against a sky of
// about 0.07; at these a lit side is a few times the sky, as a cumulus is, and a shaded side reads grey.
static const float PhysicalSunScale = 0.5, PhysicalSkyScale = 0.9;

static const float Pi = 3.14159265;
static const int Steps = 128;
// How far a ray may march: the layer's chord at the horizon is hundreds of kilometres, and stopping short shows as a hard band.
static const float FarReach = 150000;

float HG(float mu, float g)
{
    float g2 = g * g;
    return (1 - g2) / (4 * Pi * pow(max(1 + g2 - 2 * g * mu, 1e-4), 1.5));
}

// The Earth's radius: a point r metres out along the ground stands r^2 / 2R higher above the curved surface than the flat world says.
static const float EarthRadius = 6360000;

// Where a ray's height above the curved Earth, eyeY + rayY t + t^2 / 2R, crosses `height`: the later crossing (rising) or the
// earlier one (falling), -1 if it never does. Each root in the form that does not cancel in single precision.
float CurvedCrossing(float eyeY, float rayY, float height, bool rising)
{
    float c = eyeY - height;
    float d = rayY * rayY - 2 * c / EarthRadius;
    if (d < 0) return -1;
    float s = sqrt(d);
    float t = rising ? (rayY > 0 ? -2 * c / (rayY + s) : EarthRadius * (s - rayY)) : EarthRadius * (-rayY - s);
    return t > 0 ? t : -1;
}

// A world point's height above the curved Earth, the eye standing on the flat world's level.
float3 Curved(float3 where, float3 eye)
{
    float2 across = where.xz - eye.xz;
    return float3(where.x, where.y + dot(across, across) / (2 * EarthRadius), where.z);
}

float3 Encode(float3 linearColor)
{
    float3 high = 1.055 * pow(max(linearColor, 0.00001), 1 / 2.4) - 0.055;
    return linearColor <= 0.0031308 ? linearColor * 12.92 : high;
}

// The game's sun is ~68 after exposure; these bring a lit top near 0.9 without clipping the shadows away.
static const float SunScale = 0.1, AmbientScale = 1.4;
// 0 the cloud, 1 the sun's transmittance only, 2 the alpha only, 3 the peak density, 4 the ground's cloud shadow.
static const int DebugView = 0;
// The add-on's full-screen draw marches at half the render size (clouds.cpp, LowScale): each pixel stands for a 2 x 2 block of the
// scene, and clouds_composite.hlsl brings it back over the scene at full size. Out: the cloud, and how far its near edge is.
static const float LowScale = 2;
struct CloudOut
{
    float4 color : SV_Target0;
    float distance : SV_Target1;
};

CloudOut Out(float4 color, float distance)
{
    CloudOut result;
    result.color = color;
    result.distance = distance;
    return result;
}

CloudOut main(float4 color : COLOR0, float4 uv : TEXCOORD0, float4 world : TEXCOORD1, float4 position : SV_Position)
{
    float exposure = m_scene[23].z;
    float3 eye = m_scene[20].xyz;

    float3 ray;
    float2 pixel = position.xy; // In the render's own pixels
    float domeAlpha = 1;
    float sceneDistance = 1e9;
    if (world.w < -0.5)
    {
        // The add-on's full-screen draw (clouds.cpp): the ray from the camera's own matrices, and how far the
        // nearest surface is. m_scene[4..7] is the projection, [8..10] the view's axes in the world.
        pixel = floor(position.xy) * LowScale + LowScale * 0.5;
        float2 ndc = float2(pixel.x / m_renderInfo.x * 2 - 1, 1 - pixel.y / m_renderInfo.y * 2);
        float3 viewRay = m_scene[8].xyz * (ndc.x / m_scene[4].x) + m_scene[9].xyz * (ndc.y / m_scene[5].y) + m_scene[10].xyz;
        ray = normalize(viewRay);
        // The farthest of the block's surfaces: the march reaches behind every one of them, and the composite leaves out what lies
        // behind its own pixel's.
        int2 corner = int2(floor(position.xy) * LowScale);
        float depth = min(min(SceneDepth.Load(int3(corner, 0)).x, SceneDepth.Load(int3(corner + int2(1, 0), 0)).x),
            min(SceneDepth.Load(int3(corner + int2(0, 1), 0)).x, SceneDepth.Load(int3(corner + int2(1, 1), 0)).x));
        // Reversed depth: sky is exactly 0, every surface a small positive value.
        sceneDistance = depth > 0 ? m_scene[6].w / (depth - m_scene[6].z) * length(viewRay) : 1e9;
    }
    else
    {
        // The game's dome: the sky the rays see (sky_capture.cpp), with the dome's own fade.
        ray = normalize(world.xyz - eye);
        domeAlpha = color.x;
    }

    if (DebugView == 5) return Out(float4(CloudShadow.SampleLevel(LinearClamp, pixel / m_renderInfo.xy, 0).xxx, 1), 0);
    if (DebugView == 4 && sceneDistance < 1e8)
    {
        float2 shadowUV = CloudShadowUV(eye + ray * sceneDistance, normalize(m_materials[0].xyz), CloudShadowCentre(eye));
        return Out(float4(CloudShadow.SampleLevel(LinearClamp, shadowUV, 0).xxx, 1), 0);
    }
    float baseCover = CloudBaseCover(domeAlpha);

    // The layer follows the Earth's curve: a flat slab ran a horizon ray through 150 km of cloud and packed the deck into a wall.
    // Curved, a level ray from the deck meets the base about 105 km out and leaves through the top soon after.
    float tEnter = 0, tExit = -1;
    float riseBase = CurvedCrossing(eye.y, ray.y, LayerBase, true), riseTop = CurvedCrossing(eye.y, ray.y, LayerTop, true);
    float fallBase = CurvedCrossing(eye.y, ray.y, LayerBase, false), fallTop = CurvedCrossing(eye.y, ray.y, LayerTop, false);
    if (eye.y < LayerBase)
    {
        tEnter = riseBase;
        tExit = riseTop;
    }
    else if (eye.y < LayerTop)
    {
        tExit = fallBase > 0 ? fallBase : riseTop;
    }
    else if (fallTop > 0)
    {
        tEnter = fallTop;
        tExit = fallBase > 0 ? fallBase : riseTop;
    }
    // To the layer's own exit, not a fixed 12 km: at a grazing angle the chord through the layer is hundreds of kilometres, and cutting
    // it at 12 left the bottom of the deck unmarched, a hard-edged band of bare sky above the horizon.
    tExit = min(min(tExit, tEnter + FarReach), sceneDistance);
    float span = tExit - tEnter;
    float farFade = 1 - smoothstep(60000, 150000, tEnter);

    float2 windXZ = CloudWindXZ();
    float3 wind = CloudWind();

    float3 sunLight = m_object[10].xyz * m_materials[2].w * exposure;
    float3 ambient = m_materials[2].xyz * exposure + 0.00025 * sunLight;
    // The sky's blue, mostly greyed: a cloud's shaded side is a neutral grey, not lavender.
    ambient = lerp(ambient, dot(ambient, float3(0.2126, 0.7152, 0.0722)).xxx, 0.65);
    float3 towardsSun = normalize(m_materials[0].xyz);
    // With the physical sky, its own sun and sky: the game's cloud light stays white as the sun sets, so the clouds were white against
    // an orange sky at dusk and bright against a dark one after it.
    float4 deckSun = SkyParameters.Load(int3(CloudSunTexel, 0, 0));
    bool physical = deckSun.a > 0.5;
    if (physical)
    {
        sunLight = deckSun.rgb * (PhysicalSunScale / SunScale);
        float3 sky = SkyParameters.Load(int3(SkyAmbientTexel, 0, 0)).rgb * (PhysicalSkyScale / AmbientScale);
        ambient = lerp(sky, dot(sky, float3(0.2126, 0.7152, 0.0722)).xxx, 0.3);
        towardsSun = normalize(SkyParameters.Load(int3(SkyDirectionTexel, 0, 0)).xyz);
    }
    float mu = dot(ray, towardsSun);
    float phase = 0.7 * HG(mu, 0.6) + 0.2 * HG(mu, -0.2) + 0.1;

    float transmittance = 1;
    float3 light = 0;
    // How far the light actually scattered, weighted like it: a horizon cloud is kilometres away even though the ray met
    // the layer a few hundred metres out.
    float farWeight = 0;
    float sunSeen = 0, peak = 0;
    float nearEdge = 1e9; // Where the march first took light: the cloud's near edge, for the composite
    if (span > 1 && tEnter < 150000)
    {
        const int steps = max(16, (int)g_cloudSteps);
        // At least 25.6 km over the steps (100 m on Ultra, 400 on Low): a fixed count of steps took every pixel 256 times through the
        // layer, 20 m apart looking up, which cost as much over a clear sky as over a cloudy one. A cloud is kilometres across, and the
        // fine steps inside it are a quarter of this.
        float stepLength = max(span / steps, 25600.0 / steps);
        // Turned by the golden ratio each frame: the same dither every frame left a fixed grain the upscaler could not average.
        float jitter = frac(Dither.SampleLevel(PointWrap, position.xy / 8, 0).x + g_cloudFrame * 0.618034);
        const float lightStep = 350;

        // Adaptive: coarse steps through clear air, quarter steps inside a cloud, so a dense core is sampled finely
        // enough not to turn into a halftone.
        float t = tEnter + stepLength * jitter;
        int fineLeft = 0; // fine steps left before going back to coarse ones
        for (int i = 0; i < steps * 2 && t < tExit; ++i)
        {
            float3 level = eye + ray * t;
            float3 where = Curved(level, eye);
            // What one sample stands for: its pixel across (about a milliradian), and far out the step along, where steps are hundreds
            // of metres and the jitter alone would pick a different bit of the noise for each pixel.
            float footprint = max(t * 0.001, stepLength * 0.25 * smoothstep(10000, 60000, t));
            float density = saturate(CloudDensity(where, windXZ, wind, baseCover, footprint) * 4) * farFade * smoothstep(30, 250, t);
            peak = max(peak, density);
            // Every pixel's steps landed at the same depths, so a cloud's slabs read as rings around it. Each step's length varies by
            // its own turn of the dither, which breaks the rings into the same fine noise the jitter already makes.
            float wobble = 0.65 + 0.7 * frac(jitter + i * 0.618);
            if (fineLeft == 0)
            {
                if (density <= 0.004) { t += stepLength * wobble; continue; }
                // A coarse step landed inside: go back and come in finely, so the edge is found at the same place
                // by every pixel and not wherever the dither put its coarse step.
                t = max(t - stepLength, tEnter);
                fineLeft = 8;
                continue;
            }
            float dt = stepLength * 0.25 * wobble;
            t += dt;
            fineLeft = density > 0.004 ? 8 : fineLeft - 1;
            if (density <= 0.004) continue;
            // Towards the sun in world space, through the bulk of the same density: four steps lengthening to ~5.5 km.
            // Six full lookups here, every fine step, were most of the clouds' cost.
            float opticalDepth = 0, reach = 0;
            [unroll] for (int k = 0; k < 4; ++k)
            {
                float len = 1 + 1.9 * k;
                float3 lightPos = Curved(level + towardsSun * lightStep * (reach + 0.5 * len), eye);
                reach += len;
                opticalDepth += saturate(CloudDensity(lightPos, windXZ, wind, baseCover, max(footprint, 200), true) * 4) * len * lightStep;
            }
            opticalDepth *= Extinction;
            // Three octaves of ever-softer extinction (Wrenninge's approximation): a floor that high and that flat (0.7) lit every
            // side alike, and the clouds read as flat paint. Now the lit side stands out and the depth still keeps a shaded side grey.
            float sunT = 0.55 * exp(-opticalDepth) + 0.3 * exp(-opticalDepth * 0.25) + 0.15 * exp(-opticalDepth * 0.06);

            float height = saturate((where.y - LayerBase) / (LayerTop - LayerBase));
            float3 scatter = sunLight * phase * sunT * SunScale + ambient * lerp(0.55, 1.0, height) * AmbientScale;

            // Energy-conserving step: the light this slab scatters, dimmed by what is in front of it.
            float stepT = exp(-density * Extinction * dt);
            float taken = transmittance * (1 - stepT);
            light += taken * scatter;
            nearEdge = min(nearEdge, t);
            sunSeen += taken * sunT;
            farWeight += taken * t;
            transmittance *= stepT;
            if (transmittance < 0.01) break;
        }
    }

    // `light` is the radiance scattered along the ray, integrated through the march: the sun's part where it reaches, the sky's
    // where it does not. Multiplying it by the sun again, as this did, clipped every cloud to white or left only the ambient.
    float3 outColor = light / max(1 - transmittance, 1e-4);

    float alpha = saturate((1 - transmittance) * domeAlpha);
    if (DebugView == 1) outColor = (sunSeen / max(1 - transmittance, 1e-4)).xxx;
    if (DebugView == 2) return Out(float4(alpha.xxx, 1), 0);
    if (DebugView == 3) return Out(float4(peak, saturate(peak * 10), 0, 1), 0);

    // The game's fog, as the terrain behind gets it: its froxel volume (t12, 128 slices by log2 of the view's z in a 32 x 4
    // atlas, rgb the light scattered in, a what gets through) read at the cloud's own depth. Without it a cloud 20 km out
    // stayed dark while a mountain 10 km out was nearly all fog, and the deck ended in a hard edge on the mountains.
    if (world.w < -0.5 && physical && alpha > 0)
    {
        // The physical sky's air between the eye and the cloud (its Rayleigh and Mie, atmosphere.hlsli, per km, and its height
        // fog): the cloud is dimmed and reddened by what it passes, and the sky behind, which already holds the light the air
        // scatters in, shows through by as much. Without it a cloud 80 km out was as white and as sharp as one overhead, pasted on.
        float cloudDistance = farWeight / max(1 - transmittance, 1e-4);
        float4 fogShape = SkyParameters.Load(int3(SkyFogShapeTexel, 0, 0));
        float fogExtinction = SkyParameters.Load(int3(SkyFogTexel, 0, 0)).a;
        float3 depth = 0;
        [unroll] for (int k = 0; k < 8; ++k)
        {
            float t = cloudDistance * (k + 0.5) / 8;
            float h = Curved(eye + ray * t, eye).y;
            float km = max(h, 0) * 0.001;
            depth += float3(0.005802, 0.013558, 0.033100) * exp(-km / 8) + 0.00444 * exp(-km / 1.2)
                + fogExtinction * 1000 * exp(-(h - fogShape.z) / max(fogShape.x, 1));
        }
        float3 air = exp(-depth * cloudDistance * 0.001 / 8);
        float through = dot(air, 1.0 / 3);
        outColor *= air / max(through, 1e-4);
        alpha *= through;
    }
    else if (world.w < -0.5 && g_cloudFog > 0.5)
    {
        float cloudDistance = (1 - transmittance) > 1e-4 ? farWeight / (1 - transmittance) : 0;
        float2 ndc = float2(pixel.x / m_renderInfo.x * 2 - 1, 1 - pixel.y / m_renderInfo.y * 2);
        float3 viewRay = m_scene[8].xyz * (ndc.x / m_scene[4].x) + m_scene[9].xyz * (ndc.y / m_scene[5].y) + m_scene[10].xyz;
        float viewZ = max(cloudDistance / length(viewRay), 1);
        float layer = saturate(log2(viewZ) * m_scene[25].x) * 127;
        float2 clip = (pixel - 0.5) / m_renderInfo.xy * float2(2, -2) + float2(-1, 1);
        float2 uv = clip * float2(0.014648, 0.123047) + float2(0.015625, 0.125);
        float2 slices = float2(floor(layer), min(floor(layer) + 1, 127)) / 32;
        float2 column = frac(slices), row = floor(slices) * 0.25;
        float4 nearFog = GameFog.SampleLevel(LinearClamp, uv + float2(column.x, row.x), 0);
        float4 farFog = GameFog.SampleLevel(LinearClamp, uv + float2(column.y, row.y), 0);
        float4 fog = lerp(nearFog, farFog, frac(layer));
        outColor = fog.rgb * m_scene[25].y + outColor * fog.a;
    }

    float3 encoded = Encode(max(outColor, 0));
    return Out(float4(encoded * alpha, alpha), nearEdge);
}
