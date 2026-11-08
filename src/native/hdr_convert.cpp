#include "working_pixels.h"
// Derived from LocalViewer; see NOTICE and COPYING. Android entry points removed.
/*
 * JPEG XR / HDR raster → Ultra HDR JPEG via libultrahdr.
 * JNI: com.hippo.ehviewer.jni.HdrConvertKt
 */




#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "hdr_encode.h"
#include "jxrc/converter.hpp"
#include <stdexcept>
#include "reader_pixels.h"
#include "hdr_parallel.h"
#include "ultrahdr_api.h"
// Internal libultrahdr: build Display P3 / 709 ICC for baseline JPEG APP2.
#include "ultrahdr/icc.h"
// skcms (libjxl third_party) — honor JXR embedded color context (ICC).
#include "skcms.h"
#include "linear_color_profile.h"
// ultrahdrcommon.h may stub ALOG* to no-ops — restore Android logging for this TU.
#ifdef ALOGE
#undef ALOGE
#endif
#ifdef ALOGI
#undef ALOGI
#endif

// libjpeg-turbo (built as libultrahdr dep) — baseline SDR JPEG only.
extern "C" {
#include "jpeglib.h"
}
#include <csetjmp>
#include <cstdio>
#include <cstdlib>

// jxrlib (Microsoft / brion jpegxr packaging)
extern "C" {
#include "JXRGlue.h"
}
// JXRGlue.h defines min/max as macros (Windows-style) — breaks std::min/max.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#define LOG_TAG "HdrConvert"

// Lossless/high/mid/low share 4:4:4 sampling; JPEG itself is always lossy.
static int export_jpeg_quality(StillEncodeQuality quality) {
    return quality.jpeg();
}
#define ALOGE(...) ((void)0)
#define ALOGI(...) ((void)0)

namespace {

// jxrlib JXRGlue.h defines min/max macros — avoid std::max/min after that include.
static inline float fmax3(float a, float b, float c) {
    float m = a > b ? a : b;
    return m > c ? m : c;
}

}  // namespace

// Shared with jxl_hdr thumbs (declared in hdr_encode.h). Uses local half→float.
float scan_scrgb_peak(const uint16_t* rgba, size_t pixel_count) {
    if (!rgba || pixel_count == 0) return 1.0f;

    auto half_f = [](uint16_t h) -> float {
        const uint32_t sign = (static_cast<uint32_t>(h) & 0x8000u) << 16;
        uint32_t exp = (h >> 10) & 0x1fu;
        uint32_t mant = h & 0x3ffu;
        uint32_t out;
        if (exp == 0) {
            if (mant == 0) {
                out = sign;
            } else {
                exp = 1;
                while ((mant & 0x400u) == 0) {
                    mant <<= 1;
                    exp--;
                }
                mant &= 0x3ffu;
                out = sign | ((127 - 15 + exp) << 23) | (mant << 13);
            }
        } else if (exp == 31) {
            out = sign | 0x7f800000u | (mant << 13);
        } else {
            out = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
        }
        union {
            uint32_t u;
            float f;
        } v{out};
        return v.f;
    };
    auto mx3 = [](float a, float b, float c) {
        float m = a > b ? a : b;
        return m > c ? m : c;
    };

    constexpr double kPct = 0.9999;
    constexpr int kNitBins = 10001;
    std::vector<uint32_t> nit_counts(static_cast<size_t>(kNitBins), 0);
    int raw_max_nits = 0;
    uint64_t valid = 0;

    for (size_t i = 0; i < pixel_count; i++) {
        const float r = half_f(rgba[i * 4 + 0]);
        const float g = half_f(rgba[i * 4 + 1]);
        const float b = half_f(rgba[i * 4 + 2]);
        float m = mx3(r, g, b);
        if (!std::isfinite(m) || m <= 0.f) continue;
        valid++;
        float nits_f = m * kSdrWhiteNits;
        if (nits_f > kMaxNits) nits_f = kMaxNits;
        int nits = static_cast<int>(std::lround(nits_f));
        if (nits < 0) nits = 0;
        if (nits >= kNitBins) nits = kNitBins - 1;
        nit_counts[static_cast<size_t>(nits)]++;
        if (nits > raw_max_nits) raw_max_nits = nits;
    }
    if (valid == 0) return 1.0f;

    const uint64_t count_target =
        static_cast<uint64_t>(std::llround((1.0 - kPct) * static_cast<double>(valid)));
    const uint64_t need = count_target < 1 ? 1 : count_target;
    uint64_t count = 0;
    int maxcll_nits = raw_max_nits;
    for (int idx = raw_max_nits; idx >= 0; idx--) {
        count += nit_counts[static_cast<size_t>(idx)];
        if (count >= need) {
            maxcll_nits = idx;
            break;
        }
    }

    float peak = static_cast<float>(maxcll_nits) / kSdrWhiteNits;
    if (peak < 1.0f) peak = 1.0f;
    if (peak > kMaxLinear) peak = kMaxLinear;
    return peak;
}

namespace {

bool guid_eq(const PKPixelFormatGUID& a, const PKPixelFormatGUID& b) {
    return memcmp(&a, &b, sizeof(PKPixelFormatGUID)) == 0;
}

/**
 * JXR/WDP pixel families we expand to linear F16 RGBA.
 *
 * Microsoft default (no color context): integer → sRGB; half/float/fixed → scRGB.
 * With embedded color context, ICC takes priority (skcms after expand).
 *
 * HDR (linear scRGB, 1.0 ≈ paper white): half / float / 13.3 fixed-point.
 * SDR unsigned integer (sRGB-encoded by default): 8/16-bit RGB(A)/BGR(A)/gray + RGB101010.
 * Direct display of untagged linear scRGB keeps samples that already fit the panel
 * and rolls only the clipping tail into the top stop. Ultra HDR encode keeps the
 * original codes so the gain map can do that fit at display time.
 */
enum class JxrPix {
    RgbaF32,
    RgbaF32Prem,  // 128bppPRGBAFloat — must unpremultiply
    RgbaF16,
    Rgb101010,    // unsigned 10-bit; jxrlib R@20 G@10 B@0; default sRGB-encoded
    Fixed16,      // signed 13.3 fixed-point linear scRGB (HD Photo HDR)
    Bgr8,         // 24bppBGR / 32bppBGR (B,G,R[,X])
    Bgra8,        // 32bppBGRA
    Pbgra8,       // 32bppPBGRA (premultiplied)
    Rgb8,         // 24bppRGB
    Rgba8,        // 32bppRGBA / 32bppRGB (R,G,B[,A])
    Prgba8,       // 32bppPRGBA
    Rgb16,        // 48bppRGB (u16 gamma)
    Rgba16,       // 64bppRGBA / PRGBA (u16 gamma)
    Gray8,
    Gray16,
    Unsupported,
};

JxrPix classify_guid(const PKPixelFormatGUID& g) {
    // ── HDR linear (scRGB when untagged) ──────────────────────────────────
    if (guid_eq(g, GUID_PKPixelFormat128bppRGBAFloat)) return JxrPix::RgbaF32;
    if (guid_eq(g, GUID_PKPixelFormat128bppPRGBAFloat)) return JxrPix::RgbaF32Prem;
    if (guid_eq(g, GUID_PKPixelFormat128bppRGBFloat)) return JxrPix::RgbaF32;
    if (guid_eq(g, GUID_PKPixelFormat96bppRGBFloat)) return JxrPix::RgbaF32;
    if (guid_eq(g, GUID_PKPixelFormat64bppRGBAHalf)) return JxrPix::RgbaF16;
    if (guid_eq(g, GUID_PKPixelFormat64bppRGBHalf)) return JxrPix::RgbaF16;
    if (guid_eq(g, GUID_PKPixelFormat48bppRGBHalf)) return JxrPix::RgbaF16;
    if (guid_eq(g, GUID_PKPixelFormat48bppRGBFixedPoint)) return JxrPix::Fixed16;
    if (guid_eq(g, GUID_PKPixelFormat64bppRGBAFixedPoint)) return JxrPix::Fixed16;
    if (guid_eq(g, GUID_PKPixelFormat64bppRGBFixedPoint)) return JxrPix::Fixed16;
    // ── Unsigned integer (default sRGB-encoded; RGB101010 bit order per jxrlib PFC) ──
    if (guid_eq(g, GUID_PKPixelFormat32bppRGB101010)) return JxrPix::Rgb101010;
    if (guid_eq(g, GUID_PKPixelFormat24bppBGR)) return JxrPix::Bgr8;
    if (guid_eq(g, GUID_PKPixelFormat32bppBGR)) return JxrPix::Bgr8;
    if (guid_eq(g, GUID_PKPixelFormat32bppBGRA)) return JxrPix::Bgra8;
    if (guid_eq(g, GUID_PKPixelFormat32bppPBGRA)) return JxrPix::Pbgra8;
    if (guid_eq(g, GUID_PKPixelFormat24bppRGB)) return JxrPix::Rgb8;
    if (guid_eq(g, GUID_PKPixelFormat32bppRGB)) return JxrPix::Rgba8;
    if (guid_eq(g, GUID_PKPixelFormat32bppRGBA)) return JxrPix::Rgba8;
    if (guid_eq(g, GUID_PKPixelFormat32bppPRGBA)) return JxrPix::Prgba8;
    if (guid_eq(g, GUID_PKPixelFormat48bppRGB)) return JxrPix::Rgb16;
    if (guid_eq(g, GUID_PKPixelFormat64bppRGBA)) return JxrPix::Rgba16;
    if (guid_eq(g, GUID_PKPixelFormat64bppPRGBA)) return JxrPix::Rgba16;
    if (guid_eq(g, GUID_PKPixelFormat8bppGray)) return JxrPix::Gray8;
    if (guid_eq(g, GUID_PKPixelFormat16bppGray)) return JxrPix::Gray16;
    return JxrPix::Unsupported;
}

/**
 * Apply embedded JXR color context with its **full** ICC (TRC + matrix).
 *
 * WIC: pixel format does not define color space; color context takes priority.
 * Callers must expand numeric samples as **encoded** in the ICC (no default
 * sRGB EOTF / scRGB linear assumption). Do not strip TRC based on float vs int.
 *
 * Output is linear BT.2020 F16 (1.0 ≈ paper white).
 */
template<class Sample>
bool apply_jxr_color_context(std::vector<Sample>& rgba, size_t npx, const uint8_t* icc,
                             size_t icc_len) {
    if (!icc || icc_len < 128 || npx == 0) return false;
    skcms_ICCProfile src{};
    if (!skcms_Parse(icc, icc_len, &src)) {
        ALOGI("JXR ICC present (%zu B) but skcms_Parse failed", icc_len);
        return false;
    }
    skcms_ICCProfile dst{};
    if (!make_linear_bt2020_profile(&dst)) {
        ALOGI("JXR ICC: linear BT.2020 dest not usable");
        return false;
    }
    // Export transforms floats in place; legacy display callers retain half.
    // The float path preserves signed and extended-range color values.
    if (!skcms_Transform(rgba.data(), working_format<Sample>(), skcms_AlphaFormat_Unpremul, &src,
                         rgba.data(), working_format<Sample>(), skcms_AlphaFormat_Unpremul, &dst,
                         npx)) {
        ALOGI("JXR ICC skcms_Transform failed");
        return false;
    }
    for (size_t i = 0; i < npx; i++) {
        auto clamp01p = [](float v) { return sanitize_working<Sample>(v); };
        rgba[i * 4 + 0] = store_working<Sample>(clamp01p(read_working(rgba[i * 4 + 0])));
        rgba[i * 4 + 1] = store_working<Sample>(clamp01p(read_working(rgba[i * 4 + 1])));
        rgba[i * 4 + 2] = store_working<Sample>(clamp01p(read_working(rgba[i * 4 + 2])));
        float a = read_working(rgba[i * 4 + 3]);
        if (!std::isfinite(a) || a < 0.f) a = 0.f;
        if (a > 1.f) a = 1.f;
        rgba[i * 4 + 3] = store_working<Sample>(a);
    }
    ALOGI("JXR ICC applied via skcms (%zu B, full TRC)", icc_len);
    return true;
}

/**
 * Straight-alpha → composite on black, force opaque.
 * JPEG / libultrahdr read only RGB and ignore A; transparent edges otherwise show
 * pre-composite colors. Matches JXL path.
 */
template<class Sample>
void composite_straight_alpha_on_black(std::vector<Sample>& rgba, size_t npx) {
    for (size_t i = 0; i < npx; i++) {
        float a = read_working(rgba[i * 4 + 3]);
        if (!std::isfinite(a) || a < 0.f) a = 0.f;
        if (a > 1.f) a = 1.f;
        if (a >= 1.f) {
            rgba[i * 4 + 3] = store_working<Sample>(1.f);
            continue;
        }
        float r = read_working(rgba[i * 4 + 0]);
        float g = read_working(rgba[i * 4 + 1]);
        float b = read_working(rgba[i * 4 + 2]);
        if (!std::isfinite(r) || r < 0.f) r = 0.f;
        if (!std::isfinite(g) || g < 0.f) g = 0.f;
        if (!std::isfinite(b) || b < 0.f) b = 0.f;
        r *= a;
        g *= a;
        b *= a;
        if (r > kMaxLinear) r = kMaxLinear;
        if (g > kMaxLinear) g = kMaxLinear;
        if (b > kMaxLinear) b = kMaxLinear;
        rgba[i * 4 + 0] = store_working<Sample>(r);
        rgba[i * 4 + 1] = store_working<Sample>(g);
        rgba[i * 4 + 2] = store_working<Sample>(b);
        rgba[i * 4 + 3] = store_working<Sample>(1.f);
    }
}

/** Log GUID for Unsupported diagnostics (family last byte is often enough). */
void log_unsupported_guid(const PKPixelFormatGUID& g) {
    const auto* b = reinterpret_cast<const uint8_t*>(&g);
    ALOGE("Unsupported JXR pixel format GUID "
          "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x "
          "(last=0x%02x)",
          b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12],
          b[13], b[14], b[15], b[15]);
}

