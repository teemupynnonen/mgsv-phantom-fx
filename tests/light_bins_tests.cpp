#include "passes/ray_tracing/light_bins.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>

namespace
{
using namespace phantom_fx;
using gi::GIFrameConstants, gi::GILocalLight;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::unique_ptr<GIFrameConstants> constantsAt(float x, float y, float z)
{
    auto constants = std::make_unique<GIFrameConstants>();
    constants->eye = {x, y, z, 0};
    return constants;
}

GILocalLight pointLight(float x, float y, float z, float range)
{
    GILocalLight light{};
    light.position = {x, y, z, range};
    return light;
}

// The bin LightsNear in gi_common.hlsli takes: the shader's own rule, to catch the bins being filled by another.
int shaderBin(float offset, int bins)
{
    return std::clamp(static_cast<int>(std::floor(offset / GI_LIGHT_BIN_SIZE)), 0, bins - 1);
}

// Whether the bins of `world` hold the light, as LightsNear reads them.
bool isInBins(const GIFrameConstants& constants, std::uint32_t light, const std::array<float, 3>& world)
{
    const auto& origin = constants.lightBinsOrigin;
    const auto across = static_cast<int>(GI_LIGHT_BINS_ACROSS), high = static_cast<int>(GI_LIGHT_BINS_HIGH);
    const auto& x = constants.lightBinsX[shaderBin(world[0] - origin.x, across)];
    const auto& y = constants.lightBinsY[shaderBin(world[1] - origin.y, high)];
    const auto& z = constants.lightBinsZ[shaderBin(world[2] - origin.z, across)];
    const std::array words{x.x & y.x & z.x, x.y & y.y & z.y, x.z & y.z & z.z, x.w & y.w & z.w};

    return (words[light / 32] >> (light % 32) & 1u) != 0;
}

// As LocalLightReaching gives a light any light at all, without its falloff and cone.
bool canReach(const GILocalLight& light, const std::array<float, 3>& world)
{
    const float offset[]{world[0] - light.position.x, world[1] - light.position.y, world[2] - light.position.z};
    if (std::sqrt(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]) >= light.position.w) return false;
    if (light.volumeLow.w <= 0.0f) return true;

    const float margin = GI_LIGHT_VOLUME_MARGIN;

    return world[0] >= light.volumeLow.x - margin && world[0] <= light.volumeHigh.x + margin && world[1] >= light.volumeLow.y - margin
        && world[1] <= light.volumeHigh.y + margin && world[2] >= light.volumeLow.z - margin && world[2] <= light.volumeHigh.z + margin;
}

// The rule all of it rests on: a light that reaches a place is in that place's bins, wherever the light, the
// place and the eye are, in the bins or past them. A light left out there would go dark.
void everyLightThatReachesIsInItsBins()
{
    std::mt19937 random(1);
    std::uniform_real_distribution<float> eyeAt(-2000.0f, 2000.0f), lightAt(-500.0f, 500.0f), range(0.5f, 200.0f), box(0.2f, 30.0f), place(-1.2f, 1.2f);
    for (int round = 0; round < 300; ++round)
    {
        auto constants = constantsAt(eyeAt(random), eyeAt(random) * 0.1f, eyeAt(random));
        constants->localLightCount.x = GI_MAX_LOCAL_LIGHTS;
        for (std::uint32_t i = 0; i < GI_MAX_LOCAL_LIGHTS; ++i)
        {
            const float scale = i % 3 == 0 ? 0.1f : 1.0f;
            auto& light = constants->localLights[i];
            light = pointLight(constants->eye.x + lightAt(random) * scale, constants->eye.y + lightAt(random) * 0.2f, constants->eye.z + lightAt(random) * scale, range(random));
            if (i % 2)
            {
                const float side = box(random);
                light.volumeLow = {light.position.x - side, light.position.y - side / 2, light.position.z - side, 1.0f};
                light.volumeHigh = {light.position.x + side, light.position.y + side / 2, light.position.z + side, 0.0f};
            }
        }

        light_bins::fill(*constants);
        for (int sample = 0; sample < 4000; ++sample)
        {
            const auto& light = constants->localLights[sample % GI_MAX_LOCAL_LIGHTS];
            const float reach = light.position.w;
            const std::array world{light.position.x + place(random) * reach, light.position.y + place(random) * reach, light.position.z + place(random) * reach};
            if (canReach(light, world)) expect(isInBins(*constants, sample % GI_MAX_LOCAL_LIGHTS, world), "A light that reaches a place is in its bins");
        }
    }
}

