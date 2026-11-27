// On the light's far faces, the light's shadow into both light targets' alpha, for the light's blend to
// scale it by; where the surface lies past them, none of its light. The full-screen triangle, at depth 0
// (as far as can be), lies past every surface.
cbuffer LightPass : register(b0)
{
    float4 ViewToUv;
    uint4 Shadow;
};

Texture2D<float> Share : register(t0); // The share of the traced lights' light that reaches
Texture2D<float> Depth : register(t1); // Reversed: the nearer, the greater

void main(float4 position : SV_Position, out float4 diffuse : SV_Target0, out float4 specular : SV_Target1)
{
    int3 pixel = int3(position.xy, 0);
    float lit = Shadow.y ? Share.Load(pixel) : 1.0;
    diffuse = specular = float4(0.0, 0.0, 0.0, position.z <= Depth.Load(pixel) ? lit : 0.0);
}
