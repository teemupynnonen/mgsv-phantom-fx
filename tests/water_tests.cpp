#include "features/water_caustics/caustics_map.hpp"

#include <cstdlib>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>

// The caustics map is made on the processor; the shaders themselves are compiled at build time.
namespace
{
using namespace phantom_fx;

void expect(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

// How much `map` changes over a step between neighbouring rows, on average, and across its seam.
void causticsMapTiles()
{
    const auto map = water_caustics::makeCausticsMap();
    constexpr int Size = water_caustics::CausticsMapSize;
    expect(map.size() == Size * Size, "A map of its size.");

    const auto stepBetween = [&](int rowA, int rowB) {
        long long total = 0;
        for (int x = 0; x < Size; ++x) total += std::abs(int{map[rowA * Size + x]} - int{map[rowB * Size + x]});

        return static_cast<double>(total) / Size;
    };

    double inside = 0;
    for (int y = 0; y + 1 < Size; ++y) inside += stepBetween(y, y + 1);
    inside /= Size - 1;
    expect(stepBetween(Size - 1, 0) < 2 * inside, "The map wraps with no seam.");

    const auto mean = std::accumulate(map.begin(), map.end(), 0.0) / static_cast<double>(map.size());
    expect(mean > 30 && mean < 80, "Dark cells, with thin bright edges.");
}
}

int main()
{
    try
    {
        causticsMapTiles();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "Water tests passed.\n";
    return 0;
}
