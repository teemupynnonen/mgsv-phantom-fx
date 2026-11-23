// What the surfaces' shaders write into the surface targets.
struct Surface
{
    float depth : SV_Target0;
    float4 normal : SV_Target1; // World space, the roughness in w
    float4 weight : SV_Target2; // rgb: how much of what it reflects reaches the eye; a: how much it covers what is behind
};

