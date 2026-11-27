// With the game's far blur. Where the two covers agree this is the game's blend.
#include "dof_composite.hlsli"

float4 main(float4 position : SV_Position) : SV_Target
{
    return Composited(position, Far.Sample(Linear, UvOf(position)), Coverage.Load(int3(position.xy, 0)).x);
}
