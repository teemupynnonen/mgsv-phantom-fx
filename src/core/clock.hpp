#pragma once

#include <Windows.h>

// QueryPerformanceCounter's ticks, and the milliseconds between them.
namespace phantom_fx::clock
{
inline LONGLONG ticksNow()
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

inline double millisecondsOf(LONGLONG ticks)
{
    // The frequency is fixed at boot, so it is read once.
    static const double ticksPerSecond = []
    {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return static_cast<double>(frequency.QuadPart);
    }();

    return static_cast<double>(ticks) * 1000.0 / ticksPerSecond;
}
}
