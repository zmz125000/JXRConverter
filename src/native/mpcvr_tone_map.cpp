// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 adipose and the MPC Video Renderer contributors.
// CPU adaptation of the HDR-to-SDR Metadata spline.
// https://github.com/zmz125000/VideoRenderer/blob/70811bc/Shaders/convert/hdr_tone_mapping_spline.hlsl
// Modified for JXRConverter still-image export on 2026-11-20.

#include "codec_api.h"
#include "skiv_tone_map.h"

// Input and output use 203 nits per unit.
// Both are linear RGB in the encoder's gamut.
std::vector<float> tone_map_mpcvr_base(std::vector<float> image, uhdr_color_gamut_t gamut, double source_peak) {
    using skiv::Matrix;
    using skiv::RGB;
    using skiv::transform;
    constexpr double sdr_white = kSdrWhiteNits;
    constexpr double flat_at = 4000.0;
    constexpr Matrix rgb_to_lms = {{{.412109375, .523925781, .063964844},
        {.166748047, .720458984, .112792969}, {.024169922, .075439453, .900390625}}};
    constexpr Matrix lms_to_rgb = {{{3.436606694, -2.506452119, .069845424},
        {-.791329556, 1.983600452, -.192270896}, {-.025949900, -.098913715, 1.124863614}}};
    const auto& to_xyz = gamut == UHDR_CG_BT_2100 ? skiv::rgb2020_xyz :
        gamut == UHDR_CG_DISPLAY_P3 ? skiv::rgbp3_xyz : skiv::rgb709_xyz;
    const auto from_xyz = skiv::inverse(to_xyz);
    const auto xyz_to_bt2020 = skiv::inverse(skiv::rgb2020_xyz);
    const auto input_to_bt2020 = skiv::multiply(xyz_to_bt2020, to_xyz);
    const auto bt2020_to_display = skiv::multiply(from_xyz, skiv::rgb2020_xyz);
    const auto to_display = [&](const RGB& rgb) {
        return gamut == UHDR_CG_BT_2100 ? rgb : transform(bt2020_to_display, rgb);
    };
    const auto pq_nits = [](double nits) { return skiv::pq(nits * (1.0 / 80.0)); };
    if (source_peak <= sdr_white) return image;
    source_peak = std::min(source_peak, double(kMaxNits));
    const double white = pq_nits(sdr_white);
    const double knee = std::max(1.5 * white - 0.5, 0.0);
    const double width = pq_nits(std::max(source_peak, flat_at)) - knee;
    const double inv_width = 1.0 / width;
    double endpoint = white;
    if (source_peak < flat_at) {
        const double t = (pq_nits(source_peak) - knee) * inv_width;
        const double blend = t * t * (3.0 - 2.0 * t);
        endpoint = (white - (1.0 - blend) * knee - width * t * (1.0 - t) * (1.0 - t)) / blend;
    }
    const double knee_nits = 80.0 * skiv::inverse_pq(knee);
    const auto spline = [&](double nits) {
        if (nits <= knee_nits) return nits;
        const double encoded = pq_nits(nits);
        const double t = std::clamp((encoded - knee) * inv_width, 0.0, 1.0);
        const double omt = 1.0 - t;
        const double blend = t * t * (3.0 - 2.0 * t);
        const double mapped = knee + (endpoint - knee) * blend + width * t * omt * omt;
        return 80.0 * skiv::inverse_pq(std::min(mapped, white));
    };
    const auto max_channel = [](const RGB& rgb) { return std::max({rgb[0], rgb[1], rgb[2]}); };
    const size_t pixels = image.size() / 4;
    hdr_parallel_pixels(pixels, hdr_pixel_jobs(pixels), [&](size_t begin, size_t end, unsigned) {
        for (size_t pixel = begin; pixel < end; ++pixel) {
            float* rgba = image.data() + pixel * 4;
            if (!std::isfinite(rgba[3]) || rgba[3] <= 0.f) {
                std::fill_n(rgba, 3, 0.f);
                continue;
            }
            RGB rgb{};
            for (unsigned c = 0; c < 3; ++c) {
                rgb[c] = std::isfinite(rgba[c]) ? double(rgba[c]) * kSdrWhiteNits : 0.0;
            }
            auto bt2020 = gamut == UHDR_CG_BT_2100 ? rgb : transform(input_to_bt2020, rgb);
            for (double& channel : bt2020) channel = std::clamp(channel, 0.0, 10000.0);
            const double y = .2627 * bt2020[0] + .6780 * bt2020[1] + .0593 * bt2020[2];
            RGB mapped = bt2020;
            if (y > 1e-6) {
                const double low = std::min({bt2020[0], bt2020[1], bt2020[2]});
                const double high = std::max({bt2020[0], bt2020[1], bt2020[2]});
                const double pq_high = pq_nits(high);
                const double saturation = pq_high > 0.0 ? std::clamp(1.0 - pq_nits(low) / pq_high, 0.0, 1.0) : 0.0;
                const double ratio = std::max(high / y, 1.0);
                const double level = (ratio > 1.0 && saturation > 0.0)
                    ? y * double(std::pow(float(ratio), float(saturation))) : y;
                const double scale = spline(level) / level;
                RGB scaled = {bt2020[0] * scale, bt2020[1] * scale, bt2020[2] * scale};

                auto lms = transform(rgb_to_lms, bt2020);
                for (double& channel : lms) channel = spline(std::max(channel, 0.0));
                const auto channels = transform(lms_to_rgb, lms);
                const double overshoot = std::max(max_channel(to_display(scaled)) * (1.0 / sdr_white), 1.0);
                const double mix = overshoot > 1.0
                    ? 1.0 - (1.0 - .2003) * double(std::pow(float(overshoot), -2.73f))
                    : .2003;
                for (unsigned c = 0; c < 3; ++c) {
                    mapped[c] = scaled[c] + (std::max(channels[c], 0.0) - scaled[c]) * mix;
                }
            }
            const auto display = to_display(mapped);
            const double inv_norm = 1.0 / std::max(sdr_white, max_channel(display));
            // The selected encoder applies the SDR transfer and color tags.
            for (unsigned c = 0; c < 3; ++c) {
                rgba[c] = float(std::clamp(display[c] * inv_norm, 0.0, 1.0));
            }
        }
    });
    return image;
}
