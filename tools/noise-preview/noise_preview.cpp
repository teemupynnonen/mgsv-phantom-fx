// Judges the clouds' noise without the game: bakes the same cube the add-on ships (cloud_noise.hpp), then draws it as a vertical
// slice, a top-down map, or a horizon composite ray-marched through the layer, each as a PNG.
//
//   cl /std:c++20 /O2 /EHsc /I src tools/noise-preview/noise_preview.cpp
//   noise_preview --generator gradient --median auto --view sky --out tools/noise-preview/out
//
// Nothing here is linked into the add-on.
#include "features/clouds/cloud_noise.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace cloud_noise = phantom_fx::cloud_noise;

namespace
{
// cloud_density.hlsli's layer and extinction.
constexpr float LayerBase = 900, LayerTop = 6000;
constexpr float NoiseScale = 0.00022f; // the shape's cube per metre, before the Size
constexpr float GroupScale = 0.00005f; // the group lookup's
constexpr float Extinction = 0.012f;

float saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
float smoothstep(float edge0, float edge1, float x)
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

struct Image
{
    int width = 0, height = 0;
    std::vector<std::uint8_t> grey; // one channel, row 0 at the top

    void set(int x, int y, float value) { grey[static_cast<std::size_t>(y) * width + x] = static_cast<std::uint8_t>(saturate(value) * 255 + 0.5f); }
};

// The baked cube, sampled the way the shader does: trilinear, wrapping on every axis.
struct Field
{
    std::vector<std::uint8_t> cube;
    int size = 64;

