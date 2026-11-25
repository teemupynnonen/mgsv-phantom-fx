#include "features/ray_traced_lights/light_choice.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
using namespace phantom_fx::light_choice;
using phantom_fx::game_constants::SceneConstants;
using phantom_fx::game_lights::Falloff, phantom_fx::game_lights::GameLight, phantom_fx::game_lights::Shape, phantom_fx::game_lights::Twin;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool isNear(float value, float expected)
{
    return std::abs(value - expected) < 1e-4f * std::max(1.0f, std::abs(expected));
}

bool isRect(const std::optional<D3D11_RECT>& rect, LONG left, LONG top, LONG right, LONG bottom)
{
    return rect && rect->left == left && rect->top == top && rect->right == right && rect->bottom == bottom;
}

LONG pixelsOf(const D3D11_RECT& rect)
{
    return (rect.right - rect.left) * (rect.bottom - rect.top);
}

// The game's kinds of light, as their rows in game_shaders.hpp have them.
constexpr GameLight QuarticPoint{Shape::Point, Falloff::Quartic, Twin::Point2, true, false};       // SSLighting2_Point_NoShadow
constexpr GameLight QuarticSpot{Shape::Spot, Falloff::Quartic, Twin::Spot2, true, false};          // SSLighting2_Spot_NoShadow
constexpr GameLight SixteenthSpot{Shape::Spot, Falloff::Sixteenth, Twin::SpotNA, true, false};     // Lighting_Spot_NA_NShdw
constexpr GameLight ShadowMappedSpot{Shape::Spot, Falloff::Sixteenth, Twin::SpotNA, true, true};   // Lighting_Spot_NA

// A light pass's scene constants for a camera at `eye`: its view space is the world's moved to the eye,
// and it sees 90 degrees across (1 / P00 and 1 / P11 of 1).
SceneConstants cameraAt(std::array<float, 3> eye)
{
    SceneConstants scene{};
    for (int i = 0; i < 3; ++i)
    {
        scene.view[i][i] = 1;
        scene.view[i][3] = eye[i];
        scene.eye[i] = eye[i];
    }

    scene.view[3][3] = scene.eye[3] = 1;
    scene.projectionParam[0] = scene.projectionParam[1] = 1;

    return scene;
}

// A white light of kind `game` at view-space `position`, whose falloff ends `range` metres away, drawn by a
// camera at the world's origin into a 100 x 100 viewport.
DrawnLight lightAt(const GameLight& game, std::array<float, 3> position, float range)
{
    DrawnLight light{.game = &game, .scene = cameraAt({}), .viewport = {0, 0, 100, 100, 0, 1}};
    std::ranges::copy(position, light.light.position);
    light.light.color[0] = light.light.color[1] = light.light.color[2] = 1;
    if (game.falloff == Falloff::Quartic) light.light.falloff[3] = std::pow(range, -4.0f);
    else light.light.falloff[2] = std::pow(range, -16.0f);

    return light;
}

// A light `distance` metres in front of the camera reaching far past it, so it reaches all of its
// `width` x `height` viewport.
DrawnLight aroundCamera(float width, float height, float distance = 1)
{
    auto light = lightAt(QuarticPoint, {0, 0, distance}, 100);
    light.viewport.Width = width;
    light.viewport.Height = height;

    return light;
}

Choice chooseFrom(const std::vector<DrawnLight>& lights, const SceneConstants& camera = cameraAt({}),
    std::span<const std::uint64_t> placesTaken = {})
{
    std::vector<const DrawnLight*> pointers;
    for (const auto& light : lights) pointers.push_back(&light);

    return choose(pointers, camera, placesTaken);
}

// `count` lights, each reaching more pixels than the one before.
std::vector<DrawnLight> growingLights(std::size_t count)
{
    std::vector<DrawnLight> lights;
    for (std::size_t i = 0; i < count; ++i)
    {
        lights.push_back(aroundCamera(100.0f + static_cast<float>(i), 100));
        // Each at its own place: lights at one place are taken and left together, as a lamp's two spots are.
        lights.back().light.position[0] = 0.05f * static_cast<float>(i);
    }

    return lights;
}

void mostPixelsFirst()
{
    const auto choice = chooseFrom({aroundCamera(50, 50), aroundCamera(100, 100), aroundCamera(70, 70)});
    expect(choice.taken == std::vector<std::size_t>{1, 2, 0}, "The lights reaching the most pixels come first.");
}