/** sRGB-encoded channel [0,1] → linear F16. */
inline uint16_t enc_to_lin_half(float enc) {
    return float_to_half(srgb_eotf(enc));
}

/** HD Photo 13.3 signed fixed-point: 1.0 linear = 0x2000 (8192). */
inline float fixed13_3_to_float(int16_t v) {
    return static_cast<float>(v) * (1.0f / 8192.0f);
}

/**
 * Configure decoder in the given native pixel format, optionally at thumbnail scale.
 * Ported from branch `hdr` jxr_hdr.c — never force half→float via jxrlib Convert
 * (that path produces vertical stripe garbage).
 */
bool setup_full_frame(PKImageDecode* dec, PKPixelFormatGUID* fmt, unsigned max_edge = 0) {
    PKPixelInfo pi{};
    pi.pGUIDPixFmt = fmt;
    if (PixelFormatLookup(&pi, LOOKUP_FORWARD) != WMP_errSuccess) return false;

    if (!!(pi.grBit & PK_pixfmtHasAlpha)) {
        dec->WMP.wmiSCP.uAlphaMode = 2; /* image + alpha */
    } else {
        dec->WMP.wmiSCP.uAlphaMode = 0;
    }

    dec->WMP.wmiI.cfColorFormat = pi.cfColorFormat;
    dec->WMP.wmiI.bdBitDepth = pi.bdBitDepth;
    dec->WMP.wmiI.cBitsPerUnit = pi.cbitUnit;
    dec->WMP.wmiI.bRGB = !(pi.grBit & PK_pixfmtBGR);

    // jxrlib supports power-of-two transform-domain reduction. Pick the largest
    // reduction that still supplies at least the requested edge, then let the
    // existing linear resampler finish the fit. Copy_WMP divides a 16-line
    // macroblock by this scale, so never request a scale greater than 16.
    const size_t width = dec->WMP.wmiI.cWidth;
    const size_t height = dec->WMP.wmiI.cHeight;
    const size_t long_edge = std::max(width, height);
    unsigned scale = 1;
    while (max_edge > 0 && scale < 16 &&
           (long_edge + scale * 2 - 1) / (scale * 2) >= max_edge &&
           // Both axes must shrink at each step: jxrlib infers one scale
           // from the requested dimensions, including on very thin images.
           (width + scale * 2 - 1) / (scale * 2) < (width + scale - 1) / scale &&
           (height + scale * 2 - 1) / (scale * 2) < (height + scale - 1) / scale) {
        scale *= 2;
    }
    dec->WMP.wmiI.cThumbnailWidth = (width + scale - 1) / scale;
    dec->WMP.wmiI.cThumbnailHeight = (height + scale - 1) / scale;
    // The decoder honors this only on frequency-organized streams, where the
    // discarded refinement bits cannot contribute useful thumbnail detail.
    dec->WMP.wmiI.bSkipFlexbits = scale > 1 ? TRUE : FALSE;
    dec->WMP.wmiI.cROILeftX = 0;
    dec->WMP.wmiI.cROITopY = 0;
    dec->WMP.wmiI.cROIWidth = dec->WMP.wmiI.cThumbnailWidth;
    dec->WMP.wmiI.cROIHeight = dec->WMP.wmiI.cThumbnailHeight;
    dec->WMP.wmiI.oOrientation = O_NONE;
    dec->WMP.wmiI.cPostProcStrength = 0;
    return true;
}

/**
 * Decode JXR from an in-memory buffer (no temp file).
 * Same approach as branch `hdr` HdrJxr: CreateStreamFromMemory + native pixel format.
 * Input can be backed by a Java byte array or a read-only mapped file.
 *
 * @param composite_alpha_on_black true for JPEG/UHDR (no alpha plane) — flatten
 *   straight alpha onto black. false for direct Bitmap display — keep A.
 */