    float at(float x, float y, float z) const
    {
        const float fx = x * size, fy = y * size, fz = z * size;
        const int xi = static_cast<int>(std::floor(fx)), yi = static_cast<int>(std::floor(fy)), zi = static_cast<int>(std::floor(fz));
        const float tx = fx - xi, ty = fy - yi, tz = fz - zi;
        const auto wrap = [this](int v) { return ((v % size) + size) % size; };
        const auto voxel = [&](int i, int j, int k)
        {
            return cube[(static_cast<std::size_t>(wrap(k)) * size + wrap(j)) * size + wrap(i)] / 255.0f;
        };
        const auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
        const float c00 = mix(voxel(xi, yi, zi), voxel(xi + 1, yi, zi), tx);
        const float c10 = mix(voxel(xi, yi + 1, zi), voxel(xi + 1, yi + 1, zi), tx);
        const float c01 = mix(voxel(xi, yi, zi + 1), voxel(xi + 1, yi, zi + 1), tx);
        const float c11 = mix(voxel(xi, yi + 1, zi + 1), voxel(xi + 1, yi + 1, zi + 1), tx);
        return mix(mix(c00, c10, ty), mix(c01, c11, ty), tz);
    }
};

// cloud_density.hlsli's CloudDensity, point for point, with the wind at rest and no view fade unless a distance is given.
float density(const Field& field, float size, float cover, float wx, float wy, float wz, float viewDistance = 0)
{
    const float frequency = 1.0f / std::clamp(size, 0.25f, 4.0f);
    const float height = saturate((wy - LayerBase) / (LayerTop - LayerBase));
    // The shader's Turned: a turn about the vertical and a scale, so no two lookups share the cube's lattice or its repeat.
    const auto turned = [&](float x, float y, float z, float angle, float scale, float offset)
    {
        const float c = std::cos(angle), s = std::sin(angle);
        return field.at((c * x - s * z) * scale + offset, y * scale + offset, (s * x + c * z) * scale + offset);
    };
    const float gx = wx * GroupScale * frequency, gz = wz * GroupScale * frequency;
    const float group = 0.5f + (field.at(gx, 0.5f, gz) + turned(gx, 0.31f, gz, 1.21f, 0.73f, 0.17f) - 1) * 0.7f;
    if (cover <= 0) return 0;

    const float px = wx * NoiseScale * frequency, py = wy * NoiseScale * frequency * 1.6f, pz = wz * NoiseScale * frequency;
    const float fineFade = 1 - smoothstep(50000, 150000, viewDistance), detailFade = 1 - smoothstep(25000, 120000, viewDistance);
    const float broad = 0.5f + (field.at(px, py, pz) + turned(px, py, pz, 0.83f, 0.87f, 0.29f) - 1) * 0.7f;
    const float base = broad * 0.65f + lerp(0.5f, turned(px, py, pz, 2.07f, 2.3f, 0.37f), fineFade) * 0.35f;
    const float baseShift = saturate(0.02f + 0.35f * (1 - group) + 0.45f * (base - 0.5f));
    float columnTop = std::max(saturate(0.22f + 0.5f * base + 0.15f * (group - 0.5f)), baseShift + 0.15f);
    const float core = saturate((base - (lerp(0.72f, 0.28f, cover) + (0.5f - group) * 0.2f)) / 0.3f);
    const float lowTop = baseShift + 0.15f;
    columnTop = std::min(lerp(lowTop, std::max(columnTop, lowTop), std::sqrt(core)) * lerp(1.0f, 1.35f, core), 1.0f);
    const float profile = smoothstep(baseShift, baseShift + 0.1f, height) * (1 - smoothstep(columnTop * 0.7f, columnTop, height));
    if (profile <= 0) return 0;
    // The coverage is the share of the sky with cloud: the column needs a shape above it, and the group lifts or lowers that mark.
    // The shape's own 10th and 90th are the ends the fill walks between, so 100% is not a case apart from the rest. A cumulus
    // narrows as it rises, so the fill tightens with height and the top is a dome over a broad base.
    const float fill = cover * lerp(1.0f, 0.55f, saturate(height / std::max(columnTop, 0.05f)));
    const float threshold = lerp(0.72f, 0.28f, fill) + (0.5f - group) * 0.2f;
    const float mass = saturate((base - threshold) / 0.12f) * profile;
    if (mass <= 0) return 0;
    const float detail = lerp(0.5f, turned(px, py, pz, 1.39f, 6.0f, 0.11f), detailFade);
    const float erode = lerp(detail, 1 - detail, saturate(height * 3));
    return saturate((mass - erode * 0.35f) / (1 - erode * 0.35f));
}

float baseCover(float coverage, float weather)
{
    const float steep = std::pow(saturate(weather), 1.5f);
    const float more = saturate(coverage - 1);
    return saturate(steep * std::min(coverage, 1.0f) + more * (1 - steep) * 0.6f);
}

// A vertical cut through the layer, the deepest of eight parallel cuts: the profile's base and top, and any lattice plane,
// are plain to see. One cut can miss a cloud group, eight rarely do.
Image slice(const Field& field, float size, float cover, float repeat, float span)
{
    Image image{600, 400, {}};
    image.grey.resize(static_cast<std::size_t>(image.width) * image.height);
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x)
        {
            const float wx = (static_cast<float>(x) / image.width - 0.5f) * span;
            const float wy = lerp(LayerBase - 300, LayerTop + 300, 1.0f - static_cast<float>(y) / image.height);
            float deepest = 0;
            for (int cut = 0; cut < 16; ++cut)
                deepest = std::max(deepest, density(field, size, cover, wx, wy, (cut / 16.0f - 0.5f) * span));
            image.set(x, y, deepest * 0.8f);
        }
    (void)repeat;
    return image;
}

