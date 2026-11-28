// The clouds' 3D noise: one cube of fbm, baked on the CPU into the volume texture the shaders sample (clouds.cpp). The preview
// tool (tools/noise-preview) bakes the same field, so a shape can be judged without the game.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace phantom_fx::cloud_noise
{
inline constexpr int Size = 64;
// The shipping cube's middle, before the contrast gain: four octaves of value noise, periods 4 to 32.
inline constexpr float Median = 0.63f;

inline std::uint32_t hash3(int x, int y, int z)
{
    std::uint32_t v = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u
        + static_cast<std::uint32_t>(z) * 2147483647u;
    v = (v ^ (v >> 13)) * 1274126177u;
    return v ^ (v >> 16);
}

inline int wrapped(int v, int period) { return ((v % period) + period) % period; }

// Value noise: a random number at every lattice corner, blended smoothly. Its lattice is a cubic grid, which is what shows as
// straight lines when the layer is thin and seen at a grazing angle; the recipes turn or warp around it.
inline float lattice(int x, int y, int z, int period)
{
    return static_cast<float>(hash3(wrapped(x, period), wrapped(y, period), wrapped(z, period)) & 0xff) / 255.0f;
}

inline float valueNoise(float x, float y, float z, int period)
{
    const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y)), zi = static_cast<int>(std::floor(z));
    const float fx = x - xi, fy = y - yi, fz = z - zi;
    const auto smooth = [](float t) { return t * t * (3 - 2 * t); };
    const float u = smooth(fx), v = smooth(fy), w = smooth(fz);
    const auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    const float c000 = lattice(xi, yi, zi, period), c100 = lattice(xi + 1, yi, zi, period);
    const float c010 = lattice(xi, yi + 1, zi, period), c110 = lattice(xi + 1, yi + 1, zi, period);
    const float c001 = lattice(xi, yi, zi + 1, period), c101 = lattice(xi + 1, yi, zi + 1, period);
    const float c011 = lattice(xi, yi + 1, zi + 1, period), c111 = lattice(xi + 1, yi + 1, zi + 1, period);
    return mix(mix(mix(c000, c100, u), mix(c010, c110, u), v), mix(mix(c001, c101, u), mix(c011, c111, u), v), w);
}

// Gradient (Perlin) noise: a random direction at every corner, the dot product with the offset to it, blended. Its features are
// smoother than value noise's and sit at no single height, and it has no bias towards the lattice points.
inline float gradientNoise(float x, float y, float z, int period)
{
    const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y)), zi = static_cast<int>(std::floor(z));
    const float fx = x - xi, fy = y - yi, fz = z - zi;
    const auto smooth = [](float t) { return t * t * (3 - 2 * t); };
    const float u = smooth(fx), v = smooth(fy), w = smooth(fz);
    const auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    const auto corner = [period](int cx, int cy, int cz, float dx, float dy, float dz)
    {
        const std::uint32_t h = hash3(wrapped(cx, period), wrapped(cy, period), wrapped(cz, period)) % 12;
        // Perlin's twelve directions, to the middles of the cube's edges: each axis is left out by four of them, so no axis varies
        // less than another. A set that left only z out stretched every feature along z, which a deck seen edge-on turned into
        // streaks running to the horizon.
        const float a = (h & 1) ? -1.f : 1.f, b = (h & 2) ? -1.f : 1.f;
        const float d = h < 4 ? a * dx + b * dy : h < 8 ? a * dx + b * dz : a * dy + b * dz;
        return d * 0.7071f;
    };
    const float c000 = corner(xi, yi, zi, fx, fy, fz), c100 = corner(xi + 1, yi, zi, fx - 1, fy, fz);
    const float c010 = corner(xi, yi + 1, zi, fx, fy - 1, fz), c110 = corner(xi + 1, yi + 1, zi, fx - 1, fy - 1, fz);
    const float c001 = corner(xi, yi, zi + 1, fx, fy, fz - 1), c101 = corner(xi + 1, yi, zi + 1, fx - 1, fy, fz - 1);
    const float c011 = corner(xi, yi + 1, zi + 1, fx, fy - 1, fz - 1), c111 = corner(xi + 1, yi + 1, zi + 1, fx - 1, fy - 1, fz - 1);
    // Perlin's own range is about +-0.7; the bake measures its middle anyway.
    return 0.5f + mix(mix(mix(c000, c100, u), mix(c010, c110, u), v), mix(mix(c001, c101, u), mix(c011, c111, u), v), w);
}

// Cellular (Worley) noise: one random point per cell, the distance to the nearest, 1 at the point and 0 at the cell's edge. Gives
// rounded billows with no lattice plane, at the cost of the points reading as beads when the cells are small.
inline float cellularNoise(float x, float y, float z, int period)
{
    const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y)), zi = static_cast<int>(std::floor(z));
    float nearest = 1e9f;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
            {
                const int cx = xi + dx, cy = yi + dy, cz = zi + dz;
                const std::uint32_t h = hash3(wrapped(cx, period), wrapped(cy, period), wrapped(cz, period));
                const float ox = static_cast<float>(h & 0xff) / 255, oy = static_cast<float>((h >> 8) & 0xff) / 255,
                    oz = static_cast<float>((h >> 16) & 0xff) / 255;
                const float px = cx + ox - x, py = cy + oy - y, pz = cz + oz - z;
                nearest = std::min(nearest, px * px + py * py + pz * pz);
            }
    return std::sqrt(std::min(nearest, 1.0f));
}