template<class Sample>
bool decode_jxr_from_memory(const uint8_t* data, size_t len, std::vector<Sample>& out_rgba,
                            unsigned& w, unsigned& h, bool composite_alpha_on_black = true,
                            bool* out_untagged_linear = nullptr, uhdr_color_gamut_t* out_cg = nullptr,
                            unsigned thumbnail_edge = 0, int* out_source_depth = nullptr, int* out_native_kind = nullptr) {
    if (out_untagged_linear) *out_untagged_linear = false;
    if (out_cg) *out_cg = UHDR_CG_BT_709;
    if (!data || len == 0) return false;

    PKFactory* factory = nullptr;
    PKCodecFactory* codecFactory = nullptr;
    struct WMPStream* stream = nullptr;
    PKImageDecode* decoder = nullptr;
    bool ok = false;

    auto release_all = [&]() {
        if (decoder) {
            decoder->Release(&decoder);
            decoder = nullptr;
        }
        if (stream) {
            stream->Close(&stream);
            stream = nullptr;
        }
        if (codecFactory) {
            codecFactory->Release(&codecFactory);
            codecFactory = nullptr;
        }
        if (factory) {
            factory->Release(&factory);
            factory = nullptr;
        }
    };

    ERR err = PKCreateFactory(&factory, PK_SDK_VERSION);
    if (Failed(err) || !factory) {
        ALOGE("PKCreateFactory failed: %d", err);
        return false;
    }
    err = PKCreateCodecFactory(&codecFactory, WMP_SDK_VERSION);
    if (Failed(err) || !codecFactory) {
        ALOGE("PKCreateCodecFactory failed: %d", err);
        release_all();
        return false;
    }
    err = factory->CreateStreamFromMemory(&stream, const_cast<uint8_t*>(data), len);
    if (Failed(err) || !stream) {
        ALOGE("CreateStreamFromMemory failed: %d", err);
        release_all();
        return false;
    }
    err = PKImageDecode_Create_WMP(&decoder);
    if (Failed(err) || !decoder) {
        ALOGE("PKImageDecode_Create_WMP failed: %d", err);
        release_all();
        return false;
    }
    err = decoder->Initialize(decoder, stream);
    if (Failed(err)) {
        ALOGE("decoder Initialize failed: %d", err);
        release_all();
        return false;
    }
    /* Stream owned by decoder after Initialize (hdr branch). */
    decoder->fStreamOwner = 1;
    stream = nullptr;

    I32 width = 0, height = 0;
    err = decoder->GetSize(decoder, &width, &height);
    if (Failed(err) || width <= 0 || height <= 0 || uint64_t(width)*height>80000000) {
        ALOGE("GetSize failed/out of range: %d %dx%d", err, (int)width, (int)height);
        release_all();
        return false;
    }

    PKPixelFormatGUID guid{};
    err = decoder->GetPixelFormat(decoder, &guid);
    if (Failed(err)) {
        ALOGE("GetPixelFormat failed: %d", err);
        release_all();
        return false;
    }
    JxrPix pix = classify_guid(guid);
    if(out_native_kind)*out_native_kind=pix==JxrPix::RgbaF16?1:pix==JxrPix::RgbaF32?2:pix==JxrPix::Fixed16?3:0;
    if (out_source_depth) {
        switch (pix) {
            case JxrPix::RgbaF32: case JxrPix::RgbaF32Prem: *out_source_depth = 32; break;
            case JxrPix::RgbaF16: case JxrPix::Fixed16: case JxrPix::Rgb16:
            case JxrPix::Rgba16: case JxrPix::Gray16: *out_source_depth = 16; break;
            case JxrPix::Rgb101010: *out_source_depth = 10; break;
            case JxrPix::Unsupported: *out_source_depth = 0; break;
            default: *out_source_depth = 8; break;
        }
    }
    if (pix == JxrPix::Unsupported) {
        log_unsupported_guid(guid);
        release_all();
        return false;
    }

    // Embedded color context (ICC) takes priority over pixel-format defaults (WIC).
    std::vector<uint8_t> color_ctx;
    {
        U32 cb = 0;
        ERR cc = decoder->GetColorContext(decoder, nullptr, &cb);
        if (!Failed(cc) && cb > 0 && cb < (16u * 1024u * 1024u)) {
            color_ctx.resize(cb);
            cc = decoder->GetColorContext(decoder, color_ctx.data(), &cb);
            if (Failed(cc) || cb == 0) {
                color_ctx.clear();
            } else {
                color_ctx.resize(cb);
            }
        }
    }
    // WIC: valid color context wins over integer→sRGB / float→scRGB inference.
    // Any parseable ICC (including ≈sRGB) is applied with its full TRC — do not
    // assume float storage is linear when a context is present.
    bool have_valid_icc = false;
    if (!color_ctx.empty()) {
        skcms_ICCProfile probe{};
        have_valid_icc = skcms_Parse(color_ctx.data(), color_ctx.size(), &probe);
        if (!have_valid_icc) {
            ALOGI("JXR color context %zu B unparsable — Microsoft default inference",
                  color_ctx.size());
        }
    }
    // Expand numeric samples as encoded for skcms; without ICC use default TF.
    const bool expand_as_encoded = have_valid_icc;

    /* Decode in native format — no jxrlib format conversion (half→float Convert
     * produces vertical stripe garbage). Integer SDR expands below with sRGB EOTF. */
    if (!setup_full_frame(decoder, &guid, thumbnail_edge)) {
        ALOGE("setup_full_frame failed");
        release_all();
        return false;
    }

    w = static_cast<unsigned>(decoder->WMP.wmiI.cROIWidth);
    h = static_cast<unsigned>(decoder->WMP.wmiI.cROIHeight);
    if (w == 0 || h == 0) {
        w = static_cast<unsigned>(width);
        h = static_cast<unsigned>(height);
    }

    const bool rgb_half = guid_eq(guid, GUID_PKPixelFormat64bppRGBHalf) ||
        guid_eq(guid, GUID_PKPixelFormat48bppRGBHalf);
    const bool rgba_half = guid_eq(guid, GUID_PKPixelFormat64bppRGBAHalf);
    const bool rgb_float = guid_eq(guid, GUID_PKPixelFormat128bppRGBFloat) ||
        guid_eq(guid, GUID_PKPixelFormat96bppRGBFloat);
    const bool rgba_float = guid_eq(guid, GUID_PKPixelFormat128bppRGBAFloat) ||
        guid_eq(guid, GUID_PKPixelFormat128bppPRGBAFloat);
    const bool rgba_float_prem = guid_eq(guid, GUID_PKPixelFormat128bppPRGBAFloat);
    const bool fixed_has_a = guid_eq(guid, GUID_PKPixelFormat64bppRGBAFixedPoint);
    const bool rgba16_has_a = guid_eq(guid, GUID_PKPixelFormat64bppRGBA) ||
        guid_eq(guid, GUID_PKPixelFormat64bppPRGBA);
    const bool rgba16_prem = guid_eq(guid, GUID_PKPixelFormat64bppPRGBA);
    const bool rgba8_has_a = guid_eq(guid, GUID_PKPixelFormat32bppRGBA) ||
        guid_eq(guid, GUID_PKPixelFormat32bppPRGBA);

    PKPixelInfo pi{};
    pi.pGUIDPixFmt = &guid;
    if (PixelFormatLookup(&pi, LOOKUP_FORWARD) != WMP_errSuccess || pi.cbitUnit == 0 ||
        (pi.cbitUnit % 8) != 0) {
        ALOGE("bad cbitUnit");
        release_all();
        return false;
    }
    const U32 bytes_per_pixel = pi.cbitUnit / 8u;
    const size_t stride = static_cast<size_t>(w) * static_cast<size_t>(bytes_per_pixel);
    const size_t buf_size = stride * static_cast<size_t>(h);
    std::vector<uint8_t> raw(buf_size);

    PKRect rect{};
    rect.X = 0;
    rect.Y = 0;
    rect.Width = static_cast<I32>(w);
    rect.Height = static_cast<I32>(h);
    err = decoder->Copy(decoder, &rect, raw.data(), static_cast<U32>(stride));
    if (Failed(err)) {
        ALOGE("Copy failed: %d", err);
        release_all();
        return false;
    }

    out_rgba.resize(static_cast<size_t>(w) * h * 4);
    const size_t src_bpp = bytes_per_pixel;
    const size_t npx = static_cast<size_t>(w) * h;

    auto store_lin = [&](size_t i, float r, float g, float b, float a) {
        out_rgba[i * 4 + 0] = store_working<Sample>(r);
        out_rgba[i * 4 + 1] = store_working<Sample>(g);
        out_rgba[i * 4 + 2] = store_working<Sample>(b);
        out_rgba[i * 4 + 3] = store_working<Sample>(a);
    };
    auto store_enc_rgb = [&](size_t i, float er, float eg, float eb, float ea) {
        if (expand_as_encoded) {
            // Keep encoded; skcms applies ICC TRC + matrix → linear BT.2020.
            store_lin(i, er, eg, eb, ea);
        } else {
            out_rgba[i * 4 + 0] = store_working<Sample>(srgb_eotf(er));
            out_rgba[i * 4 + 1] = store_working<Sample>(srgb_eotf(eg));
            out_rgba[i * 4 + 2] = store_working<Sample>(srgb_eotf(eb));
            out_rgba[i * 4 + 3] = store_working<Sample>(ea);
        }
    };

    if (rgba_half || rgb_half) {
        for (unsigned y = 0; y < h; y++) {
            const uint8_t* row = raw.data() + static_cast<size_t>(y) * stride;
            for (unsigned x = 0; x < w; x++) {
                const uint16_t* p =
                    reinterpret_cast<const uint16_t*>(row + static_cast<size_t>(x) * src_bpp);
                const size_t i = static_cast<size_t>(y) * w + x;
                out_rgba[i * 4 + 0] = store_working<Sample>(half_to_float(p[0]));
                out_rgba[i * 4 + 1] = store_working<Sample>(half_to_float(p[1]));
                out_rgba[i * 4 + 2] = store_working<Sample>(half_to_float(p[2]));
                out_rgba[i * 4 + 3] = store_working<Sample>(rgba_half ? half_to_float(p[3]) : 1.0f);
            }
        }
        ok = true;
    } else if (rgba_float || rgb_float) {
        const bool has_a = rgba_float;
        for (unsigned y = 0; y < h; y++) {
            const uint8_t* row = raw.data() + static_cast<size_t>(y) * stride;
            for (unsigned x = 0; x < w; x++) {
                const float* p =
                    reinterpret_cast<const float*>(row + static_cast<size_t>(x) * src_bpp);
                const size_t i = static_cast<size_t>(y) * w + x;
                float r = p[0], g = p[1], b = p[2];
                float a = has_a ? p[3] : 1.0f;
                if (rgba_float_prem) {
                    if (a > 1e-6f) {
                        r /= a;
                        g /= a;
                        b /= a;
                    } else {
                        r = g = b = 0.f;
                    }
                }
                store_lin(i, r, g, b, a);
            }
        }
        ok = true;
    } else if (pix == JxrPix::Rgb101010) {
        // jxrlib RGB101010_RGB48: R = (v>>20)&0x3FF, G = (v>>10)&0x3FF, B = v&0x3FF.
        // Unsigned integer → sRGB-encoded by Microsoft default (not linear scRGB).
        for (size_t i = 0; i < npx; i++) {
            uint32_t p;
            memcpy(&p, raw.data() + i * 4, 4);
            const float r = static_cast<float>((p >> 20) & 0x3ff) / 1023.0f;
            const float g = static_cast<float>((p >> 10) & 0x3ff) / 1023.0f;
            const float b = static_cast<float>((p >> 0) & 0x3ff) / 1023.0f;
            store_enc_rgb(i, r, g, b, 1.0f);
        }
        ok = true;
    } else if (pix == JxrPix::Fixed16) {
        // Linear scRGB 13.3 fixed-point (HD Photo HDR).
        for (unsigned y = 0; y < h; y++) {
            const uint8_t* row = raw.data() + static_cast<size_t>(y) * stride;
            for (unsigned x = 0; x < w; x++) {
                const int16_t* p =
                    reinterpret_cast<const int16_t*>(row + static_cast<size_t>(x) * src_bpp);
                const size_t i = static_cast<size_t>(y) * w + x;
                store_lin(i, fixed13_3_to_float(p[0]), fixed13_3_to_float(p[1]),
                          fixed13_3_to_float(p[2]),
                          fixed_has_a ? fixed13_3_to_float(p[3]) : 1.0f);
            }
        }
        ok = true;
    } else if (pix == JxrPix::Bgr8) {
        for (unsigned y = 0; y < h; y++) {
            const uint8_t* row = raw.data() + static_cast<size_t>(y) * stride;
            for (unsigned x = 0; x < w; x++) {
                const uint8_t* p = row + static_cast<size_t>(x) * src_bpp;
                const size_t i = static_cast<size_t>(y) * w + x;
                store_enc_rgb(i, p[2] / 255.f, p[1] / 255.f, p[0] / 255.f, 1.0f);
            }
        }
        ok = true;
    } else if (pix == JxrPix::Bgra8 || pix == JxrPix::Pbgra8) {
        const bool prem = (pix == JxrPix::Pbgra8);
        for (unsigned y = 0; y < h; y++) {
            const uint8_t* row = raw.data() + static_cast<size_t>(y) * stride;
            for (unsigned x = 0; x < w; x++) {
                const uint8_t* p = row + static_cast<size_t>(x) * src_bpp;
                const size_t i = static_cast<size_t>(y) * w + x;
                float a = p[3] / 255.f;
                float b = p[0] / 255.f, g = p[1] / 255.f, r = p[2] / 255.f;
                if (prem && a > 1e-6f) {
                    r /= a;
                    g /= a;
                    b /= a;
                } else if (prem && a <= 1e-6f) {
                    r = g = b = 0.f;
                }
                store_enc_rgb(i, r, g, b, a);
            }
        }
        ok = true;
    } else if (pix == JxrPix::Rgb8) {
        for (unsigned y = 0; y < h; y++) {
            const uint8_t* row = raw.data() + static_cast<size_t>(y) * stride;
            for (unsigned x = 0; x < w; x++) {
                const uint8_t* p = row + static_cast<size_t>(x) * src_bpp;
                const size_t i = static_cast<size_t>(y) * w + x;
                store_enc_rgb(i, p[0] / 255.f, p[1] / 255.f, p[2] / 255.f, 1.0f);
            }
        }
        ok = true;
    } else if (pix == JxrPix::Rgba8 || pix == JxrPix::Prgba8) {
        const bool prem = (pix == JxrPix::Prgba8);
        for (unsigned y = 0; y < h; y++) {
            const uint8_t* row = raw.data() + static_cast<size_t>(y) * stride;
            for (unsigned x = 0; x < w; x++) {
                const uint8_t* p = row + static_cast<size_t>(x) * src_bpp;
                const size_t i = static_cast<size_t>(y) * w + x;
                float r = p[0] / 255.f, g = p[1] / 255.f, b = p[2] / 255.f;
                float a = rgba8_has_a || prem ? p[3] / 255.f : 1.0f;
                if (prem && a > 1e-6f) {
                    r /= a;
                    g /= a;
                    b /= a;
                } else if (prem && a <= 1e-6f) {
                    r = g = b = 0.f;
                }
                store_enc_rgb(i, r, g, b, a);
            }
        }
        ok = true;
    } else if (pix == JxrPix::Rgb16) {
        for (unsigned y = 0; y < h; y++) {
            const uint8_t* row = raw.data() + static_cast<size_t>(y) * stride;
            for (unsigned x = 0; x < w; x++) {
                const uint16_t* p =
                    reinterpret_cast<const uint16_t*>(row + static_cast<size_t>(x) * src_bpp);
                const size_t i = static_cast<size_t>(y) * w + x;
                store_enc_rgb(i, p[0] / 65535.f, p[1] / 65535.f, p[2] / 65535.f, 1.0f);
            }
        }
        ok = true;
    } else if (pix == JxrPix::Rgba16) {
        for (unsigned y = 0; y < h; y++) {
            const uint8_t* row = raw.data() + static_cast<size_t>(y) * stride;
            for (unsigned x = 0; x < w; x++) {
                const uint16_t* p =
                    reinterpret_cast<const uint16_t*>(row + static_cast<size_t>(x) * src_bpp);
                const size_t i = static_cast<size_t>(y) * w + x;
                float r = p[0] / 65535.f, g = p[1] / 65535.f, b = p[2] / 65535.f;
                float a = rgba16_has_a ? p[3] / 65535.f : 1.0f;
                if (rgba16_prem && a > 1e-6f) {
                    r /= a;
                    g /= a;
                    b /= a;
                } else if (rgba16_prem && a <= 1e-6f) {
                    r = g = b = 0.f;
                }
                store_enc_rgb(i, r, g, b, a);
            }
        }
        ok = true;
    } else if (pix == JxrPix::Gray8) {
        for (unsigned y = 0; y < h; y++) {
            const uint8_t* row = raw.data() + static_cast<size_t>(y) * stride;
            for (unsigned x = 0; x < w; x++) {
                const float yv = row[static_cast<size_t>(x) * src_bpp] / 255.f;
                store_enc_rgb(static_cast<size_t>(y) * w + x, yv, yv, yv, 1.0f);
            }
        }
        ok = true;
    } else if (pix == JxrPix::Gray16) {
        for (unsigned y = 0; y < h; y++) {
            const uint8_t* row = raw.data() + static_cast<size_t>(y) * stride;
            for (unsigned x = 0; x < w; x++) {
                const uint16_t* p =
                    reinterpret_cast<const uint16_t*>(row + static_cast<size_t>(x) * src_bpp);
                const float yv = p[0] / 65535.f;
                store_enc_rgb(static_cast<size_t>(y) * w + x, yv, yv, yv, 1.0f);
            }
        }
        ok = true;
    } else {
        ALOGE("Unhandled JXR expand path");
    }

    release_all();
    if (!ok) return false;

    // Honor embedded ICC after expand (color context > pixel-format inference).
    if (have_valid_icc) {
        // Samples are still encoded in the ICC; apply full TRC + matrix.
        if (!apply_jxr_color_context(out_rgba, npx, color_ctx.data(), color_ctx.size())) {
            ALOGE("JXR ICC transform failed");
            return false;
        }
        if (out_cg) *out_cg = UHDR_CG_BT_2100;
    }
    // JPEG/UHDR have no alpha plane — flatten only when the caller will encode
    // to those formats. Direct display keeps straight alpha for transparency.
    if (composite_alpha_on_black) {
        composite_straight_alpha_on_black(out_rgba, npx);
    }
    if (out_untagged_linear) {
        // Half, float, and 13.3 fixed are linear scRGB when no color context won.
        // Integer formats are sRGB-encoded. An ICC transform already produced linear
        // BT.2020, so those frames must not be fitted again.
        const bool linear_storage = rgba_half || rgb_half || rgba_float || rgb_float ||
            pix == JxrPix::Fixed16;
        *out_untagged_linear = linear_storage && !have_valid_icc;
    }
    return true;
}