// What the sky sees from above: a column's whole density, so the clumping and the cube's repeat are plain.
Image top(const Field& field, float size, float cover, float repeat, float span)
{
    Image image{600, 600, {}};
    image.grey.resize(static_cast<std::size_t>(image.width) * image.height);
    const int samples = 48;
    int clouded = 0;
    float total = 0;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x)
        {
            const float wx = (static_cast<float>(x) / image.width - 0.5f) * span;
            const float wz = (static_cast<float>(y) / image.height - 0.5f) * span;
            float column = 0;
            for (int i = 0; i < samples; ++i)
            {
                const float wy = lerp(LayerBase, LayerTop, (i + 0.5f) / samples);
                column += density(field, size, cover, wx, wy, wz);
            }
            column /= samples;
            clouded += column > 0.02f ? 1 : 0;
            total += column;
            image.set(x, y, column * 2.2f);
        }
    std::printf("sky with cloud: %.0f%%, mean column density %.3f\n", 100.0f * clouded / (image.width * image.height),
        total / (image.width * image.height));
    (void)repeat;
    return image;
}

// The look out of the cockpit: a ray a pixel, marched through the layer the way clouds.hlsl does, no lighting.
Image sky(const Field& field, float size, float cover)
{
    const int steps = 256;
    Image image{640, 360, {}};
    image.grey.resize(static_cast<std::size_t>(image.width) * image.height);
    const float eye = 26, near = 400, far = 200000;
    for (int y = 0; y < image.height; ++y)
    {
        const float pitch = lerp(-0.02f, 0.30f, 1.0f - static_cast<float>(y) / image.height);
        for (int x = 0; x < image.width; ++x)
        {
            const float yaw = (static_cast<float>(x) / image.width - 0.5f) * 1.2f;
            const float dx = std::sin(yaw) * std::cos(pitch), dy = std::sin(pitch), dz = std::cos(yaw) * std::cos(pitch);
            float transmittance = 1, last = near;
            for (int i = 1; i <= steps; ++i)
            {
                const float t = near * std::pow(far / near, static_cast<float>(i) / steps);
                const float dt = t - last;
                last = t;
                const float d = density(field, size, cover, dx * t, eye + dy * t, dz * t, t);
                if (d > 0) transmittance *= std::exp(-Extinction * d * dt);
                if (transmittance < 0.002f) break;
            }
            // Optical depth, not opacity: an opaque cloud still shows how thick it is, so the shapes stay readable.
            image.set(x, y, 1 - std::exp(-(-std::log(std::max(transmittance, 0.002f))) * 0.6f));
        }
    }
    return image;
}

// PNG, with the deflate stream stored uncompressed: a few lines instead of a library.
void writePng(const Image& image, const std::string& path)
{
    const auto be32 = [](std::vector<std::uint8_t>& out, std::uint32_t v)
    {
        out.push_back(static_cast<std::uint8_t>(v >> 24));
        out.push_back(static_cast<std::uint8_t>(v >> 16));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
        out.push_back(static_cast<std::uint8_t>(v));
    };
    const auto crc32 = [](const std::vector<std::uint8_t>& data)
    {
        std::uint32_t crc = 0xffffffffu;
        for (const std::uint8_t byte : data)
        {
            crc ^= byte;
            for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
        return crc ^ 0xffffffffu;
    };
    const auto chunk = [&](const char* type, const std::vector<std::uint8_t>& payload)
    {
        std::vector<std::uint8_t> body(type, type + 4);
        body.insert(body.end(), payload.begin(), payload.end());
        std::vector<std::uint8_t> out;
        be32(out, static_cast<std::uint32_t>(payload.size()));
        out.insert(out.end(), body.begin(), body.end());
        be32(out, crc32(body));
        return out;
    };

    // Each row: the filter byte then the grey pixels.
    std::vector<std::uint8_t> raw;
    for (int y = 0; y < image.height; ++y)
    {
        raw.push_back(0);
        raw.insert(raw.end(), image.grey.begin() + static_cast<std::size_t>(y) * image.width, image.grey.begin() + static_cast<std::size_t>(y + 1) * image.width);
    }
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t byte : raw)
    {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    std::vector<std::uint8_t> zlib{0x78, 0x01};
    std::size_t offset = 0;
    while (offset < raw.size())
    {
        const std::size_t block = std::min<std::size_t>(65535, raw.size() - offset);
        zlib.push_back(offset + block == raw.size() ? 1 : 0);
        zlib.push_back(static_cast<std::uint8_t>(block));
        zlib.push_back(static_cast<std::uint8_t>(block >> 8));
        zlib.push_back(static_cast<std::uint8_t>(~block));
        zlib.push_back(static_cast<std::uint8_t>(~block >> 8));
        zlib.insert(zlib.end(), raw.begin() + offset, raw.begin() + offset + block);
        offset += block;
    }
    be32(zlib, (b << 16) | a);

    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<std::uint8_t> header;
    be32(header, static_cast<std::uint32_t>(image.width));
    be32(header, static_cast<std::uint32_t>(image.height));
    header.insert(header.end(), {8, 0, 0, 0, 0});
    const auto ihdr = chunk("IHDR", header);
    const auto idat = chunk("IDAT", zlib);
    const auto iend = chunk("IEND", {});
    png.insert(png.end(), ihdr.begin(), ihdr.end());
    png.insert(png.end(), idat.begin(), idat.end());
    png.insert(png.end(), iend.begin(), iend.end());

    if (FILE* file = std::fopen(path.c_str(), "wb"))
    {
        std::fwrite(png.data(), 1, png.size(), file);
        std::fclose(file);
        std::printf("%s\n", path.c_str());
    }
}
} // namespace

