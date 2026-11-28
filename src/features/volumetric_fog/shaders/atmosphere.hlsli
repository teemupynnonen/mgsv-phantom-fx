// The physical atmosphere is ported from Sébastien Hillaire's reference for "A Scalable and Production
// Ready Sky and Atmosphere Rendering Technique" (Epic Games, 2020, MIT; its license is in
// licenses/UnrealEngineSkyAtmosphere-LICENSE.txt).
// Earth's atmosphere as Hillaire's reference sets it up, in km, and its lookup tables. The
// atmosphere's frame has z up with the planet's centre at the origin; the game's world has y up.
#ifndef LINEAR_CLAMP_SAMPLER
#define LINEAR_CLAMP_SAMPLER s0
#endif
SamplerState LinearClamp : register(LINEAR_CLAMP_SAMPLER);
Texture2D<float4> TransmittanceLut : register(t4);       // 256x64: transmittance to the atmosphere's top
Texture2D<float4> MultipleScatteringLut : register(t5);  // 32x32: light scattered more than once, per unit of sunlight
Texture2D<float4> SkyParameters : register(t6);          // SkyParameters' output, see there
Texture2D<float4> SkyViewLut : register(t7);             // 192x108: the sky around the camera, per unit of sunlight

static const float Pi = 3.14159265;
static const float BottomRadius = 6360, TopRadius = 6460;
static const float3 RayleighScattering = float3(0.005802, 0.013558, 0.033100);
static const float RayleighExpScale = -1.0 / 8.0;
static const float3 MieScattering = 0.003996, MieExtinction = 0.004440;
static const float MieExpScale = -1.0 / 1.2;
static const float MiePhaseG = 0.8;
static const float3 OzoneExtinction = float3(0.000650, 0.001881, 0.000085);
static const float PlanetRadiusOffset = 0.01;
// Afghanistan's and Africa's ground: sand, rock and dry grass reflect about this much.
static const float GroundAlbedo = 0.3;
static const float2 TransmittanceSize = float2(256, 64);
static const float MultipleScatteringSize = 32;
static const float2 SkyViewSize = float2(192, 108);

// SkyParameters' texels.
static const int SunTexel = 0;      // rgb the sunlight at the atmosphere's top, in the game's exposed units; a the camera's height, km
static const int DirectionTexel = 1; // xyz towards the sun, in the world
static const int AmbientTexel = 2;   // rgb the sky's mean light, for the fog
static const int FogTexel = 3;       // rgb the height fog's albedo, a its extinction at the camera, 1/m
static const int FogShapeTexel = 4;  // x its height scale, m; y its phase's g; z the camera's height in the world, m
static const int SourceTexel = 5;    // For the log: x the game's sunlight on the ground, y its sky's, z the air's transmittance to the sun
static const int OvercastTexel = 6;  // rgb the light of the game's cloud deck over the sky, a how much of the sky it covers
static const int CloudSunTexel = 7;  // rgb the sunlight at the cloud deck, through the air to it (clouds.hlsl)
static const int GroundSunTexel = 8;  // xyz towards the sun the ground is lit by (never below 26 degrees up), for the clouds' shadow (cloud_shadow.hlsl)
static const uint SkyParameterTexels = 9;

float3 ToAtmosphere(float3 world)
{
    return float3(world.x, world.z, world.y);
}

float RaySphereIntersectNearest(float3 origin, float3 direction, float3 centre, float radius)
{
    float a = dot(direction, direction);
    float3 offset = origin - centre;
    float b = 2 * dot(direction, offset);
    float c = dot(offset, offset) - radius * radius;
    float delta = b * b - 4 * a * c;
    if (delta < 0 || a == 0) return -1;

    float near = (-b - sqrt(delta)) / (2 * a), far = (-b + sqrt(delta)) / (2 * a);
    if (near < 0 && far < 0) return -1;
    if (near < 0) return max(0, far);
    if (far < 0) return max(0, near);

    return max(0, min(near, far));
}

float FromUnitToSubUvs(float u, float resolution) { return (u + 0.5 / resolution) * (resolution / (resolution + 1)); }
float FromSubUvsToUnit(float u, float resolution) { return (u - 0.5 / resolution) * (resolution / (resolution - 1)); }