/**
 * Untagged linear scRGB uses 1.0 = 80 nits. Our shared linear working space uses 1.0 = 203 nits,
 * so leaving the codes unchanged lifts midtones against a reference monitor.
 * Scale RGB only. Alpha stays. Integer sRGB and ICC frames must not come here.
 */
template<class Sample>
void rebase_untagged_scrgb(std::vector<Sample>& rgba, unsigned w, unsigned h, bool to_bt2020 = false) {
    constexpr float kScRgbWhiteNits = 80.f;
    const float scale = kScRgbWhiteNits / kSdrWhiteNits;
    const size_t npx = static_cast<size_t>(w) * h;
    if (w == 0 || h == 0 || rgba.size() < npx * 4) return;
    hdr_parallel_pixels(npx, to_bt2020 ? hdr_pixel_jobs(npx) : 1, [&](size_t begin, size_t end, unsigned) {
        for (size_t i = begin; i < end; i++) {
            float r = read_working(rgba[i * 4 + 0]);
            float g = read_working(rgba[i * 4 + 1]);
            float b = read_working(rgba[i * 4 + 2]);
            if (to_bt2020) {
                // Linear BT.709/scRGB D65 -> BT.2020 D65. Keep signed channels
                // through the matrix: negative scRGB values can represent WCG.
                // Fold the white-reference scale into this pass; alpha is unchanged.
                if (!std::isfinite(r)) r = 0.f;
                if (!std::isfinite(g)) g = 0.f;
                if (!std::isfinite(b)) b = 0.f;
                linear_bt709_to_bt2020(r, g, b);
            }
            rgba[i * 4 + 0] = store_working<Sample>(r * scale);
            rgba[i * 4 + 1] = store_working<Sample>(g * scale);
            rgba[i * 4 + 2] = store_working<Sample>(b * scale);
        }
    });
    ALOGI("JXR scRGB rebase 80/203 %ux%u bt2020=%d", w, h, to_bt2020);
}

