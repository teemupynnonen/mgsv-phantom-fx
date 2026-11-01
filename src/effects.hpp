#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace phantom_fx
{
// The lighting effects: those the enhancements switch (F1) holds.
// In a cost measurement's order (performance.hpp): the sea first, so its cost holds the light the other
// effects give its pixels, then the caustics, a draw of their own; path tracing, the ray-traced shadows and lights before the global illumination,
// so with it on, their steps give their own cost while its scene stays; the last of them left out takes the
// scene with it.
enum class Effect : std::uint8_t
{
    Sea,
    WaterCaustics,
    VolumetricFog,
    PathTracing,
    RayTracedShadows,
    RayTracedLights,
    GlobalIllumination,
    IndirectLight,
    AmbientOcclusion,
    LampContactShadows,
    LampSoftShadows,
    SunContactShadows,
    SoftShadows,
    Count
};

constexpr auto AllEffects = [] {
    std::array<Effect, static_cast<std::size_t>(Effect::Count)> all{};
    for (std::size_t i = 0; i < all.size(); ++i) all[i] = static_cast<Effect>(i);
    return all;
}();

// The effects that trace rays through the ray-traced scene, which they share.
constexpr std::array RayTracedEffects{Effect::GlobalIllumination, Effect::RayTracedShadows, Effect::RayTracedLights, Effect::PathTracing};

template<class T> struct ByEffect
{
    std::array<T, static_cast<std::size_t>(Effect::Count)> values{};

    T& operator[](Effect effect) { return values[static_cast<std::size_t>(effect)]; }
    const T& operator[](Effect effect) const { return values[static_cast<std::size_t>(effect)]; }
};

// How an effect the plan runs is doing, by its modules, for the budget.
struct Health
{
    enum class Kind : std::uint8_t { Fine, Waiting, Unavailable };

    Kind kind{};
    std::string reason; // What it waits for, or why it is unavailable (it failed)

    static Health waiting(std::string reason) { return {Kind::Waiting, std::move(reason)}; }
    static Health unavailable(std::string reason) { return {Kind::Unavailable, std::move(reason)}; }
};
}
