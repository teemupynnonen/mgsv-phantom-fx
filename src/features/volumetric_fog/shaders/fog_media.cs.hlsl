// One thread per froxel: what its depth slice adds to the fog. In the game's look, the fog its slice
// adds over the slice before it (rgb) and the transmittance at its depth (a), by the game's fog
// pass's formula (docs/DEVELOPMENT.md, "Volumetric fog") with the sun's share shadowed; without a
// shadow it gives the game's fog. In the physical look, the light the slice scatters towards the
// camera (rgb) and its transmittance (a), of the air and the height fog, and of the height fog alone
// in FogOnly.
#include "fog_inputs.hlsli"
#include "atmosphere.hlsli"

cbuffer FogObject : register(b3) // cPSObject of the game's fog pass: [9] its camera, [10] towards the sun
{
    float4 FogObjectParams[13];
};

cbuffer FogSystem : register(b4) // cPSSystem of the game's fog pass
{
    float4 SystemParam, SystemRenderInfo, SystemRenderBuffer, SystemDominantLightDirection;
};

cbuffer LastSunScene : register(b7) // The last frame's SunScene
{
    float4x4 LastProjectionView;
    float4x4 LastProjection;
    float4 LastView[4];
};

Texture2D<float4> RainTexture : register(t0);      // The fog pass's top-down rain cover
Texture3D<float> VisibilityHistory : register(t2); // The last frame's Visibility
Texture3D<float> CurrentVisibility : register(t3); // FogVisibility's
RWTexture3D<float4> Media : register(u0);
RWTexture3D<float> Visibility : register(u1);      // The sun's share in each froxel, over frames
RWTexture3D<float4> FogOnly : register(u2);

// Where a view-space point of this frame lay in the last frame's froxels, as a uvw of the
// visibility volume; outside 0 to 1 when it was out of view. m_view takes view to world space.
float3 LastFroxel(float3 viewPosition)
{
    float3 world = ToWorld(viewPosition) + float3(m_view[0].w, m_view[1].w, m_view[2].w);
    float3 offset = world - float3(LastView[0].w, LastView[1].w, LastView[2].w);
    float3 last = offset.x * LastView[0].xyz + offset.y * LastView[1].xyz + offset.z * LastView[2].xyz;
    if (last.z <= 0) return -1;

    float2 clip = last.xy / (last.z * FogProjectionParam.xy);
    float slice = log2(last.z) * FogParam[1].x * (Slices - 1);
    return float3(clip * float2(0.46875, 0.4921875) + 0.5, (slice + 0.5) / Slices);
}

// The sun's share in the froxel over frames: the last frames' share where the froxel was in view,
// held within what this frame's neighbouring froxels show, so it follows a moving shadow at once
// and stays smooth where nothing moves (Karis, "High Quality Temporal Supersampling", 2014).
float BlendedVisibility(uint3 id)
{
    float current = CurrentVisibility[id];
    float3 last = LastFroxel(ColumnDirection(id.xy) * SliceDepth(id.z));
    if (HistoryWeight <= 0 || any(last < 0 || last > 1)) return current;

    int3 size = int3(TileSize, Slices);
    float low = current, high = current;
    [unroll] for (int z = -1; z <= 1; ++z)
        [unroll] for (int y = -1; y <= 1; ++y)
            [unroll] for (int x = -1; x <= 1; ++x)
            {
                float neighbour = CurrentVisibility[clamp(int3(id) + int3(x, y, z), 0, size - 1)];
                low = min(low, neighbour);
                high = max(high, neighbour);
            }

    float history = clamp(VisibilityHistory.SampleLevel(LinearClamp, last, 0), low, high);
    return lerp(current, history, HistoryWeight);
}

// The fog pass's distance from its camera to a view-space point, and the direction to it.
float FogDistance(float3 viewPosition, out float3 direction)
{
    float4 position = float4(viewPosition, 1);
    float3 placed = float3(dot(position, FogPlacement[0]), dot(position, FogPlacement[1]), dot(position, FogPlacement[2]));
    float3 ray = placed - FogObjectParams[9].xyz;
    float distance = sqrt(dot(ray, ray));
    direction = ray / max(distance, 0.0001);

    return distance;
}

float OpticalDepth(float distance)
{
    return exp2(log2(max(distance - FogMedium[0].z, 0)) * FogMedium[1].w);
}

// How open to the rain the camera stands, as the fog pass reads it: 1 in the open.
float RainAlpha()
{
    float3 position = float3(FogMedium[4].x - FogObjectParams[9].x, FogObjectParams[9].z + FogMedium[4].y, FogMedium[4].w - FogObjectParams[9].y);
    float3 screen = float3(position.xy * 0.03 * float2(0.5, -0.5) + 0.5, 1 - saturate(position.z * 0.02));
    float4 cover = RainTexture.SampleLevel(LinearClamp, screen.xy - SystemRenderBuffer.zw * 0.5, 0).xzyw;
    float2 t = saturate((cover.xy + 0.001 - screen.z) / 0.001);
    float2 alpha = 1 - t + cover.zw * t;

    return alpha.x * alpha.y;
}