// Path helper: load whole file into memory then decode (no jxrlib file I/O).
// Path convert is always UHDR/JPEG → composite alpha.
bool decode_jxr_to_rgba_f16(const char* path, std::vector<uint16_t>& out_rgba, unsigned& w,
                            unsigned& h, uhdr_color_gamut_t* out_cg) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        ALOGE("open JXR failed: %s", path);
        return false;
    }
    const auto sz = in.tellg();
    if (sz <= 0) return false;
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (!in.read(reinterpret_cast<char*>(buf.data()), sz)) {
        ALOGE("read JXR failed: %s", path);
        return false;
    }
    bool untagged_linear = false;
    if (!decode_jxr_from_memory(buf.data(), buf.size(), out_rgba, w, h,
                                /*composite_alpha_on_black=*/true, &untagged_linear, out_cg)) return false;
    if (untagged_linear) rebase_untagged_scrgb(out_rgba, w, h);
    return true;
}

}  // namespace

static bool write_file(const char* path, const void* data, size_t size) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

namespace {

struct JpegErr {
    jpeg_error_mgr pub;
    jmp_buf jump;
};

// Owned by the caller, including across libjpeg's setjmp error path.
struct JpegMemory {
    unsigned char* data = nullptr;
    unsigned long size = 0;
    ~JpegMemory() { std::free(data); }
};

void write_export_exif(j_compress_ptr encoder, const StillEncodeQuality& quality) {
    if (!quality.exif_size) return;
    std::vector<uint8_t> payload{'E', 'x', 'i', 'f', 0, 0};
    payload.insert(payload.end(), quality.exif, quality.exif + quality.exif_size);
    jpeg_write_marker(encoder, JPEG_APP0 + 1, payload.data(), static_cast<unsigned>(payload.size()));
}

int encode_srgb_jpeg_u8(unsigned w, unsigned h, const uint8_t* rgba, const char* out_path,
                        uhdr_color_gamut_t cg, StillEncodeQuality quality = 1, JpegMemory* memory = nullptr);


void jpeg_err_exit(j_common_ptr cinfo) {
    auto* e = reinterpret_cast<JpegErr*>(cinfo->err);
    char buf[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, buf);
    ALOGE("libjpeg: %s", buf);
    longjmp(e->jump, 1);
}

/**
 * Pure SDR → baseline JPEG (no gain map). Avoids libultrahdr epsilon-bump of
 * max_content_boost when min==max (≈1.07), which made tools show ratio ≠ 1.0.
 *
 * Color policy:
 * - Keep the pixel primaries (BT.709, Display P3, or BT.2020).
 * - Encode with the sRGB curve and embed the matching ICC (APP2).
 * - If that ICC cannot be built, rematrix to BT.709 and embed an sRGB profile.
 *
 * Quality from the selected preset, 4:4:4 chroma (no 4:2:0 color blur on 4K stills).
 */
int encode_baseline_sdr_jpeg(unsigned w, unsigned h, const uint16_t* rgba, const char* out_path,
                             uhdr_color_gamut_t cg, StillEncodeQuality quality = 1) {
    if (!rgba || !out_path || w == 0 || h == 0) return -30;
    const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<uint8_t> rgb(pixels * 3);
    if (cg != UHDR_CG_BT_709 && cg != UHDR_CG_DISPLAY_P3 && cg != UHDR_CG_BT_2100) {
        cg = UHDR_CG_BT_709;
    }
    const auto& srgb_codes = half_srgb_u8_table();
    hdr_parallel_pixels(pixels, hdr_pixel_jobs(pixels), [&](size_t begin, size_t end, unsigned) {
        for (size_t i = begin; i < end; ++i) {
            float alpha=half_to_float(rgba[i*4+3]);if(!std::isfinite(alpha))alpha=0.f;
            alpha=std::clamp(alpha,0.f,1.f);
            for(unsigned c=0;c<3;++c)rgb[i*3+c]=alpha==1.f?srgb_codes[rgba[i*4+c]]:
                linear_to_srgb_u8(half_to_float(rgba[i*4+c])*alpha);
        }
    });

    FILE* fp = fopen(out_path, "wb");
    if (!fp) {
        ALOGE("fopen baseline JPEG failed: %s", out_path);
        return -31;
    }

    // APP2 payload includes the ICC_PROFILE identifier. The curve is sRGB;
    // the profile primaries match the samples.
    std::shared_ptr<ultrahdr::DataStruct> icc =
        ultrahdr::IccHelper::writeIccProfile(UHDR_CT_SRGB, cg);
    if ((!icc || !icc->getData() || icc->getLength() == 0) && cg != UHDR_CG_BT_709) {
        ALOGE("ICC build failed for cg=%d — rematrix to BT.709", (int)cg);
        icc.reset();
        for (size_t i = 0; i < pixels; i++) {
            float r = half_to_float(rgba[i * 4 + 0]);
            float g = half_to_float(rgba[i * 4 + 1]);
            float b = half_to_float(rgba[i * 4 + 2]);
            float alpha=half_to_float(rgba[i*4+3]);if(!std::isfinite(alpha))alpha=0.f;
            alpha=std::clamp(alpha,0.f,1.f);r*=alpha;g*=alpha;b*=alpha;
            if (cg == UHDR_CG_DISPLAY_P3) {
                linear_p3_to_bt709(r, g, b);
            } else {
                linear_bt2020_to_bt709(r, g, b);
            }
            if (!std::isfinite(r) || r < 0.f) r = 0.f;
            if (!std::isfinite(g) || g < 0.f) g = 0.f;
            if (!std::isfinite(b) || b < 0.f) b = 0.f;
            rgb[i * 3 + 0] = linear_to_srgb_u8(r);
            rgb[i * 3 + 1] = linear_to_srgb_u8(g);
            rgb[i * 3 + 2] = linear_to_srgb_u8(b);
        }
        icc = ultrahdr::IccHelper::writeIccProfile(UHDR_CT_SRGB, UHDR_CG_BT_709);
    }

    jpeg_compress_struct cinfo{};
    JpegErr jerr{};
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_err_exit;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_compress(&cinfo);
        fclose(fp);
        return -32;
    }
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);
    cinfo.image_width = w;
    cinfo.image_height = h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, export_jpeg_quality(quality), TRUE);
    cinfo.optimize_coding = TRUE;
    // All export quality presets preserve 4:4:4 color edges.
    cinfo.comp_info[0].h_samp_factor = 1;
    cinfo.comp_info[0].v_samp_factor = 1;
    cinfo.comp_info[1].h_samp_factor = 1;
    cinfo.comp_info[1].v_samp_factor = 1;
    cinfo.comp_info[2].h_samp_factor = 1;
    cinfo.comp_info[2].v_samp_factor = 1;
    jpeg_start_compress(&cinfo, TRUE);
    write_export_exif(&cinfo, quality);
    if (icc && icc->getData() && icc->getLength() > 0) {
        // Same as libultrahdr JpegEncoderHelper: APP2 + ICC_PROFILE payload.
        const auto len = static_cast<unsigned int>(icc->getLength());
        if (len <= 65533u) {
            jpeg_write_marker(&cinfo, JPEG_APP0 + 2, static_cast<const JOCTET*>(icc->getData()),
                              len);
        } else {
            ALOGE("P3 ICC too large for single APP2 (%u) — writing without ICC", len);
        }
    }
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = rgb.data() + static_cast<size_t>(cinfo.next_scanline) * w * 3;
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(fp);
    ALOGI("Wrote baseline SDR JPEG %ux%u cg=%d p3_icc=%d → %s", w, h, (int)cg,
          (icc && icc->getData() && icc->getLength() > 0) ? 1 : 0, out_path);
    return 0;
}

}  // namespace

/**
 * Encode LINEAR half-float RGB → Ultra HDR JPEG (HDR) or baseline JPEG (pure SDR).
 *
 * Capacity: uhdr_enc_set_target_display_peak_brightness sets displayRatioForFullHdr
 * (hdr_capacity_max) ≈ peak_nits/203. Prefer that field at display time — not ratioMax.
 *
 * Note (libultrahdr API-0, HDR raw only): set_min_max_content_boost may not fully
 * shrink gain-map ratioMax to content_peak; one-pass path can leave ratioMax near
 * 10000/203 ≈ 49. Keep target_display_peak_brightness for correct capacity.
 *
 * Pure SDR ([force_hdr] false and peak ≤ 1.0): baseline JPEG only — no gain map.
 *
 * HDR base defaults to that clamp-at-1.0 image, passed as [UHDR_SDR_IMG].
 * RAW can supply its separate camera-look SDR render in the same primaries.
 * API-0 would Reinhard-tone-map LINEAR input against a 10000-nit headroom and
 * darken the base (linear 1.0 lands near 0.5). The gain map then only restores
 * samples above 1. HDR mode stays the original linear picture.
 *
 * [fixed_peak_nits] > 0: skip full-frame peak scan (thumbs: 203 SDR / 1000 HDR).
 * [fixed_peak_nits] ≤ 0: p99.99 scan of max(R,G,B) (full pages).
 *
 * [cg] must match [rgba] primaries for Ultra HDR. Baseline SDR keeps those
 * primaries and embeds a matching sRGB-curve ICC (see encode_baseline_sdr_jpeg).
 */