int main(int argc, char** argv)
{
    std::string generator = "value", view = "sky", out = "tools/noise-preview/out", name;
    int size = 64, octaves = 4;
    float warp = 0, coverage = 1.1f, weather = 0.6f, menuSize = 1.5f, span = 24000;
    bool measureMedian = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const auto next = [&]() { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--generator") generator = next();
        else if (arg == "--view") view = next();
        else if (arg == "--out") out = next();
        else if (arg == "--name") name = next();
        else if (arg == "--size") size = std::atoi(next());
        else if (arg == "--octaves") octaves = std::atoi(next());
        else if (arg == "--warp") warp = std::strtof(next(), nullptr);
        else if (arg == "--coverage") coverage = std::strtof(next(), nullptr);
        else if (arg == "--weather") weather = std::strtof(next(), nullptr);
        else if (arg == "--menu-size") menuSize = std::strtof(next(), nullptr);
        else if (arg == "--span") span = std::strtof(next(), nullptr);
        else if (arg == "--median") measureMedian = std::strcmp(next(), "auto") == 0;
    }

    cloud_noise::Recipe recipe;
    recipe.generator = generator == "gradient" ? cloud_noise::Generator::Gradient
        : generator == "cellular"             ? cloud_noise::Generator::Cellular
                                              : cloud_noise::Generator::Value;
    recipe.size = size;
    recipe.octaves = octaves;
    recipe.warp = warp;
    if (measureMedian) recipe.median = std::nanf("");

    Field field;
    field.size = size;
    field.cube = cloud_noise::bake(recipe);
    {
        std::vector<std::uint8_t> sorted = field.cube;
        std::sort(sorted.begin(), sorted.end());
        const auto at = [&](float q) { return sorted[static_cast<std::size_t>(q * (sorted.size() - 1))] / 255.0f; };
        std::printf("cube p10 %.2f p50 %.2f p90 %.2f\n", at(0.1f), at(0.5f), at(0.9f));
    }

    const float repeat = 1.0f / (NoiseScale / std::clamp(menuSize, 0.25f, 4.0f));
    const float cover = baseCover(coverage, weather);
    const Image image = view == "slice" ? slice(field, menuSize, cover, repeat, span)
        : view == "top"                 ? top(field, menuSize, cover, repeat, span)
                                        : sky(field, menuSize, cover);

    if (name.empty()) name = generator + "-" + view;
    writePng(image, out + "/" + name + ".png");
    return 0;
}
