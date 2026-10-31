// Derived from LocalViewer; see NOTICE and COPYING. Android entry points removed.
/*
 * Camera RAW (DNG / CR2 / NEF / …).
 *
 * 64-bit: LibRaw demosaic → packed RGBA for the reader.
 *   present 0: 8-bit sRGB. Linear samples, then the camera look, then the sRGB transfer.
 *   present 1: deep color, Display P3 float16 clamped to 0..1, after the camera look.
 *   present 2: deep color + Android 16 HDR. Working space is linear Rec.2020, and
 *              the stored samples are linear scRGB: the HDR window composites there,
 *              and Rec.2020 numbers shown as scRGB look washed out. The render is the
 *              deep-color camera look, or with linear HDR the scene-linear samples
 *              (camera white = 1). One highlight gain then opens the top two stops
 *              below the sampled peak. The shoulder slider sets that opening for
 *              both HDR modes. The gain keeps the color from before the tone curve,
 *              whose shoulder otherwise turns highlights white.
 *              The advanced-color switch is not a third mode;
 *              the caller selects 2 whenever HDR display is on and the panel is HDR.
 * Every ABI: embedded JPEG preview for covers and for a failed demosaic.
 * 64-bit covers with no preview get a long-edge-512 demosaic JPEG.
 */








#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <csetjmp>
#include <functional>
#include <system_error>
#include <thread>
#include <vector>

#include "raw_camera_look.h"
#include "reader_pixels.h"
#include "hdr_parallel.h"

#if defined(EHVIEWER_HDR_CODECS)
#include <jpeglib.h>
#include "libraw/libraw.h"

struct RawJpegError {
    jpeg_error_mgr pub;
    jmp_buf jump;
};

// libjpeg's error_exit is a C function pointer. C++ linkage will not convert.
extern "C" void raw_still_jpeg_bail(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<RawJpegError*>(cinfo->err);
    longjmp(err->jump, 1);
}
#endif

namespace {

constexpr const char* kTag = "RawStill";
// Two heavy decodes may run at once, so keep each to a share of the big cores.
constexpr unsigned kPackMaxThreads = 4;
constexpr int kPackMinBandRows = 64;



uint16_t float_to_half(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((bits >> 23) & 0xff) - 127 + 15;
    uint32_t mant = bits & 0x7fffffu;
    if ((bits & 0x7fffffffu) > 0x7f800000u) return static_cast<uint16_t>(sign | 0x7e00u);
    if (((bits >> 23) & 0xff) == 0xff) return static_cast<uint16_t>(sign | 0x7c00u);
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        mant |= 0x800000u;
        uint32_t shift = static_cast<uint32_t>(1 - exp);
        uint32_t half_mant = mant >> (shift + 13);
        if ((mant >> (shift + 12)) & 1u) half_mant += 1u;
        return static_cast<uint16_t>(sign | half_mant);
    }
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
    uint16_t half = static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
    if (mant & 0x1000u) half = static_cast<uint16_t>(half + 1);
    return half;
}

struct Decoded {
    std::vector<uint8_t> pixels;
    int w = 0;
    int h = 0;
    int format = 0;
    int is_hdr = 0;
    int gamut = 0;
    float boost = 1.f;
};

void dst_size(int w, int h, int max_edge, int& dw, int& dh) {
    int long_edge = std::max(w, h);
    if (max_edge <= 0 || long_edge <= max_edge) {
        dw = w;
        dh = h;
        return;
    }
    double scale = static_cast<double>(max_edge) / static_cast<double>(long_edge);
    dw = std::max(1, static_cast<int>(std::lround(w * scale)));
    dh = std::max(1, static_cast<int>(std::lround(h * scale)));
}

float sample_component(const uint8_t* base, int bits, int colors, int x, int y, int w, int channel) {
    size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * static_cast<size_t>(colors);
    if (bits == 16) {
        uint16_t v = 0;
        std::memcpy(&v, base + i * 2 + static_cast<size_t>(channel) * 2, sizeof(v));
        return static_cast<float>(v);
    }
    return static_cast<float>(base[i + static_cast<size_t>(channel)]);
}

// The HDR buffer is linear Rec.2020.
inline float bt2020_luma(float r, float g, float b) { return 0.2627f * r + 0.6780f * g + 0.0593f * b; }

