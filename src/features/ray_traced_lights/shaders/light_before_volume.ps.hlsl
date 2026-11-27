// On the light's near faces: a surface short of them is outside its volume, so none of its light.
Texture2D<float> Depth : register(t1);

void main(float4 position : SV_Position, out float4 diffuse : SV_Target0, out float4 specular : SV_Target1)
{
    if (position.z >= Depth.Load(int3(position.xy, 0))) discard;
    diffuse = specular = 0.0;
}