int encode_linear_rgba_f16_to_uhdr(unsigned w, unsigned h, const uint16_t* rgba,
                                   const char* out_path, uhdr_color_gamut_t cg,
                                   float fixed_peak_nits, bool force_hdr, const uint8_t* sdr_base, StillEncodeQuality quality) {
    if (!rgba || !out_path || w == 0 || h == 0) return -1;

    // Only the three gamuts libultrahdr can tag/ICC-embed.
    if (cg != UHDR_CG_BT_709 && cg != UHDR_CG_DISPLAY_P3 && cg != UHDR_CG_BT_2100) {
        ALOGI("Unknown gamut %d → BT.709 tag (caller must have converted RGB if needed)", (int)cg);
        cg = UHDR_CG_BT_709;
    }

    float content_peak;
    float peak_nits;
    if (fixed_peak_nits > 0.f) {
        // Thumbs / known path: fixed MaxCLL, no pixel histogram.
        peak_nits = fixed_peak_nits;
        if (peak_nits < kSdrWhiteNits) peak_nits = kSdrWhiteNits;
        if (peak_nits > kMaxNits) peak_nits = kMaxNits;
        content_peak = peak_nits / kSdrWhiteNits;
        if (content_peak < 1.0f) content_peak = 1.0f;
    } else {
        const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
        content_peak = scan_scrgb_peak(rgba, pixels);
        peak_nits = kSdrWhiteNits * content_peak;
        if (peak_nits < kSdrWhiteNits) peak_nits = kSdrWhiteNits;
        if (peak_nits > kMaxNits) peak_nits = kMaxNits;
    }

    // Pure SDR → baseline JPEG (no Ultra HDR gain map / no fake ratio > 1).
    if (!force_hdr && content_peak <= 1.0f) {
        return encode_baseline_sdr_jpeg(w, h, rgba, out_path, cg, quality);
    }

    uhdr_codec_private_t* enc = uhdr_create_encoder();
    if (!enc) {
        ALOGE("uhdr_create_encoder failed");
        return -1;
    }

    uhdr_raw_image_t img{};
    img.fmt = UHDR_IMG_FMT_64bppRGBAHalfFloat;
    img.cg = cg;
    img.ct = UHDR_CT_LINEAR;
    img.range = UHDR_CR_FULL_RANGE;
    img.w = w;
    img.h = h;
    img.planes[UHDR_PLANE_PACKED] = const_cast<uint16_t*>(rgba);
    img.stride[UHDR_PLANE_PACKED] = w;

    uhdr_error_info_t err = uhdr_enc_set_raw_image(enc, &img, UHDR_HDR_IMG);
    if (err.error_code != UHDR_CODEC_OK) {
        ALOGE("uhdr_enc_set_raw_image: %s", err.has_detail ? err.detail : "error");
        uhdr_release_encoder(enc);
        return -2;
    }

    if(quality.exif_size) {
        std::vector<uint8_t> payload={'E','x','i','f',0,0};
        payload.insert(payload.end(),quality.exif,quality.exif+quality.exif_size);
        uhdr_mem_block_t metadata{};metadata.data=payload.data();metadata.data_sz=metadata.capacity=payload.size();
        err=uhdr_enc_set_exif_data(enc,&metadata);
        if(err.error_code!=UHDR_CODEC_OK){uhdr_release_encoder(enc);return -6;}
    }
    // Quality: base + multi-channel gain map (libultrahdr default is 95/95).
    err = uhdr_enc_set_quality(enc, export_jpeg_quality(quality), UHDR_BASE_IMG);
    if (err.error_code != UHDR_CODEC_OK) {
        ALOGE("uhdr_enc_set_quality base: %s", err.has_detail ? err.detail : "error");
        uhdr_release_encoder(enc);
        return -6;
    }
    err = uhdr_enc_set_quality(enc, export_jpeg_quality(quality), UHDR_GAIN_MAP_IMG);
    if (err.error_code != UHDR_CODEC_OK) {
        ALOGE("uhdr_enc_set_quality gainmap: %s", err.has_detail ? err.detail : "error");
        uhdr_release_encoder(enc);
        return -6;
    }
    err = uhdr_enc_set_using_multi_channel_gainmap(enc, 1);
    if (err.error_code != UHDR_CODEC_OK) {
        ALOGE("uhdr_enc_set_using_multi_channel_gainmap: %s",
              err.has_detail ? err.detail : "error");
        uhdr_release_encoder(enc);
        return -6;
    }
    err = uhdr_enc_set_gainmap_gamma(enc, 1.0f);
    if (err.error_code != UHDR_CODEC_OK) {
        ALOGE("uhdr_enc_set_gainmap_gamma: %s", err.has_detail ? err.detail : "error");
        uhdr_release_encoder(enc);
        return -6;
    }
    // Keep the primary rendition at full size. Mid/Low and the custom downsize
    // toggle halve gain-map dimensions (one quarter of the pixels).
    err = uhdr_enc_set_gainmap_scale_factor(enc, quality.jpeg_gain_map_scale());
    if (err.error_code != UHDR_CODEC_OK) {
        ALOGE("uhdr_enc_set_gainmap_scale_factor: %s", err.has_detail ? err.detail : "error");
        uhdr_release_encoder(enc);
        return -6;
    }
    err = uhdr_enc_set_preset(enc, UHDR_USAGE_BEST_QUALITY);
    if (err.error_code != UHDR_CODEC_OK) {
        ALOGE("uhdr_enc_set_preset: %s", err.has_detail ? err.detail : "error");
        uhdr_release_encoder(enc);
        return -6;
    }
    err = uhdr_enc_set_output_format(enc, UHDR_CODEC_JPG);
    if (err.error_code != UHDR_CODEC_OK) {
        ALOGE("uhdr_enc_set_output_format: %s", err.has_detail ? err.detail : "error");
        uhdr_release_encoder(enc);
        return -6;
    }

    // Hint only — API-0 one-pass may still emit ratioMax ≈ 49; capacity is set below.
    err = uhdr_enc_set_min_max_content_boost(enc, 1.0f, content_peak);
    if (err.error_code != UHDR_CODEC_OK) {
        ALOGE("uhdr_enc_set_min_max_content_boost: %s", err.has_detail ? err.detail : "error");
        uhdr_release_encoder(enc);
        return -7;
    }
    // Sets displayRatioForFullHdr / hdr_capacity_max ≈ peak_nits / 203.
    err = uhdr_enc_set_target_display_peak_brightness(enc, peak_nits);
    if (err.error_code != UHDR_CODEC_OK) {
        ALOGE("uhdr_enc_set_target_display_peak_brightness: %s",
              err.has_detail ? err.detail : "error");
        uhdr_release_encoder(enc);
        return -7;
    }

    // RAW supplies its camera-look SDR plane before HDR expansion. Other codecs
    // keep the baseline-JPEG render, avoiding API-0's 10000-nit Reinhard map.
    const size_t sdr_pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<uint8_t> sdr_rgba;
    if (!sdr_base) {
        sdr_rgba.resize(sdr_pixels * 4);
        const auto& srgb_codes = half_srgb_u8_table();
        hdr_parallel_pixels(sdr_pixels, hdr_pixel_jobs(sdr_pixels), [&](size_t begin, size_t end, unsigned) {
            for (size_t i = begin; i < end; ++i) {
                for (unsigned c = 0; c < 3; ++c) sdr_rgba[i * 4 + c] = srgb_codes[rgba[i * 4 + c]];
                sdr_rgba[i * 4 + 3] = 255;
            }
        });
        sdr_base = sdr_rgba.data();
    }
    uhdr_raw_image_t sdr_img{};
    sdr_img.fmt = UHDR_IMG_FMT_32bppRGBA8888;
    sdr_img.cg = cg;
    sdr_img.ct = UHDR_CT_SRGB;
    sdr_img.range = UHDR_CR_FULL_RANGE;
    sdr_img.w = w;
    sdr_img.h = h;
    sdr_img.planes[UHDR_PLANE_PACKED] = const_cast<uint8_t*>(sdr_base);
    sdr_img.stride[UHDR_PLANE_PACKED] = w;
    err = uhdr_enc_set_raw_image(enc, &sdr_img, UHDR_SDR_IMG);
    if (err.error_code != UHDR_CODEC_OK) {
        ALOGE("uhdr_enc_set_raw_image sdr: %s", err.has_detail ? err.detail : "error");
        uhdr_release_encoder(enc);
        return -8;
    }

    // API-2: keep the raw SDR plane for gain-map computation and provide the
    // already encoded 4:4:4 rendition for assembly, avoiding internal 4:2:0 encode.
    JpegMemory base_jpeg;
    const int rc = encode_srgb_jpeg_u8(w, h, sdr_base, nullptr, cg, quality, &base_jpeg);
    if (rc != 0) {
        uhdr_release_encoder(enc);
        return rc;
    }
    uhdr_compressed_image_t base_img{};
    base_img.data = base_jpeg.data;
    base_img.data_sz = base_jpeg.size;
    base_img.capacity = base_jpeg.size;
    base_img.cg = cg;
    base_img.ct = UHDR_CT_SRGB;
    base_img.range = UHDR_CR_FULL_RANGE;
    err = uhdr_enc_set_compressed_image(enc, &base_img, UHDR_SDR_IMG);
    if (err.error_code != UHDR_CODEC_OK) {
        ALOGE("uhdr_enc_set_compressed_image: %s", err.has_detail ? err.detail : "error");
        uhdr_release_encoder(enc);
        return -9;
    }

    ALOGI("Ultra HDR encode content_peak=%.3f peak_nits=%.1f fixed=%d force_hdr=%d cg=%d %ux%u",
          content_peak, peak_nits, fixed_peak_nits > 0.f ? 1 : 0, force_hdr ? 1 : 0, (int)cg, w, h);

    err = uhdr_encode(enc);
    if (err.error_code != UHDR_CODEC_OK) {
        ALOGE("uhdr_encode: %s", err.has_detail ? err.detail : "error");
        uhdr_release_encoder(enc);
        return -3;
    }
    uhdr_compressed_image_t* stream = uhdr_get_encoded_stream(enc);
    if (!stream || !stream->data || stream->data_sz == 0) {
        ALOGE("uhdr_get_encoded_stream empty");
        uhdr_release_encoder(enc);
        return -4;
    }
    if (!write_file(out_path, stream->data, stream->data_sz)) {
        ALOGE("write Ultra HDR failed: %s", out_path);
        uhdr_release_encoder(enc);
        return -5;
    }
    ALOGI("Wrote Ultra HDR %ux%u peak=%.2f cg=%d → %s (%zu bytes)", w, h, content_peak, (int)cg,
          out_path, stream->data_sz);
    uhdr_release_encoder(enc);
    return 0;
}