// 99.9th percentile of sampled BT.2020 luminance, in 16-bit codes. A percentile
// rather than the maximum, so a hot pixel or one specular glint does not set it.
float sampled_peak_luma(const uint8_t* base, int w, int h, int colors, int bits) {
    const int npx = w * h;
    int step = 1;
    while (npx / step > 50000) step *= 2;
    std::vector<float> ys;
    ys.reserve(static_cast<size_t>(npx / step + 1));
    const int ch = std::min(colors, 3);
    for (int i = 0; i < npx; i += step) {
        int y = i / w;
        int x = i - y * w;
        float r = sample_component(base, bits, colors, x, y, w, 0);
        float g = ch > 1 ? sample_component(base, bits, colors, x, y, w, 1) : r;
        float b = ch > 2 ? sample_component(base, bits, colors, x, y, w, 2) : r;
        ys.push_back(bt2020_luma(r, g, b));
    }
    if (ys.empty()) return 0.f;
    size_t idx = static_cast<size_t>(0.999 * static_cast<double>(ys.size() - 1));
    std::nth_element(ys.begin(), ys.begin() + static_cast<std::ptrdiff_t>(idx), ys.end());
    return ys[idx];
}

// Stops below the highlight anchor where the HDR gain starts.
constexpr float kHdrKneeStops = 2.f;

// 0 below the knee, 1 at and above the anchor. Smoothstep, so the slope matches
// at both ends.
float hdr_highlight_weight(float scene_luma, float anchor) {
    if (!(anchor > 0.f) || !std::isfinite(scene_luma)) return 0.f;
    const float knee = anchor * std::exp2(-kHdrKneeStops);
    if (!(scene_luma > knee)) return 0.f;
    const float t = std::min(1.f, std::log2(scene_luma / knee) / kHdrKneeStops);
    return t * t * (3.f - 2.f * t);
}

// HDR highlight gain shared by camera-look and linear HDR. Multiplies the SDR
// (or linear) render, so 1 keeps that render.
// scene_luma and anchor are scene luminance after exposure, camera white = 1.
// anchor is the sampled highlight peak, at least camera white. base_at_anchor is
// the render's output for a neutral at the anchor. Below the knee the gain is 1.
// It then rises with hdr_highlight_weight, and from the anchor up it holds.
// shoulder 0 keeps the render, 1 puts the anchor at the HDR target, and 0.5 is
// halfway between the two in stops. Both HDR modes use the shoulder slider.
float hdr_highlight_gain(float scene_luma, float anchor, float base_at_anchor, float shoulder, float cap) {
    const float w = hdr_highlight_weight(scene_luma, anchor);
    if (!(w > 0.f) || !(shoulder > 0.f) || !(base_at_anchor > 0.f)) return 1.f;
    const float amount = std::min(shoulder, 1.f);
    const float panel = std::isfinite(cap) && cap > 1.f ? std::min(cap, 10000.f / 203.f) : 1.f;
    const float peak_gain_stops = amount * std::log2(std::max(panel, base_at_anchor) / base_at_anchor);
    if (!(peak_gain_stops > 0.f)) return 1.f;
    const float gain = std::exp2(peak_gain_stops * w);
    return std::isfinite(gain) && gain > 0.f ? gain : 1.f;
}

// Linear Rec.2020 to linear scRGB (BT.709 primaries, D65, no adaptation).
// Same matrix as hdr_encode.h linear_bt2020_to_bt709. Values outside sRGB go
// negative or above the source peak; scRGB keeps both.
void rec2020_to_scrgb(float& r, float& g, float& b) {
    const float nr = 1.6604910021f * r + -0.5876411388f * g + -0.0728498633f * b;
    const float ng = -0.1245504745f * r + 1.1328998971f * g + -0.0083494226f * b;
    const float nb = -0.0181507634f * r + -0.1005788980f * g + 1.1187296614f * b;
    r = nr;
    g = ng;
    b = nb;
}

// Same mapping as RawPresent.rawLinearSample. scene_over_white is 1 at camera white.
// Highlight stops compress only above 1.
float raw_linear_sample(float scene_over_white, float exposure_ev, float highlight_stops, float cap) {
    if (!std::isfinite(scene_over_white) || !std::isfinite(exposure_ev) || !std::isfinite(highlight_stops) ||
        !std::isfinite(cap)) {
        return 0.f;
    }
    if (!(scene_over_white > 0.f)) return 0.f;
    const double base = static_cast<double>(scene_over_white) * std::exp2(static_cast<double>(exposure_ev));
    double y;
    if (!(highlight_stops > 0.f) || !(base > 1.0)) {
        y = base;
    } else {
        y = 1.0 + (base - 1.0) * std::exp2(-static_cast<double>(highlight_stops));
    }
    if (!std::isfinite(y) || y <= 0.0) return 0.f;
    const double limit = cap >= 1.f ? cap : 1.0;
    if (y > limit) y = limit;
    return static_cast<float>(y);
}