// What the bins are for: a light is left out of places well off its box.
void aLightStaysOutOfPlacesOffItsBox()
{
    auto constants = constantsAt(0, 0, 0);
    constants->localLightCount.x = 2;
    constants->localLights[0] = pointLight(10, 0, 10, 4);
    constants->localLights[1] = pointLight(-20, 0, 0, 100);
    constants->localLights[1].volumeLow = {-22, -2, -2, 1};
    constants->localLights[1].volumeHigh = {-18, 2, 2, 0};
    light_bins::fill(*constants);

    expect(isInBins(*constants, 0, {10, 0, 10}), "The light is in the bins of its own place");
    expect(!isInBins(*constants, 0, {-10, 0, 10}), "Not along x");
    expect(!isInBins(*constants, 0, {10, 12, 10}), "Not along y");
    expect(!isInBins(*constants, 0, {10, 0, -10}), "Not along z");
    expect(isInBins(*constants, 1, {-20, 0, 0}), "A long reach is in the bins of its box");
    expect(!isInBins(*constants, 1, {-40, 0, 0}), "A long reach is cut to the game's volume");
}

// A box takes the bins the shader's floor gives its ends, no more: x from 4 to 6 m around the eye here is one bin.
void aBoxEndsAtItsBinsEdges()
{
    auto constants = constantsAt(0, 0, 0);
    constants->localLightCount.x = 1;
    constants->localLights[0] = pointLight(4.6f, 0, 0, 0.5f);
    light_bins::fill(*constants);

    expect(isInBins(*constants, 0, {4.1f, 0, 0}), "Its bin starts at 4 m");
    expect(isInBins(*constants, 0, {5.9f, 0, 0}), "And ends at 6 m");
    expect(!isInBins(*constants, 0, {3.9f, 0, 0}), "Not the bin below");
    expect(!isInBins(*constants, 0, {6.1f, 0, 0}), "Not the bin above");
}

// A light gives light a little past its volume, so where its volume ends just short of a bin's edge it reaches the next bin.
void aVolumesMarginReachesTheNextBin()
{
    auto constants = constantsAt(0, 0, 0);
    constants->localLightCount.x = 1;
    auto& light = constants->localLights[0];
    light = pointLight(5, 0, 0, 100);
    light.volumeLow = {4.01f, -1, -1, 1};
    light.volumeHigh = {5.99f, 1, 1, 0};
    light_bins::fill(*constants);

    const std::array<float, 3> belowBin{3.995f, 0, 0}, aboveBin{6.005f, 0, 0};
    expect(canReach(light, belowBin), "It reaches past its volume");
    expect(isInBins(*constants, 0, belowBin), "So it is in the bin below");
    expect(canReach(light, aboveBin), "And past the other end");
    expect(isInBins(*constants, 0, aboveBin), "And in the bin above");
}

// Lights past the bins share the last bin with the places past them.
void placesPastTheBinsKeepTheLightsPastThem()
{
    auto constants = constantsAt(0, 0, 0);
    constants->localLightCount.x = 1;
    constants->localLights[0] = pointLight(5000, 0, 0, 10);
    light_bins::fill(*constants);

    expect(isInBins(*constants, 0, {5000, 0, 0}), "A far light is found at its place");
    expect(!isInBins(*constants, 0, {0, 0, 0}), "And not around the eye");
}

// A reach past what an int counts in bins is one past the bins, not one that wraps to the first of them.
void aReachPastAnIntIsInEveryBin()
{
    auto constants = constantsAt(0, 0, 0);
    constants->localLightCount.x = 1;
    constants->localLights[0] = pointLight(0, 0, 0, 1e10f);
    light_bins::fill(*constants);

    expect(isInBins(*constants, 0, {0, 0, 0}), "Around the eye");
    expect(isInBins(*constants, 0, {300, 100, -300}), "And past the bins");
}
}

int main()
{
    try
    {
        everyLightThatReachesIsInItsBins();
        aLightStaysOutOfPlacesOffItsBox();
        aBoxEndsAtItsBinsEdges();
        aVolumesMarginReachesTheNextBin();
        placesPastTheBinsKeepTheLightsPastThem();
        aReachPastAnIntIsInEveryBin();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "Light bins: all tests passed.\n";
}
