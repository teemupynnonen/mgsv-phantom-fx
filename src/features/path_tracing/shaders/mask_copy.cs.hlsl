// The mask atlas's size, the same as gi_shared.h's GI_LAMP_MASK_TILES and GI_LAMP_MASK_SIZE (fxc cannot include
// that header; the CMake configure fails if they differ).
#define GI_LAMP_MASK_TILES 8
#define GI_LAMP_MASK_SIZE 256
// A masked spot's mask (the game's t4) into its tile of the atlas the lamps' rays sample (direct_light.hlsl),
// sampled as the game samples it (bilinear, wrapped) at the tile's texels.
Texture2D<float4> Mask : register(t0);
SamplerState LinearWrap : register(s0);
RWTexture2D<unorm float4> Atlas : register(u0);

cbuffer Parameters : register(b0)
{
    uint Tile;
    uint3 Padding;
};

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= GI_LAMP_MASK_SIZE)) return;

    Atlas[uint2(Tile * GI_LAMP_MASK_SIZE + id.x, id.y)] = Mask.SampleLevel(LinearWrap, (id + 0.5) / GI_LAMP_MASK_SIZE, 0);
}
