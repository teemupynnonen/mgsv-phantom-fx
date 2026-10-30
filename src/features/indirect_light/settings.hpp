#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
struct IndirectLight
{
    bool on{};
    float radius{0.8f};        // Share of the screen's height the gather reaches
    float strength{1};         // The gathered light's multiplier at the resolve
    float samples{8};          // Gather samples per pixel
    int resolution{1};         // The gather's resolution: 0 full, 1 half, 2 quarter
    float denoisePasses{3};    // A-trous iterations the denoiser runs
    float history{0.9f};       // Ceiling on how much of the previous frame survives
    int debug{};               // Diagnostic view: 0 off, 1 GI only, 2 history delta, 3 history length, 4 reprojection, 5 variance

    bool operator==(const IndirectLight&) const = default;
};

void describe(IndirectLight& light, SettingVisitor& visit);
}  // namespace phantom_fx
