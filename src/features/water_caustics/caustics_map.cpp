#include "features/water_caustics/caustics_map.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace phantom_fx::water_caustics
{
namespace
{
constexpr int Cells = 7; // Across the tile
constexpr auto Tau = 2 * std::numbers::pi_v<float>;

struct Cell
{
    float x, y, brightness;
};

float randomOf(int x, int y, int seed)
{
    auto hash = static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u ^ static_cast<std::uint32_t>(seed) * 83492791u;
    hash *= 2654435761u;
    hash ^= hash >> 15;
    hash *= 2246822519u;
    hash ^= hash >> 13;

    return static_cast<float>(hash) / 4294967296.0f;
}

std::array<Cell, Cells * Cells> makeCells()
{
    std::array<Cell, Cells * Cells> cells{};
    for (int y = 0; y < Cells; ++y)
        for (int x = 0; x < Cells; ++x)
            cells[y * Cells + x] = {static_cast<float>(x) + randomOf(x, y, 1), static_cast<float>(y) + randomOf(x, y, 2), randomOf(x, y, 3)};

    return cells;
}

// The shortest way round the tile.
float wrapped(float distance)
{
    return distance - Cells * std::round(distance / Cells);
}

// The web at a point of the tile, in cells. Light gathers along the edges between two cells and most where three meet;
// each cell's brightness keeps them from reading as a net.
float webAt(const std::array<Cell, Cells * Cells>& cells, float x, float y)
{
    std::array<float, 3> nearest{1e6f, 1e6f, 1e6f};
    std::array<float, 2> brightness{};
    for (const auto& cell : cells)
    {
        const auto distance = std::hypot(wrapped(x - cell.x), wrapped(y - cell.y));
        if (distance < nearest[0])
        {
            nearest = {distance, nearest[0], nearest[1]};
            brightness = {cell.brightness, brightness[0]};
        }
        else if (distance < nearest[1])
        {
            nearest = {nearest[0], distance, nearest[1]};
            brightness[1] = cell.brightness;
        }
        else if (distance < nearest[2]) nearest[2] = distance;
    }

    const auto edge = nearest[1] - nearest[0];
    const auto bright = 0.25f + 0.375f * (brightness[0] + brightness[1]);

    return 0.07f + 0.17f * std::exp(-edge / 0.32f) + 0.34f * bright * std::exp(-edge / 0.055f)
        + 0.6f * bright * bright * std::exp(-(nearest[2] - nearest[0]) / 0.085f);
}
}

std::vector<std::uint8_t> makeCausticsMap()
{
    const auto cells = makeCells();

    std::vector<std::uint8_t> map(CausticsMapSize * CausticsMapSize);
    for (int y = 0; y < CausticsMapSize; ++y)
        for (int x = 0; x < CausticsMapSize; ++x)
        {
            const auto u = (static_cast<float>(x) + 0.5f) / CausticsMapSize, v = (static_cast<float>(y) + 0.5f) / CausticsMapSize;
            // Whole waves across the tile bend the edges, and keep it tiling.
            const auto bentU = u + 0.016f * std::sin(Tau * (2 * v + u)) + 0.012f * std::sin(Tau * (4 * v - 3 * u) + 1.3f);
            const auto bentV = v + 0.016f * std::sin(Tau * (2 * u - v) + 2.1f) + 0.012f * std::sin(Tau * (4 * u + 3 * v) + 0.7f);
            const auto web = webAt(cells, bentU * Cells, bentV * Cells);
            map[y * CausticsMapSize + x] = static_cast<std::uint8_t>(std::clamp(web, 0.0f, 1.0f) * 255 + 0.5f);
        }

    return map;
}
}
