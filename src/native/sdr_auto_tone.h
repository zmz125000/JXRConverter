#pragma once
#include "hdr_parallel.h"
#include "tonal_zones.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace sdr_auto_tone {
// Working linear RGB uses 1.0 = 203 nits. ST.2084 PQ encodes 0..10000 nits into 0..1.
inline double pq(double y) {
    const double n = std::clamp(y * (203.0 / 10000.0), 0.0, 1.0);
    if (n <= 0.0) return 0.0;
    const double p = std::pow(n, 0.1593017578125);
    return std::pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), 78.84375);
}
inline double inverse_pq(double code) {
    const double c = std::clamp(code, 0.0, 1.0);
    if (c <= 0.0) return 0.0;
    const double p = std::pow(c, 1.0 / 78.84375);
    const double n = std::pow(std::max(p - 0.8359375, 0.0) / std::max(18.8515625 - 18.6875 * p, 1e-12), 1.0 / 0.1593017578125);
    return n * (10000.0 / 203.0);
}
} // namespace sdr_auto_tone

// Scene-adaptive negative midtone curve for bright HDR daylight/game scenes.
// Leaves SDR, dark, indoor, night, and backlit shadow-dominated scenes untouched,
// while smoothly dimming overly bright midtones and preserving shadows and peak highlights.
struct AutoSdrTone {
    float strength_ev = 0.f; // Peak dimming in EV (>= 0; 0 leaves pixels untouched).
    float lo_pq = 0.25f;
    float center_pq = 0.52f;
    float hi_pq = 0.78f;
    float lo_y = 0.f;
    float hi_y = 0.f;
    bool active() const { return strength_ev > 1e-4f; }
};

inline AutoSdrTone analyze_auto_sdr_tone(const std::vector<float>& source, const std::array<float, 3>& luma) {
    AutoSdrTone out{};
    const size_t pixels = source.size() / 4;
    if (pixels == 0) return out;
    const double pq_shoulder = sdr_auto_tone::pq(0.5);
    struct Stats { double visible = 0., sum_pq = 0., hdr_area = 0., excess_pq = 0.; };
    const unsigned jobs = hdr_pixel_jobs(pixels);
    std::vector<Stats> partials(jobs);
    hdr_parallel_pixels(pixels, jobs, [&](size_t begin, size_t end, unsigned job) {
        Stats local{};
        for (size_t pixel = begin; pixel < end; ++pixel) {
            const size_t i = pixel * 4;
            const float alpha = source[i + 3];
            if (!std::isfinite(alpha) || alpha <= 0.f) continue;
            const double weight = std::min(double(alpha), 1.0);
            const double r = std::max(std::isfinite(source[i]) ? double(source[i]) : 0.0, 0.0);
            const double g = std::max(std::isfinite(source[i + 1]) ? double(source[i + 1]) : 0.0, 0.0);
            const double b = std::max(std::isfinite(source[i + 2]) ? double(source[i + 2]) : 0.0, 0.0);
            const double y = r * double(luma[0]) + g * double(luma[1]) + b * double(luma[2]);
            const double p = sdr_auto_tone::pq(y);
            local.visible += weight;
            local.sum_pq += weight * p;
            if (y > 1.0) local.hdr_area += weight;
            if (p > pq_shoulder) local.excess_pq += weight * (p - pq_shoulder);
        }
        partials[job] = local;
    });
    Stats total{};
    for (const auto& s : partials) {
        total.visible += s.visible;
        total.sum_pq += s.sum_pq;
        total.hdr_area += s.hdr_area;
        total.excess_pq += s.excess_pq;
    }
    if (total.visible <= 0.0) return out;
    const float mean_pq = float(total.sum_pq / total.visible);
    const float hdr_fraction = float(total.hdr_area / total.visible);
    const float excess_pq = float(total.excess_pq / total.visible);

    // 1. Leave pure SDR and small specular-only HDR scenes neutral.
    const float hdr_gate = zone_smooth(0.01f, 0.08f, hdr_fraction);
    // 2. Leave dark, indoor, night, and backlit shadow-dominated scenes neutral.
    //    Perceptual 18% diffuse gray (36.5 nits) sits around PQ 0.417.
    const float daylight_gate = zone_smooth(0.395f, 0.470f, mean_pq);
    // 3. Scale dimming smoothly with perceptual bright-area load above the SDR shoulder.
    const float pressure = std::sqrt(std::clamp(excess_pq / 0.085f, 0.f, 1.f));
    out.strength_ev = 1.50f * hdr_gate * daylight_gate * pressure;
    if (!out.active()) {
        out.strength_ev = 0.f;
        return out;
    }
    // Shift the midtone bell gently upward for very bright snow/cloud daylight scenes,
    // while keeping its minimum span wide enough to guarantee strict monotonicity.
    out.lo_pq = 0.25f;
    out.center_pq = std::clamp(0.52f + 0.55f * (mean_pq - 0.43f), 0.52f, 0.60f);
    out.hi_pq = std::clamp(out.center_pq + 0.26f, 0.78f, 0.86f);
    out.lo_y = float(sdr_auto_tone::inverse_pq(double(out.lo_pq)));
    out.hi_y = float(sdr_auto_tone::inverse_pq(double(out.hi_pq)));
    return out;
}

inline float auto_sdr_tone_weight(float p, const AutoSdrTone& tone) {
    if (p <= tone.lo_pq || p >= tone.hi_pq) return 0.f;
    if (p < tone.center_pq) return zone_smooth(tone.lo_pq, tone.center_pq, p);
    return 1.f - zone_smooth(tone.center_pq, tone.hi_pq, p);
}

inline void apply_auto_sdr_tone(float& r, float& g, float& b, const AutoSdrTone& tone,
                                const std::array<float, 3>& luma) {
    if (!tone.active()) return;
    r = std::max(r, 0.f);
    g = std::max(g, 0.f);
    b = std::max(b, 0.f);
    const float y = r * luma[0] + g * luma[1] + b * luma[2];
    if (!std::isfinite(y) || y <= tone.lo_y || y >= tone.hi_y) return;
    const float w = auto_sdr_tone_weight(float(sdr_auto_tone::pq(double(y))), tone);
    if (w <= 0.f) return;
    const float gain = std::exp2(-tone.strength_ev * w);
    if (!std::isfinite(gain) || gain <= 0.f) return;
    r *= gain;
    g *= gain;
    b *= gain;
}