float2 TransmittanceUv(float viewHeight, float viewZenithCos)
{
    float H = sqrt(max(0, TopRadius * TopRadius - BottomRadius * BottomRadius));
    float rho = sqrt(max(0, viewHeight * viewHeight - BottomRadius * BottomRadius));
    float discriminant = viewHeight * viewHeight * (viewZenithCos * viewZenithCos - 1) + TopRadius * TopRadius;
    float d = max(0, -viewHeight * viewZenithCos + sqrt(discriminant));
    float dMin = TopRadius - viewHeight, dMax = rho + H;

    return float2((d - dMin) / (dMax - dMin), rho / H);
}

float3 TransmittanceToTop(float viewHeight, float viewZenithCos)
{
    return TransmittanceLut.SampleLevel(LinearClamp, TransmittanceUv(viewHeight, viewZenithCos), 0).rgb;
}

// The sunlight's share through the air at a height: none once the planet hides the sun.
float3 SunTransmittance(float viewHeight, float sunZenithCos)
{
    float horizonCos = -sqrt(max(1 - BottomRadius * BottomRadius / (viewHeight * viewHeight), 0));
    return sunZenithCos >= horizonCos ? TransmittanceToTop(viewHeight, sunZenithCos) : 0;
}

float3 MultipleScattering(float viewHeight, float sunZenithCos)
{
    float2 uv = saturate(float2(sunZenithCos * 0.5 + 0.5, (viewHeight - BottomRadius) / (TopRadius - BottomRadius)));
    uv = float2(FromUnitToSubUvs(uv.x, MultipleScatteringSize), FromUnitToSubUvs(uv.y, MultipleScatteringSize));
    return MultipleScatteringLut.SampleLevel(LinearClamp, uv, 0).rgb;
}

float2 SkyViewUv(bool intersectGround, float viewZenithCos, float lightViewCos, float viewHeight)
{
    float horizon = sqrt(viewHeight * viewHeight - BottomRadius * BottomRadius);
    float beta = acos(horizon / viewHeight);
    float zenithHorizonAngle = Pi - beta;
    float2 uv;
    if (!intersectGround)
    {
        float coord = 1 - sqrt(1 - acos(viewZenithCos) / zenithHorizonAngle);
        uv.y = coord * 0.5;
    }
    else
    {
        float coord = sqrt((acos(viewZenithCos) - zenithHorizonAngle) / beta);
        uv.y = coord * 0.5 + 0.5;
    }
    uv.x = sqrt(-lightViewCos * 0.5 + 0.5);

    return float2(FromUnitToSubUvs(uv.x, SkyViewSize.x), FromUnitToSubUvs(uv.y, SkyViewSize.y));
}

// The sky towards `direction` from the camera, both in the world, per unit of sunlight. Below the
// horizon it is the horizon, as the game's sky does: the game's ground and sea cover the planet there,
// and where they stop short of it (the sea at Mother Base) the planet's dark air showed as a band.
float3 SkyLight(float3 direction, float3 towardsSun, float cameraHeight)
{
    float3 d = ToAtmosphere(direction), sun = ToAtmosphere(towardsSun);
    float viewHeight = BottomRadius + cameraHeight;
    float3 position = float3(0, 0, viewHeight);
    float3 side = cross(float3(0, 0, 1), d);
    side = dot(side, side) > 1e-8 ? normalize(side) : float3(1, 0, 0);
    float3 forward = normalize(cross(side, float3(0, 0, 1)));
    float2 sunOnPlane = float2(dot(sun, forward), dot(sun, side));
    float lightViewCos = dot(sunOnPlane, sunOnPlane) > 1e-8 ? normalize(sunOnPlane).x : 1;
    bool intersectGround = RaySphereIntersectNearest(position, d, 0, BottomRadius) >= 0;
    float2 uv = SkyViewUv(intersectGround, d.z, lightViewCos, viewHeight);
    // The table's upper half is the sky, its last row the horizon.
    uv.y = min(uv.y, (SkyViewSize.y / 2 - 0.5) / SkyViewSize.y);

    return SkyViewLut.SampleLevel(LinearClamp, uv, 0).rgb;
}

struct Medium
{
    float3 scattering, extinction, rayleigh, mie;
};

