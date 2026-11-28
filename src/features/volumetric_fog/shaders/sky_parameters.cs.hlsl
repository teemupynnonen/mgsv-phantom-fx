// One thread: what the sky, the physical fog and the sky draw share this frame, from the sun pass's
// constants and the game's fog pass's.
#include "fog_inputs.hlsli"
#include "atmosphere.hlsli"

cbuffer SkyMaterial : register(b8) // cPSMaterial of the last sky draw: [3] towards the sun in the world, and the sun its sky is lit by
{
    float4 SkyMaterial[8];
};

cbuffer SkyObject : register(b9) // cPSObject of the last sky draw: [10].x how overcast it draws the sky
{
    float4 SkyObject[13];
};

RWTexture2D<float4> Output : register(u0);

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// The game's fog has Schlick's phase; its k stands for the g of Henyey-Greenstein's that solves
// k = 1.55 g - 0.55 g^3 (k 0.96 is g 0.8). Taken as g it narrows the sun's glow to a disc.
float SchlickToG(float k)
{
    k = clamp(k, 0, 0.99);
    float g = 0.5;
    [unroll] for (int step = 0; step < 6; ++step) g -= (1.55 * g - 0.55 * g * g * g - k) / (1.55 - 1.65 * g * g);

    return saturate(g);
}

[numthreads(1, 1, 1)]
void main()
{
    // The game lights the ground with the sun at least 26 degrees up, and its sky with the sun where it
    // is, brighter as it sets to make up for the longer way through the air: the sky's direction and
    // light belong together.
    float3 towardsSun = SkySun ? normalize(SkyMaterial[3].xyz) : normalize(ToWorld(m_lightParams[0].xyz));
    float cameraHeight = max(m_eyepos.y * 0.001, 0.001);
    // The sun above the air is the ground's sunlight taken back through it: the game lights a surface
    // with its albedo times that light, where a physical one takes albedo / pi. Not the game's sky sun
    // (logged for comparison): it is about four times the ground's at noon and grows as the sun sets,
    // standing in for the air the atmosphere here already has, so it blew the sky out to white.
    float3 transmittance = TransmittanceToTop(BottomRadius + cameraHeight, towardsSun.y);
    float groundSunlight = Luminance(m_lightParams[1].xyz * m_exposure.z);
    float skySunlight = min(SkyMaterial[3].w * m_exposure.z, 1000);
    float sunlight = Pi * groundSunlight / max(Luminance(transmittance), 0.002);

    // The game's weather draws a grey cloud deck over its sky (its sky draws' "cloudy" share: 0 on a
    // clear day, 1 in rain), and lights the ground under it with almost no sun. The deck takes that
    // share of the blue sky and of the sun on the air and the fog, and lights them itself, with what
    // the game's fog adds up to far away: (sun x mean phase x betaM + sky x betaU) / extinction, the
    // game's own light for that weather (in rain at dusk about 0.18 grey, as its sky draws).
    float overcast = SkySun ? saturate(SkyObject[10].x) : 0;
    float3 betaM = max(FogMedium[2].xyz, 0), betaR = max(FogMedium[3].xyz, 0);
    float betaU = max(FogMedium[3].w, 0), mieRatio = FogMedium[0].y;
    float meanMiePhase = (1 - mieRatio) / (4 * Pi) + mieRatio;
    float3 deckLight = (FogMedium[0].w * (meanMiePhase * betaM + betaR / (4 * Pi)) + FogMedium[1].xyz * betaU)
        / max(betaM + betaR + betaU, 1e-6) + max(FogMedium[7].xyz, 0);

    // The game's fog thins the view to e^(-density x od(1 km)) over a kilometre, a clear day in
    // Afghanistan to a fifth: it fades the distant ground, and the game's sky leaves most of it out.
    // As a medium that thick it would grey the whole sky, so at no haze the height fog takes a tenth
    // of it on a clear day, rising to all of it at ten times that (fog, sandstorms), less what the
    // air does. Haze raises it to all of it in any weather, in a layer as deep as dust (1 km): the
    // game's haze does not thin with height, and real dust pales the sky. Under the cloud deck it is
    // rain and mist, which the air does not make: all of it too.
    float opticalDepth = exp2(log2(max(1000 - FogMedium[0].z, 0)) * FogMedium[1].w);
    float gameExtinction = FogMedium[0].x * opticalDepth / 1000;
    float share = lerp(min(gameExtinction / 0.015, 1), 1, max(saturate(Haze), overcast));
    float fogExtinction = max(gameExtinction * share - 0.000015, 0);
    float fogScaleHeight = lerp(300, 1000, saturate(Haze));
    float3 albedo = 0.9 * betaM / max(max(betaM.r, max(betaM.g, betaM.b)), 1e-6);
    if (all(betaM <= 0)) albedo = 0.9;

    Output[uint2(SunTexel, 0)] = float4((sunlight * (1 - overcast)).xxx, cameraHeight);
    Output[uint2(DirectionTexel, 0)] = float4(towardsSun, 0);
    Output[uint2(FogTexel, 0)] = float4(albedo, fogExtinction);
    Output[uint2(FogShapeTexel, 0)] = float4(fogScaleHeight, SchlickToG(FogMedium[2].w), m_eyepos.y, 0);
    Output[uint2(SourceTexel, 0)] = float4(groundSunlight, SkySun ? skySunlight : 0, Luminance(transmittance), gameExtinction);
    Output[uint2(OvercastTexel, 0)] = float4(deckLight * overcast, overcast);
    // The clouds' sun: the same sunlight, through the air down to the deck's middle (2 km), so a cloud reddens with the sky at dusk.
    // a is 1: the clouds tell this texture from an empty slot by it.
    float3 deckTransmittance = TransmittanceToTop(BottomRadius + 2, towardsSun.y);
    Output[uint2(CloudSunTexel, 0)] = float4(sunlight * (1 - overcast) * deckTransmittance, 1);
    Output[uint2(GroundSunTexel, 0)] = float4(normalize(ToWorld(m_lightParams[0].xyz)), 0);
}
