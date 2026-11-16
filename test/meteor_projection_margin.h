#pragma once
#include <cmath>
#include <cstdlib>

// Expand tangent-space coverage, preserving the eye poses and asymmetric optics.
// The same expanded FOV must drive rendering, composition, and wire metadata.
inline bool meteor_projection_margin(const char *text, float eyes[22], float *applied = nullptr) {
    float scale = 1.15f;
    if (text && *text) {
        char *end = nullptr;
        scale = std::strtof(text, &end);
        if (end == text || *end || !std::isfinite(scale) || scale < 1.f || scale > 1.4f) return false;
    }
    if (!eyes) return false;
    for (unsigned eye = 0; eye < 2; ++eye)
        for (unsigned side = 7; side < 11; ++side) {
            const float angle = eyes[eye * 11 + side];
            if (!std::isfinite(angle) || std::abs(angle) >= 1.57079632679f) return false;
        }
    // Preserve bit-identical optics for the explicit compatibility setting.
    if (scale != 1.f)
        for (unsigned eye = 0; eye < 2; ++eye)
            for (unsigned side = 7; side < 11; ++side) {
                float &angle = eyes[eye * 11 + side];
                angle = std::atan(std::tan(angle) * scale);
            }
    if (applied) *applied = scale;
    return true;
}
