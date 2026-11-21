#pragma once

#include "game_constants.hpp"

#include <array>
#include <cstdint>

// The camera side of DLSS: recognising the main view in the engine's scene constants, adding the
// jitter to it, and the matrices the motion vectors and camera cuts come from. Pure maths, so the
// tests can check it without the game.
namespace phantom_fx::camera
{
// Row-major 4x4 in the engine's convention.
using Matrix = std::array<double, 16>;

// Gauss-Jordan with partial pivoting. A matrix with no inverse gives a wrong but finite result, as a
// zero pivot scales its row by 0.
Matrix inverse(Matrix matrix);

enum class SceneView : std::uint8_t
{
    Other,
    // The projection is the main camera's perspective projection for the render size (width, height),
    // unjittered.
    MainProjection,
    // Also projectionView = projection x view: the camera itself.
    MainCamera,
};

SceneView classify(const game_constants::SceneConstants& scene);
// Whether scene constants as the upscaler leaves them (jittered, the jitter in cameraCenterOffset) are
// the main camera's.
bool isMainCamera(const game_constants::SceneConstants& jittered);

// The main camera as one upload had it before the jitter.
struct View
{
    Matrix projectionView{}, projection{}, view{};
    std::array<float, 3> eye{}, forward{};
    float focal{}; // P11: changes with the field of view
    float width{}, height{};
    float nearPlane{}, farPlane{};
};

View viewOf(const game_constants::SceneConstants& scene);

// Offsets a MainProjection upload by `ndc`: the projection, the projection-view when it has one, and
// cameraCenterOffset, which the engine's screen-space passes subtract before reconstructing positions
// from depth.
void applyJitter(game_constants::SceneConstants& scene, std::array<float, 2> ndc);
// A projection without the jitter applyJitter gave it (`ndc`): its third column in its first two rows.
Matrix unjitteredProjection(Matrix projection, std::array<float, 2> ndc);

// DLSS takes the jitter in render pixels, x right and y down; the projection takes it in NDC, y up.
std::array<float, 2> jitterNdc(std::array<float, 2> pixels, float width, float height);
// Halton(2, 3) offsets in [-0.5, 0.5) pixels; `frame` counts up by one per rendered frame.
std::array<float, 2> jitterPixels(std::uint64_t frame, int phases);
// DLSS asks for 8 x (output / render)^2 phases; DLAA renders at output size.
int jitterPhases(float renderScale);

// The Halton sequence in `base`, from 0 to 1; index 0 gives 0.
double halton(std::uint64_t index, unsigned base);

// Maps this frame's unjittered NDC and depth to the previous frame's clip position.
Matrix previousFromCurrent(const View& previous, const View& current);
// The same from the projections and views apart, the projections unjittered: the views' change is the
// camera's movement alone, so its place in the world cancels before anything rounds. A projection-view
// the game wrote in float holds that place in its translation, rounded differently for each jitter.
Matrix previousFromCurrentByParts(const View& previous, const View& current);

// A cut (cutscenes, menus into the game) moves or turns the camera further than gameplay does in
// one frame, or changes the field of view at once; DLSS then drops its history.
bool cameraCut(const View& previous, const View& current);

// The camera as NVIDIA Streamline takes it for DLSS Frame Generation, and NGX for Ray Reconstruction:
// matrices for row vectors (v x M) and a view space with x right, y up and z forward. The engine's view
// mirrors x (P00 < 0); the mirror moves from the projection into the view, so the projection is the
// usual one and P x V is unchanged.
struct StreamlineCamera
{
    std::array<float, 16> worldToView{}, viewToClip{}, clipToView{}, clipToPreviousClip{}, previousClipToClip{};
    std::array<float, 3> position{}, right{}, up{}, forward{};
    float nearPlane{}, farPlane{};
    float verticalFov{}; // Radians
    float aspectRatio{};
};

StreamlineCamera streamlineCamera(const View& previous, const View& current);
}
