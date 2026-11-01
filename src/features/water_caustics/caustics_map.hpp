#pragma once

#include <cstdint>
#include <vector>

namespace phantom_fx::water_caustics
{
inline constexpr int CausticsMapSize = 256;

// A tiling web of light, R8, CausticsMapSize square: the cells a wave's lenses leave, their edges bright where the light
// gathers and brightest where three meet. Made rather than stored, so the add-on ships no image.
std::vector<std::uint8_t> makeCausticsMap();
}