void nearestBreaksPixelTies()
{
    const auto choice = chooseFrom({aroundCamera(100, 100, 3), aroundCamera(100, 100, 1), aroundCamera(100, 100, 2)});
    expect(choice.taken == std::vector<std::size_t>{1, 2, 0}, "Among lights reaching as many pixels, the nearest come first.");
}

void allTakenAtTheCap()
{
    const auto choice = chooseFrom(growingLights(GI_MAX_LOCAL_LIGHTS));
    expect(choice.taken.size() == GI_MAX_LOCAL_LIGHTS && choice.overCap == 0, "Every light is taken up to the light cap.");
}

void fewestPixelsDroppedPastTheCap()
{
    const auto choice = chooseFrom(growingLights(GI_MAX_LOCAL_LIGHTS + 1));
    expect(choice.taken.size() == GI_MAX_LOCAL_LIGHTS && choice.overCap == 1, "A light past the cap is counted.");
    expect(std::ranges::find(choice.taken, std::size_t{0}) == choice.taken.end(), "The light reaching the fewest pixels goes without.");
}

void sameChoiceInAnyOrder()
{
    // Past the cap, lights alike in pixels, each at its own place: whichever order they come in, the same ones
    // go without, and the order they are taken in does not change.
    std::vector<DrawnLight> lights;
    for (std::size_t i = 0; i <= GI_MAX_LOCAL_LIGHTS; ++i)
    {
        lights.push_back(aroundCamera(100, 100));
        lights.back().light.color[0] = 1.0f + static_cast<float>(i);
        lights.back().light.position[0] = 0.05f * static_cast<float>(i);
    }

    const auto keysTaken = [](const std::vector<DrawnLight>& from) {
        std::vector<std::uint64_t> keys;
        for (const auto index : chooseFrom(from).taken) keys.push_back(keyOf(from[index]));

        return keys;
    };
    auto reversed = lights;
    std::ranges::reverse(reversed);
    expect(keysTaken(lights) == keysTaken(reversed), "The same lights are chosen, in the same order, whatever order the game drew them in.");
    expect(chooseFrom(lights).overCap == 1, "One light past the cap goes without, whichever order it came in.");
}

// A distant lamp's reach is a few render pixels: the upscaler's jitter, under a pixel, moves its edges across
// pixel boundaries, so the lamps at the cap swapped from frame to frame with the camera still, each drawn
// path traced one frame and by the game, unshadowed, the next (2026-11-07, Africa at night, 20,000 lights
// past the cap in ten seconds).
void jitterLeavesChoiceAlone()
{
    std::vector<DrawnLight> lights;
    for (std::size_t i = 0; i <= GI_MAX_LOCAL_LIGHTS; ++i)
        lights.push_back(lightAt(QuarticPoint, {0.37f * static_cast<float>(i % 7) - 1.1f, 0.21f * static_cast<float>(i % 5) - 0.4f,
                                                30.0f + 0.013f * static_cast<float>(i)}, 0.5f + 0.0007f * static_cast<float>(i)));

    const auto keysTaken = [&](float x, float y) {
        auto jittered = lights;
        for (auto& light : jittered)
        {
            light.scene.cameraCenterOffset[0] = x;
            light.scene.cameraCenterOffset[1] = y;
        }
        std::vector<std::uint64_t> keys;
        for (const auto index : chooseFrom(jittered).taken) keys.push_back(keyOf(jittered[index]));
        std::ranges::sort(keys);

        return keys;
    };
    const auto still = keysTaken(0, 0);
    // A Halton sequence's first offsets, in NDC of a 100 x 100 viewport: half a pixel at most.
    for (const auto [x, y] : {std::pair{0.0f, 0.0033f}, {-0.005f, -0.0033f}, {0.0025f, 0.0078f}, {-0.0075f, -0.0011f}})
        expect(keysTaken(x, y) == still, "The upscaler's jitter does not change which lights the rays take.");
}