// The game's look: its formula, the sun's share shadowed.
float4 GameMedia(uint3 id, float visibility)
{
    float3 column = ColumnDirection(id.xy);
    float nearDepth = id.z == 0 ? 0 : SliceDepth(id.z - 1);
    float farDepth = SliceDepth(id.z);
    float3 direction, unused;
    float farDistance = FogDistance(column * farDepth, direction);
    float nearDistance = id.z == 0 ? 0 : FogDistance(column * nearDepth, unused);

    float representativeDensity = FogMedium[0].x, mieRatio = FogMedium[0].y, sunIntensity = FogMedium[0].w;
    float mieK = FogMedium[2].w * RainAlpha();
    float3 skyLuminance = FogMedium[1].xyz, betaM = FogMedium[2].xyz, betaR = FogMedium[3].xyz, betaU = FogMedium[3].www;
    float mu = dot(FogObjectParams[10].xyz, direction);
    float miePhase = (1 - mieK * mieK) * 0.0795775 / ((1 - mieK * mu) * (1 - mieK * mu));
    miePhase = miePhase * (1 - mieRatio) + mieRatio;
    float rayleighPhase = (mu * mu + 1) * 0.0596831;
    float3 sun = (miePhase * betaM + rayleighPhase * betaR) * sunIntensity;
    float3 ambient = skyLuminance * betaU;
    float3 extinction = max(betaM + betaR + betaU, 0.000001);

    // The game's colour is (sun + ambient) / extinction x (1 - e^-extinction x optical depth); each
    // slice adds its part of that, so the slices add up to it where nothing shadows the sun.
    float3 before = id.z == 0 ? 1 : exp(-extinction * OpticalDepth(nearDistance));
    float3 after = exp(-extinction * OpticalDepth(farDistance));
    float3 added = (sun * visibility + ambient) / extinction * (before - after);
    return float4(added, exp(-representativeDensity * OpticalDepth(farDistance)));
}

// The physical look: the air (Rayleigh, Mie, ozone) at the froxel's height, lit by the sun through
// the air above it and by the air's multiple scattering, and the height fog, lit by the sun and the
// sky's mean light; integrated over the slice as Frostbite does, (S - S e^-extinction x d) / extinction.
float4 PhysicalMedia(uint3 id, float visibility, out float4 fogOnly)
{
    float3 column = ColumnDirection(id.xy);
    float nearDepth = id.z == 0 ? 0 : SliceDepth(id.z - 1);
    float farDepth = SliceDepth(id.z);
    float columnLength = length(column);
    float3 viewDirection = column / columnLength;
    float length_ = (farDepth - nearDepth) * columnLength; // metres
    float3 middle = column * (nearDepth + farDepth) * 0.5;

    float4 sun = SkyParameters.Load(int3(SunTexel, 0, 0));
    float3 ambient = SkyParameters.Load(int3(AmbientTexel, 0, 0)).rgb;
    float4 fog = SkyParameters.Load(int3(FogTexel, 0, 0));
    float4 fogShape = SkyParameters.Load(int3(FogShapeTexel, 0, 0));
    float3 towardsSunWorld = SkyParameters.Load(int3(DirectionTexel, 0, 0)).xyz;

    float height = fogShape.z + dot(ToWorld(middle), float3(0, 1, 0)); // metres in the world
    float altitude = max(sun.a + (height - fogShape.z) * 0.001, 0);   // km above the ground
    Medium air = SampleMedium(altitude);
    float fogDensity = fog.a * min(exp(-(height - fogShape.z) / fogShape.x), 4);

    // Lit as the game lights the ground: from the sun pass's sun, at least 26 degrees up, which casts
    // the shadow map, so a low sun's glow never shows through the hills in front of it; the light is
    // the sky's sun's through the air, so it reddens and fades as the real sun sets.
    // The height fog also dims the sunlight that reaches the froxel: its optical depth towards the sun,
    // density x scale height / sin(elevation), the sine at least 0.01 (around the Earth's curve a 1 km
    // layer is about 100 km long at the horizon). Without it dense haze glowed white towards a low sun.
    float cosTheta = dot(normalize(m_lightParams[0].xyz), viewDirection);
    float hazeToSun = exp(-fogDensity * fogShape.x / max(towardsSunWorld.y, 0.01));
    float3 toSun = SunTransmittance(BottomRadius + altitude, towardsSunWorld.y) * hazeToSun * visibility;
    float3 multiple = MultipleScattering(BottomRadius + altitude, towardsSunWorld.y);
    // The game's cloud deck takes its share of the sun (in sun.rgb) and lights the air from all around.
    float3 deck = SkyParameters.Load(int3(OvercastTexel, 0, 0)).rgb;
    float3 airLight = sun.rgb * (toSun * (air.rayleigh * RayleighPhase(cosTheta) + air.mie * MiePhase(MiePhaseG, cosTheta))
        + multiple * air.scattering) + deck * air.scattering;
    float3 fogScattering = fog.rgb * fogDensity;
    float3 fogLight = fogScattering * (sun.rgb * toSun * MiePhase(fogShape.y, cosTheta) + ambient);

    // The air's coefficients are per km.
    float3 airExtinction = air.extinction * 0.001;
    float3 extinction = max(airExtinction + fogDensity, 1e-9);
    float3 transmittance = exp(-extinction * length_);
    float3 light = (airLight * 0.001 + fogLight) * (1 - transmittance) / extinction;

    float fogExtinction = max(fogDensity, 1e-9);
    float fogTransmittance = exp(-fogExtinction * length_);
    fogOnly = float4(fogLight * (1 - fogTransmittance) / fogExtinction, fogTransmittance);

    return float4(light, dot(transmittance, 1.0 / 3));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= TileSize)) return;

    float visibility = Shadowed ? BlendedVisibility(id) : 1;
    Visibility[id] = visibility;

    float4 fogOnly = 0;
    Media[id] = Physical ? PhysicalMedia(id, visibility, fogOnly) : GameMedia(id, visibility);
    FogOnly[id] = fogOnly;
}
