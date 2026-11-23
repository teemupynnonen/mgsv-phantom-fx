#ifndef COLUMNS
#define COLUMNS 0
#endif
// One line of N complex pairs through the inverse FFT, in place: Stockham's radix-2 (Govindaraju et al.,
// "High Performance Discrete Fourier Transforms on Graphics Processors", 2008). COLUMNS takes the
// columns, otherwise the rows. A thread group a line and slice.
#include "sea_common.hlsli"

RWStructuredBuffer<float4> g_Spectrum : register(u0);

groupshared float4 g_Line[2][N];

// The index'th of a row or column (`strip`) in a slice.
uint IndexOf(uint strip, uint index, uint slice)
{
#if COLUMNS
    return SpectrumIndex(uint2(strip, index), slice);
#else
    return SpectrumIndex(uint2(index, strip), slice);
#endif
}

[numthreads(N / 2, 1, 1)]
void main(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    g_Line[0][thread] = g_Spectrum[IndexOf(group.x, thread, group.y)];
    g_Line[0][thread + N / 2] = g_Spectrum[IndexOf(group.x, thread + N / 2, group.y)];
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint stage = 0; stage < LOG_N; ++stage)
    {
        uint from = stage & 1, size = 1u << stage, within = thread % size;
        float angle = Pi * within / size;
        float2 twiddle = float2(cos(angle), sin(angle));
        float4 a = g_Line[from][thread];
        float4 b = g_Line[from][thread + N / 2];
        b = float4(Multiply(b.xy, twiddle), Multiply(b.zw, twiddle));
        uint to = (thread / size) * size * 2 + within;
        g_Line[1 - from][to] = a + b;
        g_Line[1 - from][to + size] = a - b;
        GroupMemoryBarrierWithGroupSync();
    }

    g_Spectrum[IndexOf(group.x, thread, group.y)] = g_Line[LOG_N & 1][thread];
    g_Spectrum[IndexOf(group.x, thread + N / 2, group.y)] = g_Line[LOG_N & 1][thread + N / 2];
}