enum class Generator
{
    Value,
    Gradient,
    Cellular,
};

struct Recipe
{
    Generator generator = Generator::Value;
    int size = Size;
    int octaves = 4; // periods 4, 8, 16, ...
    float gain = 6.0f; // contrast about the middle
    // The field's middle before the gain. NaN measures it off the baked field, which is what a generator that is not the
    // shipping value noise wants.
    float median = Median;
    // Domain warp of the shape lookup, in cube units (0.05 is a twentieth of the repeat). 0 leaves the lattice's planes intact.
    float warp = 0.0f;
};

inline std::vector<std::uint8_t> bake(const Recipe& recipe)
{
    const int size = std::max(4, recipe.size);
    std::vector<float> field(static_cast<std::size_t>(size) * size * size);
    const int warpPeriod = 4;
    for (int z = 0; z < size; ++z)
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                float u = static_cast<float>(x) / size, v = static_cast<float>(y) / size, w = static_cast<float>(z) / size;
                if (recipe.warp > 0)
                {
                    // The warp is itself periodic in the cube, so it pushes the shape around without opening a seam at the cube's edge.
                    const float offset = recipe.warp;
                    u += (valueNoise(u * warpPeriod, v * warpPeriod, w * warpPeriod, warpPeriod) - 0.5f) * offset;
                    v += (valueNoise(u * warpPeriod + 7.3f, v * warpPeriod + 7.3f, w * warpPeriod + 7.3f, warpPeriod) - 0.5f) * offset;
                    w += (valueNoise(u * warpPeriod + 19.1f, v * warpPeriod + 19.1f, w * warpPeriod + 19.1f, warpPeriod) - 0.5f) * offset;
                }
                float n = 0, amplitude = 0.5f, total = 0;
                for (int octave = 0, period = 4; octave < recipe.octaves; ++octave, period *= 2)
                {
                    float value = 0;
                    switch (recipe.generator)
                    {
                    case Generator::Value: value = valueNoise(u * period, v * period, w * period, period); break;
                    case Generator::Gradient: value = gradientNoise(u * period, v * period, w * period, period); break;
                    case Generator::Cellular: value = 1 - cellularNoise(u * period, v * period, w * period, period); break;
                    }
                    n += value * amplitude;
                    total += amplitude;
                    amplitude *= 0.5f;
                }
                n /= total;
                n = std::clamp(n, 0.0f, 1.0f);
                // Billows: the first octave's rounded side turned into the cloud's cauliflower.
                field[(static_cast<std::size_t>(z) * size + y) * size + x] = std::clamp(0.75f * n + 0.25f * (1 - std::fabs(2 * n - 1)), 0.0f, 1.0f);
            }

    float median = recipe.median;
    float spread = 1.0f;
    if (std::isnan(median))
    {
        // A generator's own middle and its 10-to-90 spread: value noise's spread is the one the contrast was tuned on, so every
        // generator is brought to the same contrast and they can be compared on shape alone.
        std::vector<float> sorted = field;
        const auto tenth = sorted.begin() + sorted.size() / 10;
        const auto middle = sorted.begin() + sorted.size() / 2;
        const auto ninetieth = sorted.begin() + sorted.size() * 9 / 10;
        std::nth_element(sorted.begin(), tenth, sorted.end());
        std::nth_element(sorted.begin(), middle, sorted.end());
        std::nth_element(sorted.begin(), ninetieth, sorted.end());
        median = *middle;
        spread = 0.212f / std::max(*ninetieth - *tenth, 0.01f);
    }

    std::vector<std::uint8_t> voxels(field.size());
    for (std::size_t i = 0; i < field.size(); ++i)
        voxels[i] = static_cast<std::uint8_t>(std::clamp((field[i] - median) * spread * recipe.gain + 0.5f, 0.0f, 1.0f) * 255);
    return voxels;
}

// The cube's mip chain, each level the mean of eight voxels of the one above, wrapping like the sampler. A far sample reads the level
// its pixel and its step cover: the full-detail cube there gives each pixel a different speck of it, which is grain.
inline std::vector<std::vector<std::uint8_t>> mips(const std::vector<std::uint8_t>& top, int size)
{
    std::vector<std::vector<std::uint8_t>> levels{top};
    for (int from = size; from > 1; from /= 2)
    {
        const std::vector<std::uint8_t>& above = levels.back();
        const int to = from / 2;
        std::vector<std::uint8_t> level(static_cast<std::size_t>(to) * to * to);
        for (int z = 0; z < to; ++z)
            for (int y = 0; y < to; ++y)
                for (int x = 0; x < to; ++x)
                {
                    int sum = 0;
                    for (int k = 0; k < 8; ++k)
                        sum += above[(static_cast<std::size_t>(2 * z + (k >> 2)) * from + 2 * y + ((k >> 1) & 1)) * from + 2 * x + (k & 1)];
                    level[(static_cast<std::size_t>(z) * to + y) * to + x] = static_cast<std::uint8_t>((sum + 4) / 8);
                }
        levels.push_back(std::move(level));
    }
    return levels;
}
} // namespace phantom_fx::cloud_noise
