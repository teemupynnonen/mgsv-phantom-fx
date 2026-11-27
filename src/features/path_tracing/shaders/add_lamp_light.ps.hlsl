// The lamps' traced light (ray_tracing::lampLight) into both light targets, added as the game adds its
// lights' light.
Texture2D<float4> LampDiffuse : register(t0);
Texture2D<float4> LampSpecular : register(t1);

void main(float4 position : SV_Position, out float4 diffuse : SV_Target0, out float4 specular : SV_Target1)
{
    int3 pixel = int3(position.xy, 0);
    diffuse = float4(LampDiffuse.Load(pixel).rgb, 0);
    specular = float4(LampSpecular.Load(pixel).rgb, 0);
}