namespace {

inline float clamp_nonneg(float v) {
    if (!std::isfinite(v) || v < 0.f) return 0.f;
    if (v > kMaxLinear) return kMaxLinear;
    return v;
}

}  // namespace

// Platform decoders produce extended-linear sRGB F16. Convert the captured
// buffer directly instead of allocating a second Bitmap and drawing with Canvas.
namespace {

constexpr int kExportJpeg = 0;
constexpr int kExportJpegHdr = 1;
constexpr int kExportJxl = 2;
constexpr int kExportJxlHdr = 3;
constexpr int kExportAvif = 4;
constexpr int kExportAvifPq = 5;
constexpr int kExportAvifGainMap = 6;
constexpr int kExportJxlPq = 7;

uhdr_color_gamut_t export_gamut(int gamut) {
    if (gamut == 1) return UHDR_CG_DISPLAY_P3;
    if (gamut == 2) return UHDR_CG_BT_2100;
    return UHDR_CG_BT_709;
}

void unpremultiply_f16(uint16_t* rgba, size_t pixels) {
    hdr_parallel_pixels(pixels, hdr_pixel_jobs(pixels), [&](size_t begin, size_t end, unsigned) {
        for (size_t i = begin; i < end; i++) {
            if (rgba[i * 4 + 3] == 0x3c00 && rgba[i * 4] < 0x7c00 &&
                rgba[i * 4 + 1] < 0x7c00 && rgba[i * 4 + 2] < 0x7c00) continue;
            float a = half_to_float(rgba[i * 4 + 3]);
            if (!std::isfinite(a) || a <= 0.f) {
                rgba[i * 4 + 0] = float_to_half(0.f);
                rgba[i * 4 + 1] = float_to_half(0.f);
                rgba[i * 4 + 2] = float_to_half(0.f);
                rgba[i * 4 + 3] = float_to_half(0.f);
                continue;
            }
            if (a > 1.f) a = 1.f;
            float r = half_to_float(rgba[i * 4 + 0]) / a;
            float g = half_to_float(rgba[i * 4 + 1]) / a;
            float b = half_to_float(rgba[i * 4 + 2]) / a;
            rgba[i * 4 + 0] = float_to_half(std::isfinite(r) && r > 0.f ? r : 0.f);
            rgba[i * 4 + 1] = float_to_half(std::isfinite(g) && g > 0.f ? g : 0.f);
            rgba[i * 4 + 2] = float_to_half(std::isfinite(b) && b > 0.f ? b : 0.f);
            rgba[i * 4 + 3] = float_to_half(a);
        }
    });
}

void unpremultiply_u8(uint8_t* rgba, size_t pixels) {
    hdr_parallel_pixels(pixels, hdr_pixel_jobs(pixels), [&](size_t begin, size_t end, unsigned) {
        for (size_t i = begin; i < end; i++) {
            const unsigned a = rgba[i * 4 + 3];
            if (a == 0) {
                rgba[i * 4 + 0] = 0;
                rgba[i * 4 + 1] = 0;
                rgba[i * 4 + 2] = 0;
                continue;
            }
            if (a == 255) continue;
            for (int c = 0; c < 3; c++) {
                const unsigned v = (static_cast<unsigned>(rgba[i * 4 + c]) * 255u + a / 2u) / a;
                rgba[i * 4 + c] = static_cast<uint8_t>(v > 255u ? 255u : v);
            }
        }
    });
}

int encode_srgb_jpeg_u8(unsigned w, unsigned h, const uint8_t* rgba, const char* out_path,
                        uhdr_color_gamut_t cg, StillEncodeQuality quality, JpegMemory* memory) {
    if (!rgba || (!out_path && !memory) || w == 0 || h == 0) return -40;
    FILE* fp = memory ? nullptr : fopen(out_path, "wb");
    if (!memory && !fp) return -41;
    jpeg_compress_struct cinfo{};
    JpegErr jerr{};
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_err_exit;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_compress(&cinfo);
        if (fp) fclose(fp);
        return -42;
    }
    jpeg_create_compress(&cinfo);
    if (memory) jpeg_mem_dest(&cinfo, &memory->data, &memory->size);
    else jpeg_stdio_dest(&cinfo, fp);
    cinfo.image_width = w;
    cinfo.image_height = h;
    cinfo.input_components = 4;
    cinfo.in_color_space = JCS_EXT_RGBA;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, export_jpeg_quality(quality), TRUE);
    cinfo.optimize_coding = TRUE;
    cinfo.comp_info[0].h_samp_factor = 1;
    cinfo.comp_info[0].v_samp_factor = 1;
    cinfo.comp_info[1].h_samp_factor = 1;
    cinfo.comp_info[1].v_samp_factor = 1;
    cinfo.comp_info[2].h_samp_factor = 1;
    cinfo.comp_info[2].v_samp_factor = 1;
    jpeg_start_compress(&cinfo, TRUE);
    write_export_exif(&cinfo, quality);
    auto icc = ultrahdr::IccHelper::writeIccProfile(UHDR_CT_SRGB, cg);
    if (!icc || !icc->getData() || icc->getLength() == 0) {
        jpeg_destroy_compress(&cinfo);
        if (fp) fclose(fp);
        return -43;
    }
    const auto len = static_cast<unsigned int>(icc->getLength());
    if (len > 65533u) {
        jpeg_destroy_compress(&cinfo);
        if (fp) fclose(fp);
        return -44;
    }
    jpeg_write_marker(&cinfo, JPEG_APP0 + 2, static_cast<const JOCTET*>(icc->getData()), len);
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = const_cast<uint8_t*>(rgba) + static_cast<size_t>(cinfo.next_scanline) * w * 4;
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    if (fp) fclose(fp);
    return 0;
}

}  // namespace

/**
 * Encode a reader bitmap or a full-res direct pack.
 * mode: 0 JPEG, 1 Ultra HDR JPEG, 2 SDR JXL, 3 HDR JXL, 4 SDR AVIF, 5 PQ AVIF, 6 ISO AVIF, 7 PQ JXL.
 * 8-bit samples are already encoded. F16 samples are linear. SDR JPEG
 * apply the sRGB curve and embed an ICC profile for the pixel primaries
 * (BT.709, Display P3, or BT.2020). JXL keeps linear half precision and those primaries.
 * 8-bit JPEG (HDR) stays a baseline JPEG. There is no headroom to put in a gain map.
 */
int encode_still_export(const uint8_t* src, size_t src_len, unsigned w, unsigned h, bool f16,
                        int gamut, int mode, bool premultiplied, const char* out_path, size_t sdr_offset = 0,
                        StillEncodeQuality quality = 1, bool mutable_input = false) {
    if (!src || !out_path || w == 0 || h == 0 || mode < 0 || mode > kExportJxlPq) return -50;
    const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
    const size_t bpp = f16 ? 8u : 4u;
    if (pixels == 0 || pixels > 80000000u || src_len < pixels * bpp) return -51;
    if (sdr_offset && (!f16 || sdr_offset != pixels * 8u || sdr_offset > src_len || pixels * 4u > src_len - sdr_offset)) return -51;
    const uhdr_color_gamut_t cg = export_gamut(gamut);
    if (f16) {
        std::vector<uint16_t> rgba_storage;
        auto* rgba = reinterpret_cast<uint16_t*>(const_cast<uint8_t*>(src));
        if (!mutable_input) {
            rgba_storage.resize(pixels * 4);
            memcpy(rgba_storage.data(), src, pixels * 8);
            rgba = rgba_storage.data();
        }
        if (premultiplied) unpremultiply_f16(rgba, pixels);
        if (mode >= kExportAvif && mode <= kExportAvifGainMap) {
            return encode_avif_f16(w, h, rgba, cg, mode - kExportAvif, out_path, quality,
                                   sdr_offset ? src + sdr_offset : nullptr);
        }
        if (mode == kExportJpegHdr) {
            return encode_linear_rgba_f16_to_uhdr(w, h, rgba, out_path, cg, 0.f, true,
                                                  sdr_offset ? src + sdr_offset : nullptr, quality);
        }
        if (mode == kExportJxl || mode == kExportJxlHdr || mode == kExportJxlPq) {
            return encode_jxl_f16(w, h, rgba, cg, mode != kExportJxl, out_path, quality, mode == kExportJxlPq);
        }
        return encode_baseline_sdr_jpeg(w, h, rgba, out_path, cg, quality);
    }
    std::vector<uint8_t> rgba_storage;
    auto* rgba = const_cast<uint8_t*>(src);
    if (!mutable_input) {
        rgba_storage.assign(src, src + pixels * 4);
        rgba = rgba_storage.data();
    }
    if (premultiplied) unpremultiply_u8(rgba, pixels);
    if (mode == kExportAvif) return encode_avif_u8(w, h, rgba, cg, out_path, quality);
    if (mode == kExportJxl) return encode_jxl_u8(w, h, rgba, cg, out_path, quality);
    if (mode == kExportJxlHdr || mode >= kExportAvifPq) {
        std::vector<uint16_t> linear(pixels * 4);
        std::array<uint16_t, 256> table{};
        for (size_t c = 0; c < table.size(); ++c) table[c] = float_to_half(srgb_eotf(c / 255.f));
        hdr_parallel_pixels(pixels, hdr_pixel_jobs(pixels), [&](size_t begin, size_t end, unsigned) {
            for (size_t i = begin; i < end; ++i) {
                for (unsigned c = 0; c < 3; ++c) linear[i * 4 + c] = table[rgba[i * 4 + c]];
                linear[i * 4 + 3] = float_to_half(rgba[i * 4 + 3] / 255.f);
            }
        });
        std::vector<uint8_t>().swap(rgba_storage);
        if (mode >= kExportAvifPq && mode <= kExportAvifGainMap) {
            return encode_avif_f16(w, h, linear.data(), cg, mode - kExportAvif, out_path, quality);
        }
        return encode_jxl_f16(w, h, linear.data(), cg, true, out_path, quality, mode == kExportJxlPq);
    }
    return encode_srgb_jpeg_u8(w, h, rgba, out_path, cg, quality);
}


