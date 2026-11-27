// The constant buffer of the march's compute shaders, and of the global
// illumination's combine; each reads the fields it needs.
cbuffer Parameters : register(b0)
{
    uint2 Size;              // Render size, the depth buffer's
    float Radius;            // Metres
    float MaxRadiusPixels;   // Caps the march for surfaces close to the camera
    float OcclusionStrength; // GTVBAO: exponent on the visibility
    float LightStrength;     // The combine: multiplies the light
    float DiffuseScale;      // The combine: 0 leaves the game's diffuse light out, for the diagnostics
    float Padding;
};