float zone_smooth(float edge0, float edge1, float x) {
    float t = (x - edge0) / (edge1 - edge0);
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// shadows, midtones, and highlights are −1..1. 0 leaves the pixel alone.
// Applied to the developed sample. The weight is taken from compressed luminance
// so a value above 1 still counts as a highlight.
void apply_raw_zones(float& r, float& g, float& b, float shadows, float midtones, float highlights) {
    shadows = std::isfinite(shadows) ? std::clamp(shadows, -1.f, 1.f) : 0.f;
    midtones = std::isfinite(midtones) ? std::clamp(midtones, -1.f, 1.f) : 0.f;
    highlights = std::isfinite(highlights) ? std::clamp(highlights, -1.f, 1.f) : 0.f;
    if (shadows == 0.f && midtones == 0.f && highlights == 0.f) return;
    r = std::max(r, 0.f);
    g = std::max(g, 0.f);
    b = std::max(b, 0.f);
    const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
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

bool pack_image(const libraw_processed_image_t* img, int max_edge, int present, float panel_boost,
                float exposure_ev, float highlight_stops, const rawlook::CameraLook* look, bool hdr_linear,
                float shoulder, float camera_white, float shadows, float midtones, float highlights, Decoded& out) {
    if (!img || img->width == 0 || img->height == 0) return false;
    if (img->type != LIBRAW_IMAGE_BITMAP) return false;
    const int sw = img->width;
    const int sh = img->height;
    const int colors = img->colors >= 1 ? img->colors : 1;
    const size_t bpp = img->bits == 16 ? 2u : 1u;
    const size_t need = static_cast<size_t>(sw) * static_cast<size_t>(sh) * static_cast<size_t>(colors) * bpp;
    if (img->data_size < need) return false;
    const bool deep = present != 0;
    const bool linear16 = img->bits == 16;
    // 8-bit input is an already-rendered preview bitmap. 16-bit input is linear.
    if (deep && !linear16) return false;
    if (!linear16 && img->bits != 8) return false;
    int dw = 0;
    int dh = 0;
    dst_size(sw, sh, max_edge, dw, dh);
    const size_t bytes = static_cast<size_t>(dw) * static_cast<size_t>(dh) * (deep ? 8u : 4u);
    if (bytes == 0 || bytes > 400u * 1024u * 1024u) return false;

    const bool hdr = present >= 2;
    const bool linear_hdr = hdr && hdr_linear;
    // 10000/203 matches hdr_encode.h kMaxLinear. HDR does not clamp to 1.
    const float panel = std::isfinite(panel_boost) ? std::clamp(panel_boost, 1.f, 64.f) : 1.f;
    const float cap = hdr ? std::min(panel, 10000.f / 203.f) : 1.f;
    // camera_white is the 16-bit code of the camera white level after LibRaw
    // has left highlight headroom in the buffer. 0 means full scale.
    const float full = linear16 ? 65535.f : 255.f;
    const float white = (linear16 && camera_white > 1.f && camera_white < full) ? camera_white : full;
    float exposure_gain = std::exp2(std::isfinite(exposure_ev) ? exposure_ev : 0.f);
    if (!std::isfinite(exposure_gain) || !(exposure_gain > 0.f)) exposure_gain = 1.f;
    // HDR gain inputs, in scene units after exposure (camera white = 1). The
    // anchor never drops below camera white, so a dim frame is not expanded.
    float anchor = 1.f;
    float base_at_anchor = 1.f;
    const float hdr_shoulder = std::isfinite(shoulder) ? std::clamp(shoulder, 0.f, 1.f) : 0.5f;
    if (hdr && linear16) {
        const float peak = sampled_peak_luma(img->data, sw, sh, colors, img->bits) / white * exposure_gain;
        if (std::isfinite(peak) && peak > 1.f) anchor = peak;
        base_at_anchor = look && !linear_hdr ? rawlook::tone_map(*look, anchor) : anchor;
    }

    // Export HDR retains the camera-look SDR render in the same gamut. Append
    // RGBA8 after the F16 plane so decode/encode share one JNI array and demosaic.
    const bool keep_sdr = present >= 3 && export_pixels_keep_sdr();
    const size_t sdr_bytes = keep_sdr ? static_cast<size_t>(dw) * dh * 4u : 0u;
    out.pixels.assign(bytes + sdr_bytes, 0);
    out.w = dw;
    out.h = dh;
    out.format = deep ? 1 : 0;
    out.is_hdr = hdr ? 1 : 0;
    // 0 is sRGB: 8-bit after the transfer, or HDR linear scRGB (values above 1, and
    // negatives for colors outside sRGB). 1 is linear Display P3, clamped to 0..1.
    out.gamut = present >= 3 ? 2 : (present == 1 ? 1 : 0);
    out.boost = 1.f;

    // Rows are independent and write disjoint output, so bands give the same bytes.
    auto pack_rows = [&](int y_begin, int y_end, float& peak) {
        for (int y = y_begin; y < y_end; ++y) {
            int y0 = y * sh / dh;
            int y1 = std::max(y0 + 1, (y + 1) * sh / dh);
            for (int x = 0; x < dw; ++x) {
                int x0 = x * sw / dw;
                int x1 = std::max(x0 + 1, (x + 1) * sw / dw);
                double acc[3] = {0, 0, 0};
                int count = 0;
                for (int sy = y0; sy < y1; ++sy) {
                    for (int sx = x0; sx < x1; ++sx) {
                        for (int c = 0; c < 3; ++c) {
                            int src_c = colors == 1 ? 0 : std::min(c, colors - 1);
                            acc[c] += sample_component(img->data, img->bits, colors, sx, sy, sw, src_c);
                        }
                        ++count;
                    }
                }
                if (count <= 0) continue;
                size_t o = (static_cast<size_t>(y) * static_cast<size_t>(dw) + static_cast<size_t>(x)) * (deep ? 8u : 4u);
                if (!linear16) {
                    for (int c = 0; c < 3; ++c) {
                        int v = static_cast<int>(std::lround(acc[c] / count));
                        out.pixels[o + static_cast<size_t>(c)] = static_cast<uint8_t>(std::clamp(v, 0, 255));
                    }
                    out.pixels[o + 3] = 255;
                } else {
                    double ch[3];
                    for (int c = 0; c < 3; ++c) ch[c] = acc[c] / count;
                    float rgb[3];
                    for (int c = 0; c < 3; ++c) rgb[c] = static_cast<float>(ch[c] / white);
                    const float scene_luma = bt2020_luma(rgb[0], rgb[1], rgb[2]) * exposure_gain;
                    // HDR runs the same look as deep color, so the body matches it.
                    // pre is that color before the tone curve.
                    float pre[3] = {rgb[0] * exposure_gain, rgb[1] * exposure_gain, rgb[2] * exposure_gain};
                    if (look && !linear_hdr) {
                        rawlook::apply_camera_look(*look, exposure_ev, rgb[0], rgb[1], rgb[2], pre);
                    } else {
                        for (int c = 0; c < 3; ++c) rgb[c] = pre[c];
                    }
                    if (keep_sdr) {
                        float base[3] = {rgb[0], rgb[1], rgb[2]};
                        apply_raw_zones(base[0], base[1], base[2], shadows, midtones, highlights);
                        const size_t sdr = bytes + (static_cast<size_t>(y) * dw + x) * 4u;
                        for (int c = 0; c < 3; ++c) {
                            out.pixels[sdr + c] = static_cast<uint8_t>(std::lround(rawlook::srgb_encode(base[c]) * 255.f));
                        }
                        out.pixels[sdr + 3] = 255;
                    }
                    if (hdr && look && !linear_hdr && hdr_shoulder > 0.f) {
                        // The curve's shoulder moves highlights toward white. Put the
                        // pre-curve color back across the same knee the gain opens,
                        // at the curved luminance, so the body is unchanged.
                        const float w = hdr_highlight_weight(scene_luma, anchor);
                        const float curved = bt2020_luma(rgb[0], rgb[1], rgb[2]);
                        const float before = bt2020_luma(pre[0], pre[1], pre[2]);
                        if (w > 0.f && before > 1e-4f && curved > 0.f) {
                            const float s = curved / before;
                            for (int c = 0; c < 3; ++c) {
                                rgb[c] = rgb[c] * (1.f - w) + std::max(pre[c], 0.f) * s * w;
                            }
                        }
                    }
                    apply_raw_zones(rgb[0], rgb[1], rgb[2], shadows, midtones, highlights);
                    if (hdr) {
                        // Ordinary HDR export follows source headroom. Explicit
                        // boost/custom export and reader mode use the HDR target
                        // passed by Kotlin.
                        const float target = present == 3 ? std::min(cap, anchor) : cap;
                        const float lift = hdr_highlight_gain(scene_luma, anchor, base_at_anchor, hdr_shoulder, target);
                        for (int c = 0; c < 3; ++c) rgb[c] *= lift;
                    }
                    if (!deep) {
                        for (int c = 0; c < 3; ++c) {
                            int v = static_cast<int>(std::lround(rawlook::srgb_encode(rgb[c]) * 255.f));
                            out.pixels[o + static_cast<size_t>(c)] = static_cast<uint8_t>(std::clamp(v, 0, 255));
                        }
                        out.pixels[o + 3] = 255;
                    } else {
                        // Exposure already ran inside the camera look, ahead of the tone curve.
                        // HDR compresses and caps the largest channel and scales the others with
                        // it, so a saturated highlight keeps its hue at the HDR target.
                        float hdr_scale = 1.f;
                        if (hdr) {
                            // Cap in Rec.2020, where 1 is camera white. An sRGB channel
                            // above 1 can be an ordinary saturated color, so the cap has
                            // to run before the scRGB conversion.
                            const float m = std::max(rgb[0], std::max(rgb[1], rgb[2]));
                            if (m > 0.f) hdr_scale = raw_linear_sample(m, 0.f, highlight_stops, cap) / m;
                            for (int c = 0; c < 3; ++c) rgb[c] = std::max(rgb[c], 0.f) * hdr_scale;
                            if (present < 3) rec2020_to_scrgb(rgb[0], rgb[1], rgb[2]);
                        }
                        for (int c = 0; c < 3; ++c) {
                            float v = hdr ? rgb[c] : raw_linear_sample(rgb[c], 0.f, highlight_stops, cap);
                            peak = std::max(peak, v);
                            uint16_t half = float_to_half(v);
                            out.pixels[o + static_cast<size_t>(c) * 2] = static_cast<uint8_t>(half & 0xff);
                            out.pixels[o + static_cast<size_t>(c) * 2 + 1] = static_cast<uint8_t>(half >> 8);
                        }
                        uint16_t alpha = 0x3c00;
                        out.pixels[o + 6] = static_cast<uint8_t>(alpha & 0xff);
                        out.pixels[o + 7] = static_cast<uint8_t>(alpha >> 8);
                    }
                }
            }
        }
    };

    const unsigned hw = std::thread::hardware_concurrency();
    int bands = static_cast<int>(jxrc_limit_threads(std::clamp(hw == 0 ? 1u : hw, 1u, kPackMaxThreads)));
    bands = std::min(bands, std::max(1, dh / kPackMinBandRows));
    std::vector<float> peaks(static_cast<size_t>(bands), 1.f);
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(bands - 1));
    for (int b = 1; b < bands; ++b) {
        const int y0 = static_cast<int>(static_cast<int64_t>(dh) * b / bands);
        const int y1 = static_cast<int>(static_cast<int64_t>(dh) * (b + 1) / bands);
        try {
            workers.emplace_back(pack_rows, y0, y1, std::ref(peaks[static_cast<size_t>(b)]));
        } catch (const std::system_error&) {
            pack_rows(y0, y1, peaks[static_cast<size_t>(b)]);
        }
    }
    pack_rows(0, static_cast<int>(static_cast<int64_t>(dh) / bands), peaks[0]);
    for (auto& w : workers) w.join();
    float peak = 1.f;
    for (float p : peaks) peak = std::max(peak, p);
    if (hdr) out.boost = peak;
    return true;
}

// 0 camera, 1 auto, 2 daylight, 3 cloudy, 4 shade, 5 tungsten, 6 fluorescent, 7 flash, 8 kelvin.
bool wb_coeff(const LibRaw& raw, int index, float mul[4]) {
    if (index < 0 || index >= 256) return false;
    const int* c = raw.imgdata.color.WB_Coeffs[index];
    if (c[0] <= 0 || c[1] <= 0 || c[2] <= 0) return false;
    mul[0] = static_cast<float>(c[0]);
    mul[1] = static_cast<float>(c[1]);
    mul[2] = static_cast<float>(c[2]);
    mul[3] = c[3] > 0 ? static_cast<float>(c[3]) : mul[1];
    return true;
}

bool wb_coeff_any(const LibRaw& raw, const int* ids, int count, float mul[4]) {
    for (int i = 0; i < count; ++i) {
        if (wb_coeff(raw, ids[i], mul)) return true;
    }
    return false;
}

// Daylight locus from 4000 K. Below that, the Planckian locus (Illuminant A at 2856 K).
bool kelvin_xy(float kelvin, double& x, double& y) {
    double t = std::clamp(static_cast<double>(kelvin), 1667.0, 25000.0);
    double t2 = t * t;
    double t3 = t2 * t;
    if (t >= 4000.0) {
        if (t <= 7000.0) {
            x = -4.6070e9 / t3 + 2.9678e6 / t2 + 0.09911e3 / t + 0.244063;
        } else {
            x = -2.0064e9 / t3 + 1.9018e6 / t2 + 0.24748e3 / t + 0.237040;
        }
        y = -3.000 * x * x + 2.870 * x - 0.275;
    } else {
        x = -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910;
        if (t <= 2222.0) {
            y = -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683;
        } else {
            y = -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867;
        }
    }
    return y > 1e-6;
}

bool mul_from_wbct(const float ct[64][5], float kelvin, float mul[4]) {
    struct Pt {
        float t;
        float m[4];
    };
    Pt pts[64];
    int n = 0;
    for (int i = 0; i < 64; ++i) {
        if (!(ct[i][0] > 1.f) || !(ct[i][1] > 0.f) || !(ct[i][2] > 0.f) || !(ct[i][3] > 0.f)) continue;
        pts[n].t = ct[i][0];
        pts[n].m[0] = ct[i][1];
        pts[n].m[1] = ct[i][2];
        pts[n].m[2] = ct[i][3];
        pts[n].m[3] = ct[i][4] > 0.f ? ct[i][4] : ct[i][2];
        ++n;
    }
    if (n < 2) return false;
    std::sort(pts, pts + n, [](const Pt& a, const Pt& b) { return a.t < b.t; });
    int w = 1;
    for (int i = 1; i < n; ++i) {
        if (pts[i].t > pts[w - 1].t + 0.5f) pts[w++] = pts[i];
    }
    n = w;
    if (n < 2) return false;
    auto copy_pt = [&](const Pt& p) {
        for (int c = 0; c < 4; ++c) mul[c] = p.m[c];
    };
    if (kelvin <= pts[0].t) {
        copy_pt(pts[0]);
        return mul[0] > 0.f;
    }
    if (kelvin >= pts[n - 1].t) {
        copy_pt(pts[n - 1]);
        return mul[0] > 0.f;
    }
    for (int i = 1; i < n; ++i) {
        if (kelvin > pts[i].t) continue;
        float m0 = 1e6f / pts[i - 1].t;
        float m1 = 1e6f / pts[i].t;
        float m = 1e6f / kelvin;
        float u = (m0 == m1) ? 0.f : (m0 - m) / (m0 - m1);
        u = std::clamp(u, 0.f, 1.f);
        for (int c = 0; c < 4; ++c) mul[c] = pts[i - 1].m[c] + (pts[i].m[c] - pts[i - 1].m[c]) * u;
        return mul[0] > 0.f;
    }
    return false;
}

bool mul_from_matrix(const LibRaw& raw, float kelvin, float mul[4]) {
    double x = 0;
    double y = 0;
    if (!kelvin_xy(kelvin, x, y)) return false;
    const auto& m = raw.imgdata.color.cam_xyz;
    double acc = 0;
    for (int c = 0; c < 3; ++c) acc += std::fabs(m[c][0]) + std::fabs(m[c][1]) + std::fabs(m[c][2]);
    if (acc < 0.01) return false;
    double X = x / y;
    double Y = 1.0;
    double Z = (1.0 - x - y) / y;
    int colors = raw.imgdata.idata.colors;
    for (int c = 0; c < 4; ++c) {
        int src = c;
        if (c == 3 && (colors < 4 || (std::fabs(m[3][0]) + std::fabs(m[3][1]) + std::fabs(m[3][2]) < 1e-6))) src = 1;
        double cam = m[src][0] * X + m[src][1] * Y + m[src][2] * Z;
        if (!(cam > 1e-4)) cam = 1e-4;
        mul[c] = static_cast<float>(1.0 / cam);
    }
    return mul[0] > 0.f;
}

void use_multipliers(LibRaw& raw, const float mul[4]) {
    auto& p = raw.imgdata.params;
    p.use_camera_wb = 0;
    p.use_auto_wb = 0;
    for (int i = 0; i < 4; ++i) p.user_mul[i] = mul[i];
}

bool apply_kelvin(LibRaw& raw, float kelvin) {
    float mul[4];
    if (mul_from_wbct(raw.imgdata.color.WBCT_Coeffs, kelvin, mul) || mul_from_matrix(raw, kelvin, mul)) {
        use_multipliers(raw, mul);
        return true;
    }
    auto& p = raw.imgdata.params;
    p.use_camera_wb = 0;
    p.use_auto_wb = 0;
    return false;
}

void configure_white_balance(LibRaw& raw, int wb_mode, int kelvin) {
    auto& p = raw.imgdata.params;
    for (int i = 0; i < 4; ++i) p.user_mul[i] = 0.f;
    float mul[4];
    switch (wb_mode) {
        case 1:
            p.use_camera_wb = 0;
            p.use_auto_wb = 1;
            break;
        case 2: {
            const int ids[] = {LIBRAW_WBI_Daylight, LIBRAW_WBI_D55, LIBRAW_WBI_D65};
            if (wb_coeff_any(raw, ids, 3, mul)) {
                use_multipliers(raw, mul);
            } else {
                p.use_camera_wb = 0;
                p.use_auto_wb = 0;
            }
            break;
        }
        case 3: {
            const int ids[] = {LIBRAW_WBI_Cloudy, LIBRAW_WBI_FineWeather};
            if (wb_coeff_any(raw, ids, 2, mul)) use_multipliers(raw, mul);
            else apply_kelvin(raw, 6000.f);
            break;
        }
        case 4:
            if (wb_coeff(raw, LIBRAW_WBI_Shade, mul)) use_multipliers(raw, mul);
            else apply_kelvin(raw, 7500.f);
            break;
        case 5: {
            const int ids[] = {LIBRAW_WBI_Tungsten, LIBRAW_WBI_StudioTungsten, LIBRAW_WBI_Ill_A};
            if (wb_coeff_any(raw, ids, 3, mul)) use_multipliers(raw, mul);
            else apply_kelvin(raw, 2850.f);
            break;
        }
        case 6: {
            const int ids[] = {LIBRAW_WBI_Fluorescent, LIBRAW_WBI_FL_D, LIBRAW_WBI_FL_N,
                               LIBRAW_WBI_FL_W,       LIBRAW_WBI_FL_WW, LIBRAW_WBI_FL_L};
            if (wb_coeff_any(raw, ids, 6, mul)) use_multipliers(raw, mul);
            else apply_kelvin(raw, 4000.f);
            break;
        }
        case 7:
            if (wb_coeff(raw, LIBRAW_WBI_Flash, mul)) use_multipliers(raw, mul);
            else apply_kelvin(raw, 5500.f);
            break;
        case 8:
            apply_kelvin(raw, static_cast<float>(std::clamp(kelvin, 2000, 12000)));
            break;
        default:
            p.use_camera_wb = 1;
            p.use_auto_wb = 0;
            break;
    }
}

// identify() copies an embedded color matrix only while opening.
// 3 copies it for any file. 1 copies it for DNG, and for other files only when
// camera white balance is already set. That flag is applied after open, so
// camera WB (mode 0) asks for 3 here. Other modes stay at 1 and keep Adobe's matrix.
void request_embedded_matrix(LibRaw& raw, int wb_mode) {
    raw.imgdata.params.use_camera_matrix = wb_mode == 0 ? 3 : 1;
}

// LibRaw stores the tag and never multiplies by it. -999 means absent.
float dng_baseline_ev(const LibRaw& raw) {
    const float ev = raw.imgdata.color.dng_levels.baseline_exposure;
    if (!std::isfinite(ev) || ev <= -100.f || ev >= 100.f) return 0.f;
    return ev;
}

void configure_process(LibRaw& raw, int present, int max_edge, int demosaic_qual, int wb_mode, int kelvin) {
    auto& p = raw.imgdata.params;
    configure_white_balance(raw, wb_mode, kelvin);
    // identify() already copied the matrix. Match the value set before open.
    p.use_camera_matrix = wb_mode == 0 ? 3 : 1;
    p.user_qual = demosaic_qual;
    p.half_size = 0;
    // Histogram stretch and a lowered white point both brighten past as-shot.
    p.no_auto_bright = 1;
    p.bright = 1.f;
    p.adjust_maximum_thr = 0.f;
    // Preview and cover requests stay at or under 4096. The visible size is known
    // after open, before unpack. Under 24 MP stays full size. Above that, half size.
    // View original passes a larger edge and stays full size.
    if (max_edge > 0 && max_edge <= 4096) {
        const auto& sz = raw.imgdata.sizes;
        int64_t pixels = static_cast<int64_t>(sz.width) * static_cast<int64_t>(sz.height);
        if (pixels <= 0) {
            pixels = static_cast<int64_t>(sz.raw_width) * static_cast<int64_t>(sz.raw_height);
        }
        if (pixels > 24000000LL) p.half_size = 1;
    }
    // 0 flattens every channel that reaches the white level to the same code,
    // before exposure, so later EV cannot bring the detail back. 2 keeps that
    // headroom and blends the clipped channel instead of leaving it pink.
    p.highlight = 2;
    p.gamm[0] = 1.0;
    p.gamm[1] = 1.0;
    if (present == 0) {
        p.output_bps = 16;
        p.output_color = 1;  // linear sRGB. The sRGB transfer is applied after the camera look.
    } else {
        p.output_bps = 16;
        p.output_color = present >= 2 ? 8 : 7;  // Rec.2020 or DCI-P3 D65
    }
}

// LibRaw maps camera white to 65535 and clips anything above it. Samples between
// the camera white level and the real sensor peak are still in the file; this
// raises the white level so they survive in the 16-bit image. The scale-colors
// hook is protected, so only a subclass can install it.
struct HeadroomRaw : LibRaw {
    float sat_fit = 1.f;
    HeadroomRaw() { callbacks.pre_scalecolors_cb = &HeadroomRaw::keep_sensor_headroom; }

    static void keep_sensor_headroom(void* ctx) {
        auto* raw = static_cast<HeadroomRaw*>(ctx);
        if (!raw) return;
        unsigned maximum = raw->imgdata.color.maximum;
        unsigned data_max = raw->imgdata.color.data_maximum;
        if (maximum == 0 || data_max <= maximum) return;
        unsigned cap = maximum > (std::numeric_limits<unsigned>::max() / 8u) ? std::numeric_limits<unsigned>::max()
                                                                              : maximum * 8u;
        unsigned raised = std::min(data_max, cap);
        if (raised <= maximum) return;
        raw->sat_fit = static_cast<float>(raised) / static_cast<float>(maximum);
        raw->imgdata.color.maximum = raised;
    }
};

// highlight 2 scales to the strongest white-balance channel, which darkens the
// frame versus highlight 0. pre_mul was divided by that peak, so the span is
// 1/min(pre_mul). Combined with the raised white level, this is how far camera
// white sits below the 16-bit ceiling.
float highlight_fit(const LibRaw& raw, float sat_fit) {
    float dmin = 1e30f;
    bool any = false;
    for (int c = 0; c < 4; ++c) {
        float v = raw.imgdata.color.pre_mul[c];
        if (!(v > 0.f) || !std::isfinite(v)) continue;
        dmin = std::min(dmin, v);
        any = true;
    }
    float wb_fit = 1.f;
    if (any && dmin > 0.f && dmin < 1.f) wb_fit = 1.f / dmin;
    if (!std::isfinite(wb_fit) || wb_fit < 1.f) wb_fit = 1.f;
    if (wb_fit > 8.f) wb_fit = 8.f;
    float sat = std::isfinite(sat_fit) && sat_fit > 1.f ? sat_fit : 1.f;
    if (sat > 8.f) sat = 8.f;
    float fit = wb_fit * sat;
    if (!std::isfinite(fit) || fit < 1.f) return 1.f;
    return fit;
}

bool process_open_raw(HeadroomRaw& raw, const uint8_t* tiff, size_t tiff_len, int max_edge, int present, float panel_boost,
                      int demosaic_qual, int wb_mode, float exposure_ev, float highlight_stops, int kelvin,
                      bool hdr_linear, bool camera_look, float shoulder, float shadows, float midtones, float highlights,
                      Decoded& out) {
    configure_process(raw, present, max_edge, demosaic_qual, wb_mode, kelvin);
    if (raw.unpack() != LIBRAW_SUCCESS) return false;
    if (raw.dcraw_process() != LIBRAW_SUCCESS) return false;
    const float fit = highlight_fit(raw, raw.sat_fit);
    const float camera_white = fit > 1.f ? 65535.f / fit : 65535.f;
    int err = 0;
    libraw_processed_image_t* img = raw.dcraw_make_mem_image(&err);
    if (!img || err != LIBRAW_SUCCESS) {
        if (img) LibRaw::dcraw_clear_mem(img);
        return false;
    }
    // Linear HDR skips the camera look, and so does SDR when the look is off.
    // Baseline exposure lives inside the look, so those paths skip it too.
    // HDR still uses the look unless linear HDR is on.
    const bool linear_hdr = present >= 2 && hdr_linear;
    const bool skip_look = linear_hdr || (present < 2 && !camera_look);
    const float user_ev = std::isfinite(exposure_ev) ? exposure_ev : 0.f;
    const float pack_ev = skip_look ? user_ev : user_ev + dng_baseline_ev(raw);
    int space = present >= 2 ? 2 : (present == 1 ? 1 : 0);
    float temp = 5500.f;
    if (wb_mode == 8) {
        temp = static_cast<float>(kelvin);
    } else {
        float measured = raw.imgdata.makernotes.common.CameraTemperature;
        if (std::isfinite(measured) && measured >= 1667.f && measured <= 25000.f) temp = measured;
    }
    rawlook::CameraLook look;
    const rawlook::CameraLook* look_ptr = nullptr;
    if (!skip_look) {
        rawlook::prepare_camera_look(tiff, tiff_len, temp, space, look);
        look_ptr = &look;
    }
    const float shoulder_amount = std::isfinite(shoulder) ? std::clamp(shoulder, 0.f, 1.f) : 0.5f;
    bool ok = pack_image(img, max_edge, present, panel_boost, pack_ev, highlight_stops, look_ptr, linear_hdr,
                          shoulder_amount, camera_white, shadows, midtones, highlights, out);
    LibRaw::dcraw_clear_mem(img);
    return ok;
}


} // namespace
#include "jxrc/converter.hpp"
int decode_raw(const uint8_t* data, size_t size, const jxrc::RawControls& controls,
               bool hdr, unsigned max_edge, std::vector<uint8_t>& pixels,
               unsigned& w, unsigned& h, int& gamut) {
    HeadroomRaw raw;
    request_embedded_matrix(raw, controls.white_balance);
    if (raw.open_buffer(data,size)!=LIBRAW_SUCCESS) return -1;
    if (uint64_t(raw.imgdata.sizes.width)*raw.imgdata.sizes.height>80000000) return -2;
    Decoded result;
    if (!process_open_raw(raw,data,size,max_edge,hdr?(controls.hdr_mode==jxrc::RawHdrMode::source?3:4):1,
        (controls.hdr_mode==jxrc::RawHdrMode::source?10000.f:float(controls.target_nits))/203.f,
        3,controls.white_balance,controls.exposure,controls.highlight_stops,controls.kelvin,
        controls.linear_hdr,controls.camera_look,controls.shoulder,controls.shadows,
        controls.midtones,controls.highlights,result)) return -3;
    w=result.w; h=result.h; gamut=result.gamut; pixels=std::move(result.pixels);
    return 0;
}