// Africa's villa wall lamps are two spots at one place, one up and one down (2026-11-08). Traced separately, one could
// fit the cap while the other did not, and the wall they light blinked between the two draws.
void aPlacesLightsComeAndGoTogether()
{
    // Room under the cap: both spots of the place are taken.
    std::vector<DrawnLight> roomy = growingLights(GI_MAX_LOCAL_LIGHTS - 4);
    auto up = lightAt(QuarticSpot, {0, 1, 40}, 6);
    auto down = up;
    down.light.axis[1] = -1;
    roomy.push_back(up);
    roomy.push_back(down);
    const auto taken = chooseFrom(roomy);
    expect(std::ranges::find(taken.taken, roomy.size() - 2) != taken.taken.end()
            && std::ranges::find(taken.taken, roomy.size() - 1) != taken.taken.end(),
        "Two spots at one place are taken together.");

    // At the cap, one more light would not fit: both are left out together, not one of the two.
    std::vector<DrawnLight> lights = growingLights(GI_MAX_LOCAL_LIGHTS - 2);
    lights.push_back(up);
    lights.push_back(down);
    lights.push_back(aroundCamera(150, 150)); // The biggest: always taken

    const auto choice = chooseFrom(lights);
    const auto takenUp = std::ranges::find(choice.taken, lights.size() - 3) != choice.taken.end();
    const auto takenDown = std::ranges::find(choice.taken, lights.size() - 2) != choice.taken.end();
    expect(takenUp == takenDown && !takenUp, "Two spots at one place are both left out when their place does not fit.");
    expect(choice.overCap == 2, "Both spots of the place left out are counted.");
}

// A place the rays took last time keeps a fifth's advantage, so at the cap it is not handed to a place a few
// pixels larger: the wall lamp it lights would blink between the rays and the game's unshadowed draw.
void aPlaceTakenLastTimeKeepsItsPlace()
{
    std::vector<DrawnLight> lights;
    auto kept = aroundCamera(100, 100);
    auto rival = aroundCamera(110, 100); // Reaches more
    rival.light.position[0] = 0.05f;     // At another place
    lights.push_back(kept);
    lights.push_back(rival);

    const auto fresh = chooseFrom(lights);
    expect(fresh.taken.front() == 1, "Without history the place reaching more pixels is taken first.");

    const auto keptOnly = std::vector{kept};
    const auto keptPlace = chooseFrom(keptOnly).places.front();
    const auto keptAgain = chooseFrom(lights, cameraAt({}), std::span{&keptPlace, 1});
    expect(keptAgain.taken.front() == 0, "A place the rays had last time keeps its place against a rival a tenth larger.");
}

void otherEyeLeftAndCounted()
{
    auto lights = std::vector{aroundCamera(100, 100), aroundCamera(100, 100)};
    lights[0].scene = cameraAt({1.002f, 2, 3});
    lights[1].scene = cameraAt({1.0005f, 2, 3});
    const auto choice = chooseFrom(lights, cameraAt({1, 2, 3}));
    expect(choice.taken == std::vector<std::size_t>{1} && choice.otherFrame == 1,
        "A light drawn from more than a millimetre off the camera's eye is left for another frame and counted.");
}

void noPixelNotTaken()
{
    const auto choice = chooseFrom({lightAt(QuarticPoint, {0, 0, -5}, 2), aroundCamera(100, 100)});
    expect(choice.taken == std::vector<std::size_t>{1} && choice.otherFrame == 0 && choice.overCap == 0,
        "A light reaching no pixel is not taken, nor counted.");
}

void wholeViewportWhenInside()
{
    auto light = lightAt(QuarticPoint, {0, 0, 1}, 2);
    light.viewport = {200, 50, 640, 360, 0, 1};
    expect(isRect(reachOf(light), 200, 50, 840, 410), "A light around the camera plane reaches its whole viewport.");
}

void noneBehindCamera()
{
    expect(!reachOf(lightAt(QuarticPoint, {0, 0, -3}, 2)), "A light wholly behind the camera reaches nothing.");
}

void narrowSpotReachesLessThanPoint()
{
    const auto point = reachOf(lightAt(QuarticPoint, {0, 0, 10}, 4));
    auto spot = lightAt(QuarticSpot, {0, 0, 10}, 4);
    spot.light.axis[2] = 1;
    spot.light.cutOffCosine = 0.9f;
    const auto spotReach = reachOf(spot);
    expect(point && spotReach && pixelsOf(*spotReach) < pixelsOf(*point), "A narrow spot reaches fewer pixels than a point as far.");
}

void offsetByViewportCorner()
{
    auto light = lightAt(QuarticPoint, {2, 1, 10}, 2);
    const auto atOrigin = reachOf(light);
    light.viewport.TopLeftX = 200;
    light.viewport.TopLeftY = 50;
    expect(atOrigin && isRect(reachOf(light), atOrigin->left + 200, atOrigin->top + 50, atOrigin->right + 200, atOrigin->bottom + 50),
        "A light's reach moves with its viewport's corner.");
}

void rangeEndsFalloff()
{
    auto point = lightAt(QuarticPoint, {0, 0, 5}, 4);
    point.light.falloff[0] = 0.5f;
    expect(isNear(forRays(point, {}).range, 3.5f), "A point's range ends where its falloff does, less its added distance.");

    auto spot = lightAt(SixteenthSpot, {0, 0, 5}, 4);
    spot.light.falloff[0] = 0.5f;
    expect(isNear(forRays(spot, {}).range, 4), "A spot's range ends where its falloff does.");
}

