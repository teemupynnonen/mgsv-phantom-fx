// GGX's specular for the surfaces path tracing's rays hit (docs/DEVELOPMENT.md, "Path tracing"), which have no
// game shader to follow. What the eye sees directly has the game's own specular table.
#ifndef GGX_SPECULAR_HLSLI
#define GGX_SPECULAR_HLSLI

// The specular light from `toLight` for the light `reaching` a surface, in the diffuse light's units (the
// game's irradiance over pi): the Trowbridge-Reitz distribution, Smith's height-correlated visibility and
// Schlick's Fresnel, of roughness squared and the specular colour at normal incidence.
float3 GgxSpecular(float3 reaching, float3 normal, float3 toEye, float3 toLight, float roughness, float3 specularColor)
{
    const float pi = 3.14159265f;
    float3 halfway = normalize(toEye + toLight);
    float NoL = saturate(dot(normal, toLight));
    float NoV = max(dot(normal, toEye), 1e-4f);
    float NoH = saturate(dot(normal, halfway));
    float VoH = saturate(dot(toEye, halfway));
    float alpha = max(roughness * roughness, 1e-3f);
    float a2 = alpha * alpha;
    float d = (NoH * a2 - NoH) * NoH + 1.0f;
    float distribution = a2 / (pi * d * d);
    float visibility = 0.5f / max(NoL * sqrt(NoV * NoV * (1.0f - a2) + a2) + NoV * sqrt(NoL * NoL * (1.0f - a2) + a2), 1e-6f);
    float3 fresnel = specularColor + (1.0f - specularColor) * pow(1.0f - VoH, 5.0f);
    return pi * reaching * (NoL * distribution * visibility) * fresnel;
}

#endif
