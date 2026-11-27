// At a deferred resolve: its cPSMaterial c0, TppTonemap's (a, b, c).
cbuffer GameMaterial : register(b1)
{
    float4 GameMaterial[1];
};

RWStructuredBuffer<float4> OutCurve : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    OutCurve[0] = GameMaterial[0];
}