void bulbRadiusWhenSizelessOrShadowMapped()
{
    auto lamp = lightAt(SixteenthSpot, {0, 0, 5}, 4);
    lamp.light.size = 0.42f;
    expect(forRays(lamp, {}).radius == 0.42f, "A light's shadow rays spread over its size.");

    auto sizeless = lamp;
    sizeless.light.size = 0;
    expect(forRays(sizeless, {}).radius == 0.03f, "A light without a size takes a bulb's.");

    auto shadowMapped = lamp;
    shadowMapped.game = &ShadowMappedSpot;
    expect(forRays(shadowMapped, {}).radius == 0.03f, "A shadow-mapped light takes a bulb's, whatever its size.");
}

void shadowStartAtMapNearPlane()
{
    // A spot pointing along z whose map's reversed depth, 0.25 / the distance along z past the light, is 1
    // (its near plane) a quarter metre along its axis.
    auto spot = lightAt(ShadowMappedSpot, {0, 1, 2}, 4);
    spot.light.axis[2] = 2;
    spot.scene.shadowProjection[2][3] = 0.25f;
    spot.scene.shadowProjection[3][2] = 1;
    spot.scene.shadowProjection[3][3] = -2;
    expect(isNear(forRays(spot, {}).shadowStart, 0.25f), "A shadow-mapped spot's rays end where its map starts.");

    auto unmapped = spot;
    unmapped.game = &SixteenthSpot;
    expect(forRays(unmapped, {}).shadowStart == 0, "A spot without a shadow map has no map start.");

    auto pastRange = spot;
    pastRange.scene.shadowProjection[2][3] = 5;
    expect(forRays(pastRange, {}).shadowStart == 0, "A map starting past the light's range is none.");
}

void sameKeyFromTwoCameras()
{
    // The same lamp at (3, 1, 7) in the world, seen from the origin and from (1, 0, 2).
    const auto fromOrigin = lightAt(QuarticPoint, {3, 1, 7}, 4);
    auto fromElsewhere = lightAt(QuarticPoint, {2, 1, 5}, 4);
    fromElsewhere.scene = cameraAt({1, 0, 2});
    expect(keyOf(fromOrigin) == keyOf(fromElsewhere), "A light keeps its key from any camera.");
}

// A wall lamp in Africa's villa: two spots at one place, alike but for where they point (2026-11-07).
void spotsPointingApartHaveTwoKeys()
{
    auto up = lightAt(SixteenthSpot, {3, 1, 7}, 6);
    up.light.axis[1] = 1;
    auto down = up;
    down.light.axis[1] = -1;
    expect(keyOf(up) != keyOf(down), "Two spots at one place pointing apart are two lights.");

    auto alsoUp = lightAt(SixteenthSpot, {2, 1, 5}, 6);
    alsoUp.scene = cameraAt({1, 0, 2});
    alsoUp.light.axis[1] = 1;
    expect(keyOf(alsoUp) == keyOf(up), "A spot keeps its key from any camera.");
}

void kindOrColourChangesKey()
{
    const auto lamp = lightAt(QuarticPoint, {3, 1, 7}, 4);
    auto otherKind = lamp;
    otherKind.game = &QuarticSpot;
    auto otherColour = lamp;
    otherColour.light.color[1] = 2;
    expect(keyOf(otherKind) != keyOf(lamp) && keyOf(otherColour) != keyOf(lamp), "Another kind or colour is another light.");
}
}

int main()
{
    try
    {
        mostPixelsFirst();
        nearestBreaksPixelTies();
        allTakenAtTheCap();
        fewestPixelsDroppedPastTheCap();
        sameChoiceInAnyOrder();
        jitterLeavesChoiceAlone();
        aPlacesLightsComeAndGoTogether();
        aPlaceTakenLastTimeKeepsItsPlace();
        otherEyeLeftAndCounted();
        noPixelNotTaken();
        wholeViewportWhenInside();
        noneBehindCamera();
        narrowSpotReachesLessThanPoint();
        offsetByViewportCorner();
        rangeEndsFalloff();
        bulbRadiusWhenSizelessOrShadowMapped();
        shadowStartAtMapNearPlane();
        sameKeyFromTwoCameras();
        kindOrColourChangesKey();
        spotsPointingApartHaveTwoKeys();
        std::cout << "Light choice tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