Medium SampleMedium(float height) // km above the ground
{
    float rayleigh = exp(RayleighExpScale * height), mie = exp(MieExpScale * height);
    float ozone = saturate(height < 25 ? height / 15 - 2.0 / 3 : -height / 15 + 8.0 / 3);
    Medium medium;
    medium.rayleigh = rayleigh * RayleighScattering;
    medium.mie = mie * MieScattering;
    medium.scattering = medium.rayleigh + medium.mie;
    medium.extinction = medium.rayleigh + mie * MieExtinction + ozone * OzoneExtinction;

    return medium;
}

float RayleighPhase(float cosTheta)
{
    return 3 / (16 * Pi) * (1 + cosTheta * cosTheta);
}

// Cornette-Shanks; cosTheta between the way towards the light and the way the eye looks.
float MiePhase(float g, float cosTheta)
{
    float k = 3 / (8 * Pi) * (1 - g * g) / (2 + g * g);
    return k * (1 + cosTheta * cosTheta) / pow(max(1 + g * g - 2 * g * cosTheta, 1e-6), 1.5);
}

struct Scattering
{
    float3 light, multipleAsOne;
};

// Hillaire's IntegrateScatteredLuminance, per unit of sunlight; with `ground`, light the ground
// reflects where the ray meets it.
Scattering Integrate(float3 position, float3 direction, float3 towardsSun, float samples, bool variableSamples, bool phases, bool multiple,
    bool ground)
{
    Scattering result = (Scattering)0;
    float tBottom = RaySphereIntersectNearest(position, direction, 0, BottomRadius);
    float tTop = RaySphereIntersectNearest(position, direction, 0, TopRadius);
    float tMax = 0;
    if (tBottom < 0)
    {
        if (tTop < 0) return result;
        tMax = tTop;
    }
    else if (tTop > 0)
        tMax = min(tTop, tBottom);

    float count = samples, countFloor = samples, tMaxFloor = tMax;
    if (variableSamples)
    {
        count = lerp(4, 14, saturate(tMax * 0.01));
        countFloor = floor(count);
        tMaxFloor = tMax * countFloor / count;
    }

    float cosTheta = dot(towardsSun, direction);
    float miePhase = MiePhase(MiePhaseG, cosTheta), rayleighPhase = RayleighPhase(cosTheta);
    float3 light = 0, throughput = 1;
    float t = 0;
    for (float s = 0; s < count; s += 1)
    {
        float dt;
        if (variableSamples)
        {
            float t0 = s / countFloor, t1 = (s + 1) / countFloor;
            t0 = tMaxFloor * t0 * t0;
            t1 = t1 * t1 > 1 ? tMax : tMaxFloor * t1 * t1;
            t = t0 + (t1 - t0) * 0.3;
            dt = t1 - t0;
        }
        else
        {
            float next = tMax * (s + 0.3) / count;
            dt = next - t;
            t = next;
        }

        float3 p = position + t * direction;
        float height = length(p);
        float3 up = p / height;
        Medium medium = SampleMedium(height - BottomRadius);
        float3 sampleTransmittance = exp(-medium.extinction * dt);
        float sunZenithCos = dot(towardsSun, up);
        float3 toSun = TransmittanceToTop(height, sunZenithCos);
        float3 phaseScattering = phases ? medium.mie * miePhase + medium.rayleigh * rayleighPhase : medium.scattering / (4 * Pi);
        float earthShadow = RaySphereIntersectNearest(p, towardsSun, PlanetRadiusOffset * up, BottomRadius) >= 0 ? 0 : 1;
        float3 multipleLight = multiple ? MultipleScattering(height, sunZenithCos) : 0;

        float3 inScattered = earthShadow * toSun * phaseScattering + multipleLight * medium.scattering;
        result.multipleAsOne += throughput * (medium.scattering - medium.scattering * sampleTransmittance) / medium.extinction;
        light += throughput * (inScattered - inScattered * sampleTransmittance) / medium.extinction;
        throughput *= sampleTransmittance;
    }

    if (ground && tMax == tBottom && tBottom > 0)
    {
        float3 p = position + tBottom * direction;
        float3 up = normalize(p);
        light += TransmittanceToTop(length(p), dot(towardsSun, up)) * throughput * saturate(dot(up, towardsSun)) * GroundAlbedo / Pi;
    }

    result.light = light;
    return result;
}

