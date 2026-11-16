#pragma once
#include <algorithm>
#include <array>
#include <cmath>

inline float zone_smooth(float edge0, float edge1, float x) {
    float t = (x - edge0) / (edge1 - edge0);
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// shadows, midtones, and highlights are −1..1. 0 leaves the pixel alone.
// Applied to the developed sample. The weight is taken from compressed luminance
// so a value above 1 still counts as a highlight.
inline void apply_tonal_zones(float& r, float& g, float& b, float shadows, float midtones, float highlights,
                              std::array<float,3> weights = {.2126f,.7152f,.0722f}) {
    shadows = std::isfinite(shadows) ? std::clamp(shadows, -1.f, 1.f) : 0.f;
    midtones = std::isfinite(midtones) ? std::clamp(midtones, -1.f, 1.f) : 0.f;
    highlights = std::isfinite(highlights) ? std::clamp(highlights, -1.f, 1.f) : 0.f;
    if (shadows == 0.f && midtones == 0.f && highlights == 0.f) return;
    r = std::max(r, 0.f);
    g = std::max(g, 0.f);
    b = std::max(b, 0.f);
    const float luma = weights[0] * r + weights[1] * g + weights[2] * b;
    const float s = luma / (1.f + luma);
    const float shadow_w = 1.f - zone_smooth(0.02f, 0.42f, s);
    const float mid_d = (s - 0.33f) / 0.14f;
    const float mid_w = std::exp(-0.5f * mid_d * mid_d);
    const float high_w = zone_smooth(0.38f, 0.90f, s);
    const float ev = shadows * shadow_w * 1.5f + midtones * mid_w * 1.f + highlights * high_w * 1.5f;
    float gain = std::exp2(ev);
    if (!std::isfinite(gain) || gain < 0.f) gain = 1.f;
    const float lift = shadows * shadow_w * 0.04f;
    r = std::max(0.f, r * gain + lift);
    g = std::max(0.f, g * gain + lift);
    b = std::max(0.f, b * gain + lift);
}