int decode_jxr_memory(const uint8_t* data,size_t size,std::vector<uint16_t>& rgba,unsigned& w,unsigned& h,uhdr_color_gamut_t* gamut,unsigned thumbnail_edge) {
    bool untagged=false;
    if (!decode_jxr_from_memory(data,size,rgba,w,h,false,&untagged,gamut,thumbnail_edge))return -1;
    if(untagged)rebase_untagged_scrgb(rgba,w,h);
    return 0;
}

int encode_still_export_f32(const float* src,unsigned w,unsigned h,int gamut,int mode,
                            const char* path,StillEncodeQuality quality,const float* sdr_base) {
    if(!src||!path||!w||!h||mode<0||mode>kExportJxlPq||uint64_t(w)*h>80000000)return -50;
    const auto cg=export_gamut(gamut);const size_t pixels=size_t(w)*h;
    const float background=quality.transparency_background_white?1.f:0.f;
    if(mode==kExportJxl||mode==kExportJxlHdr||mode==kExportJxlPq)
        return encode_jxl_f32(w,h,src,cg,mode!=kExportJxl,path,quality,mode==kExportJxlPq);
    if(mode>=kExportAvif&&mode<=kExportAvifGainMap)
        return encode_avif_f32(w,h,src,cg,mode-kExportAvif,path,quality,sdr_base);
    if(mode==kExportJpeg) {
        std::vector<uint8_t> rgba(pixels*4);
        for(size_t i=0;i<pixels;++i) {
            const float alpha=std::isfinite(src[i*4+3])?std::clamp(src[i*4+3],0.f,1.f):0.f;
            for(unsigned c=0;c<3;++c)rgba[i*4+c]=linear_to_srgb_u8((alpha>0.f?src[i*4+c]*alpha:0.f)+background*(1.f-alpha));
            rgba[i*4+3]=255;
        }
        return encode_srgb_jpeg_u8(w,h,rgba.data(),path,cg,quality);
    }
    // Ultra HDR's public codec accepts linear half, not float32. Narrow only at
    // this external boundary; its JPEG base is necessarily 8-bit.
    std::vector<uint16_t> half(pixels*4);std::vector<uint8_t> base(sdr_base?pixels*4:0);
    for(size_t i=0;i<pixels;++i) {
        const float alpha=std::isfinite(src[i*4+3])?std::clamp(src[i*4+3],0.f,1.f):0.f;
        for(unsigned c=0;c<3;++c) {
            float v=(alpha>0.f?src[i*4+c]*alpha:0.f)+background*(1.f-alpha);if(!std::isfinite(v))v=0.f;
            half[i*4+c]=float_to_half(std::clamp(v,0.f,kMaxLinear));
            if(sdr_base)base[i*4+c]=linear_to_srgb_u8((alpha>0.f?sdr_base[i*4+c]*alpha:0.f)+background*(1.f-alpha));
        }
        half[i*4+3]=float_to_half(1.f);if(sdr_base)base[i*4+3]=255;
    }
    return encode_linear_rgba_f16_to_uhdr(w,h,half.data(),path,cg,0.f,true,sdr_base?base.data():nullptr,quality);
}

// Area filtering in linear light. RGB is premultiplied only while filtering;
// fractional boundary weights avoid uneven sampling at non-integer scales.
template<class Sample>
void scale_rgba_working(std::vector<Sample>& rgba,unsigned& w,unsigned& h,unsigned max_edge) {
    const unsigned edge=std::max(w,h);
    if(!max_edge||!w||!h||edge<=max_edge)return;
    const unsigned nw=std::max(1u,static_cast<unsigned>(uint64_t(w)*max_edge/edge));
    const unsigned nh=std::max(1u,static_cast<unsigned>(uint64_t(h)*max_edge/edge));
    std::vector<Sample> out(size_t(nw)*nh*4);
    for(unsigned y=0;y<nh;++y) {
        const double top=double(y)*h/nh,bottom=double(y+1)*h/nh;
        const unsigned y0=static_cast<unsigned>(top),y1=std::min(h,static_cast<unsigned>(std::ceil(bottom)));
        for(unsigned x=0;x<nw;++x) {
            const double left=double(x)*w/nw,right=double(x+1)*w/nw;
            const unsigned x0=static_cast<unsigned>(left),x1=std::min(w,static_cast<unsigned>(std::ceil(right)));
            double sum[4]={0,0,0,0},area=0;
            for(unsigned sy=y0;sy<y1;++sy) {
                const double wy=std::min(bottom,double(sy+1))-std::max(top,double(sy));
                for(unsigned sx=x0;sx<x1;++sx) {
                    const double weight=wy*(std::min(right,double(sx+1))-std::max(left,double(sx)));
                    const size_t i=(size_t(sy)*w+sx)*4;
                    const float a=read_working(rgba[i+3]);
                    const double alpha=std::isfinite(a)?std::clamp(a,0.f,1.f):0.f;
                    if(alpha>0)for(unsigned c=0;c<3;++c) {
                        const float v=read_working(rgba[i+c]);
                        if(std::isfinite(v))sum[c]+=double(v)*alpha*weight;
                    }
                    sum[3]+=alpha*weight;area+=weight;
                }
            }
            const size_t o=(size_t(y)*nw+x)*4;
            for(unsigned c=0;c<3;++c)out[o+c]=store_working<Sample>(sum[3]>0?static_cast<float>(sum[c]/sum[3]):0.f);
            out[o+3]=store_working<Sample>(area>0?static_cast<float>(sum[3]/area):0.f);
        }
    }
    rgba.swap(out);w=nw;h=nh;
}
void scale_rgba_f32_max_edge(std::vector<float>& rgba,unsigned& w,unsigned& h,unsigned max_edge) {
    scale_rgba_working(rgba,w,h,max_edge);
}
void scale_rgba_f16_max_edge(std::vector<uint16_t>& rgba,unsigned& w,unsigned& h,unsigned max_edge) {
    scale_rgba_working(rgba,w,h,max_edge);
}

jxrc::ImageInfo inspect_jxr(const uint8_t* data,size_t size) {
    PKFactory* factory=nullptr;WMPStream* stream=nullptr;PKImageDecode* decoder=nullptr;
    struct Cleanup {
        PKFactory*& f;WMPStream*& s;PKImageDecode*& d;
        ~Cleanup(){if(d)d->Release(&d);if(s)s->Close(&s);if(f)f->Release(&f);}
    } cleanup{factory,stream,decoder};
    if(Failed(PKCreateFactory(&factory,PK_SDK_VERSION))||!factory||Failed(factory->CreateStreamFromMemory(&stream,const_cast<uint8_t*>(data),size))||!stream||Failed(PKImageDecode_Create_WMP(&decoder))||!decoder||Failed(decoder->Initialize(decoder,stream)))throw std::runtime_error("Invalid JPEG XR header");
    decoder->fStreamOwner=1;stream=nullptr;I32 w=0,h=0;PKPixelFormatGUID guid{};
    if(Failed(decoder->GetSize(decoder,&w,&h))||Failed(decoder->GetPixelFormat(decoder,&guid))||w<=0||h<=0)throw std::runtime_error("Invalid JPEG XR dimensions");
    jxrc::ImageInfo info;info.width=w;info.height=h;info.format="JXR";info.bits=8;
    const auto format=classify_guid(guid);
    switch(format){case JxrPix::RgbaF32:case JxrPix::RgbaF32Prem:info.bits=32;info.hdr=true;break;
        case JxrPix::RgbaF16:case JxrPix::Fixed16:info.bits=16;info.hdr=true;break;
        case JxrPix::Rgb16:case JxrPix::Rgba16:case JxrPix::Gray16:info.bits=16;break;
        case JxrPix::Rgb101010:info.bits=10;break;default:break;}
    info.transfer=info.hdr?"Linear":"SDR";info.primaries="Source profile";return info;
}

int decode_jxr_f32(const uint8_t* data,size_t size,std::vector<float>& rgba,unsigned& w,unsigned& h,uhdr_color_gamut_t* gamut,bool to_bt2020) {
    bool untagged=false;
    if(!decode_jxr_from_memory(data,size,rgba,w,h,false,&untagged,gamut,0,nullptr))return -1;
    if(untagged) {
        rebase_untagged_scrgb(rgba,w,h,to_bt2020);
        if(gamut)*gamut=to_bt2020?UHDR_CG_BT_2100:UHDR_CG_BT_709;
    }
    return 0;
}

// Native scRGB samples, without the 80/203 rebase or gamut conversion.
bool decode_jxr_scrgb(const uint8_t* data,size_t size,std::vector<float>& rgba,unsigned& w,unsigned& h,bool& half) {
    bool untagged=false;int native=0;
    if(!decode_jxr_from_memory(data,size,rgba,w,h,false,&untagged,nullptr,0,nullptr,&native))return false;
    half=native==1;
    return untagged && native!=0;
}
