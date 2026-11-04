#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

namespace phantom_fx
{
inline float halfToFloat(std::uint16_t h)
{
    const std::uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 31, mantissa = h & 1023;
    if (exponent == 0) return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mantissa), -24);
    if (exponent == 31) return std::bit_cast<float>(sign | 0x7F800000u | (mantissa << 13));
    return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
}
}
