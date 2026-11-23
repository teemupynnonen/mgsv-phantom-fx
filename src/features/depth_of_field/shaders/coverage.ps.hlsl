// Each render pixel's own cover by the far layer, as DOF_ScatterCompositeFar works a half-size pixel's
// out: its blur radius from its depth (DOF_ScatterBakeFirst), then a ramp over the lower half of the
// sizes the blur takes. Drawn at DOF_ScatterBakeFirst, while the game's constants are bound.
cbuffer cPSScene : register(b2)
{
    float4 SceneRegisters[21];
    float4 ProjectionParam; // c21: z and w turn device depth into view depth
};
cbuffer cPSObject : register(b5)
{
    float4 ObjectRegisters[11];
    float4 CocLimits; // c11: z the largest far radius
    float4 CocSizes;  // c12: x and y the sizes the blur takes, z the radius's scale, w the focus distance
};
Texture2D<float> Depth : register(t1);

// x the game's cover; y the far radius in render pixels, the game's being in pixels of a 1920-wide image.
float2 main(float4 position : SV_Position) : SV_Target
{
    uint width, height;
    Depth.GetDimensions(width, height);

    const float depth = Depth.Load(int3(position.xy, 0));
    const float viewZ = ProjectionParam.z / (depth - ProjectionParam.w);
    const float radius = min(CocSizes.z * (viewZ - CocSizes.w) / viewZ, CocLimits.z);
    const float cover = saturate((radius - CocSizes.x) * 2 / max(CocSizes.y - CocSizes.x, 1e-4));

    // The game's blur starts at its smallest size; from there the radius is the lens's own.
    const float fadeIn = saturate((radius - CocSizes.x) / max(CocSizes.x * 0.5, 1e-4));

    return float2(cover, max(radius, 0) * fadeIn * width / 1920);
}
