#include "working_pixels.h"
#include "render_analysis.h"
#include "heif_decode.h"
#include "gain_map.h"
// Derived from LocalViewer; see NOTICE and COPYING. Android entry points removed.
/*
 * AVIF still decode via libavif → Ultra HDR JPEG (libultrahdr).
 *
 * - ISO 21496-1 gain-map AVIF (`tmap`): shared F32 gain reconstruction,
 *   including ICC profiles, arbitrary source depths and signed gain ranges.
 * - Absolute PQ/HLG AVIF: decode with libavif, linearize in **source primaries**,
 *   encode content-matched Ultra HDR JPEG tagged with matching libultrahdr gamut
 *   (BT.2100 / Display P3 / BT.709). Do not force BT.709 clip when the format can
 *   carry the source gamut.
 *
 * JNI: com.hippo.ehviewer.jni.HdrConvertKt.convertAvifBytesToUltraHdr
 */






#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "avif/avif.h"
#include "hdr_encode.h"
#include "hdr_parallel.h"
#include "linear_color_profile.h"
#include "ultrahdr_api.h"

#define LOG_TAG "AvifHdr"
#define ALOGE(...) ((void)0)
#define ALOGI(...) ((void)0)

// BT.2100 reference display OOTF: scene-linear HLG to 203-nit-relative RGB.
static void hlg_display_rgb(float& r,float& g,float& b,uhdr_color_gamut_t cg) {
    const float yr=cg==UHDR_CG_DISPLAY_P3?0.2289746f:cg==UHDR_CG_BT_2100?0.2627f:0.2126f;
    const float yb=cg==UHDR_CG_DISPLAY_P3?0.0792869f:cg==UHDR_CG_BT_2100?0.0593f:0.0722f;
    const float scene_y=(yr*r+(1.f-yr-yb)*g+yb*b)/12.f;
    const float scale=std::pow(std::max(scene_y,0.f),0.2f)*(1000.f/(12.f*kSdrWhiteNits));
    r*=scale;g*=scale;b*=scale;
}

// Parse container metadata only, independently of platform AV1/profile support.
namespace {

// SMPTE ST 2084 PQ EOTF → linear relative luminance [0, 10000] nits-ish (absolute).
float pq_eotf(float n) {
    if (n <= 0.f) return 0.f;
    if (n >= 1.f) n = 1.f;
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    const float np = std::pow(n, 1.f / m2);
    float num = np - c1;
    if (num < 0.f) num = 0.f;
    const float den = c2 - c3 * np;
    if (den <= 1e-6f) return 10000.f;
    return 10000.f * std::pow(num / den, 1.f / m1);
}

// HLG inverse OETF (BT.2100) → relative scene-linear [0, 12] roughly, then scale.
float hlg_inv_oetf(float x) {
    if (x <= 0.f) return 0.f;
    if (x >= 1.f) x = 1.f;
    const float a = 0.17883277f;
    const float b = 0.28466892f;
    const float c = 0.55991073f;
    if (x <= 0.5f) {
        return (x * x) / 3.f;
    }
    return (std::exp((x - c) / a) + b) / 12.f;
}

// RGB conversion produces integer 16-bit codes. Evaluate each transfer once
// per possible code, rather than millions of pow/exp calls for every PQ page.
std::vector<float> make_avif_linear_lut(avifTransferCharacteristics tc) {
    std::vector<float> lut(65536);
    for (unsigned i = 0; i < lut.size(); ++i) {
        const float v = i / 65535.f;
        if (tc == AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084) lut[i] = pq_eotf(v) / kSdrWhiteNits;
        else if (tc == AVIF_TRANSFER_CHARACTERISTICS_HLG) lut[i] = hlg_inv_oetf(v) * 12.f;
        else if (tc == AVIF_TRANSFER_CHARACTERISTICS_LINEAR) lut[i] = v;
        else if (tc == AVIF_TRANSFER_CHARACTERISTICS_BT709 || tc == AVIF_TRANSFER_CHARACTERISTICS_BT601 ||
                 tc == AVIF_TRANSFER_CHARACTERISTICS_BT2020_10BIT || tc == AVIF_TRANSFER_CHARACTERISTICS_BT2020_12BIT) {
            lut[i] = v < 4.5f * 0.018053968510807f ? v / 4.5f :
                std::pow((v + 0.09929682680944f) / 1.09929682680944f, 1.f / 0.45f);
        }
        else if (tc == AVIF_TRANSFER_CHARACTERISTICS_BT470M) lut[i] = std::pow(v, 2.2f);
        else if (tc == AVIF_TRANSFER_CHARACTERISTICS_BT470BG) lut[i] = std::pow(v, 2.8f);
        else lut[i] = srgb_eotf(v);
    }
    return lut;
}

const std::vector<float>& avif_linear_lut(avifTransferCharacteristics tc) {
    if (tc == AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084) {
        static const auto lut = make_avif_linear_lut(AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084);
        return lut;
    }
    if (tc == AVIF_TRANSFER_CHARACTERISTICS_HLG) {
        static const auto lut = make_avif_linear_lut(AVIF_TRANSFER_CHARACTERISTICS_HLG);
        return lut;
    }
    if (tc == AVIF_TRANSFER_CHARACTERISTICS_LINEAR) {
        static const auto lut = make_avif_linear_lut(AVIF_TRANSFER_CHARACTERISTICS_LINEAR);
        return lut;
    }
    if (tc == AVIF_TRANSFER_CHARACTERISTICS_BT709 || tc == AVIF_TRANSFER_CHARACTERISTICS_BT601 ||
        tc == AVIF_TRANSFER_CHARACTERISTICS_BT2020_10BIT || tc == AVIF_TRANSFER_CHARACTERISTICS_BT2020_12BIT) {
        static const auto lut = make_avif_linear_lut(AVIF_TRANSFER_CHARACTERISTICS_BT709);
        return lut;
    }
    if (tc == AVIF_TRANSFER_CHARACTERISTICS_BT470M) {
        static const auto lut = make_avif_linear_lut(AVIF_TRANSFER_CHARACTERISTICS_BT470M);
        return lut;
    }
    if (tc == AVIF_TRANSFER_CHARACTERISTICS_BT470BG) {
        static const auto lut = make_avif_linear_lut(AVIF_TRANSFER_CHARACTERISTICS_BT470BG);
        return lut;
    }
    static const auto lut = make_avif_linear_lut(AVIF_TRANSFER_CHARACTERISTICS_SRGB);
    return lut;
}

/**
 * Map AVIF CICP primaries (+ transfer) → libultrahdr gamut tag.
 * Values and tags must stay consistent: if we keep BT.2020 RGB, tag BT_2100.
 */
uhdr_color_gamut_t map_avif_primaries_to_uhdr_cg(avifColorPrimaries primaries,
                                                   bool is_pq_or_hlg) {
    switch (primaries) {
        case AVIF_COLOR_PRIMARIES_BT2020:
            return UHDR_CG_BT_2100;
        case AVIF_COLOR_PRIMARIES_SMPTE432:  // Display P3 (D65)
            return UHDR_CG_DISPLAY_P3;
        case AVIF_COLOR_PRIMARIES_SMPTE431:  // DCI-P3 — closest Ultra HDR tag is Display P3
            return UHDR_CG_DISPLAY_P3;
        case AVIF_COLOR_PRIMARIES_BT709:
            return UHDR_CG_BT_709;
        default:
            // PQ/HLG with unspecified/unknown primaries: industry default is BT.2020.
            if (is_pq_or_hlg) return UHDR_CG_BT_2100;
            return UHDR_CG_BT_709;
    }
}

/** Two reader pages decode at once, so each decode takes half the cores. */
int avif_decode_threads() {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    unsigned per = hw / 2;
    if (per < 2) per = hw;
    if (per < 1) per = 1;
    if (per > 8) per = 8;
    return static_cast<int>(jxrc_limit_threads(per));
}

/**
 * Shrink YUV to the reader/thumb long edge before gain-map apply and YUV→RGB.
 * dav1d has already reconstructed the full frame. [max_edge] 0 keeps it.
 * @return false when the image was left unusable.
 */
bool scale_avif_to_max_edge(avifImage* image, unsigned max_edge) {
    if (image == nullptr || max_edge == 0 || image->width == 0 || image->height == 0) return true;
    const unsigned long_edge = image->width > image->height ? image->width : image->height;
    if (long_edge <= max_edge) return true;
    const double scale = static_cast<double>(max_edge) / static_cast<double>(long_edge);
    uint32_t dw = static_cast<uint32_t>(std::lround(static_cast<double>(image->width) * scale));
    uint32_t dh = static_cast<uint32_t>(std::lround(static_cast<double>(image->height) * scale));
    if (dw > image->width) dw = image->width;
    if (dh > image->height) dh = image->height;
    if (dw < 2) dw = 2;
    if (dh < 2) dh = 2;
    if ((dw & 1u) != 0u && dw > 2) dw -= 1u;
    if ((dh & 1u) != 0u && dh > 2) dh -= 1u;
    if (dw == image->width && dh == image->height) return true;
    avifDiagnostics diag;
    avifDiagnosticsClearError(&diag);
    [[maybe_unused]] const unsigned src_w = image->width;
    [[maybe_unused]] const unsigned src_h = image->height;
    const avifResult scaled = avifImageScale(image, dw, dh, &diag);
    if (scaled != AVIF_RESULT_OK) {
        ALOGE("avifImageScale %ux%u -> %ux%u failed: %s", src_w, src_h, dw, dh,
              diag.error[0] ? diag.error : avifResultToString(scaled));
        return false;
    }
    return true;
}

// Reconstruct in F32 for every source gamut/depth, including ICC-tagged bases.
// Half is a presentation conversion at the boundary, never a working raster.
template<class Sample>
bool apply_iso_gain_map(const avifImage* image,std::vector<Sample>& out,int* has_gain,
                        int* transfer,uhdr_color_gamut_t* gamut,std::vector<float>* sdr=nullptr,const std::vector<float>* gainCodes=nullptr) {
    std::vector<float> scratch;
    auto& pixels=[&]() -> std::vector<float>& {if constexpr(std::is_same_v<Sample,float>)return out;else return scratch;}();
    uhdr_color_gamut_t cg;
    if(!reconstruct_gain_map_f32(image,pixels,cg,sdr,gainCodes))return false;
    if constexpr(!std::is_same_v<Sample,float>) {
        out.resize(pixels.size());std::transform(pixels.begin(),pixels.end(),out.begin(),[](float v){return store_working<Sample>(sanitize_working<Sample>(v));});
    }
    if(gamut)*gamut=cg;
    if(transfer)*transfer=16;
    if(has_gain)*has_gain=1;
    return true;
}

/**
 * Decode AVIF → linear RGBA half in source (or fallback BT.709) primaries.
 * 1.0 = SDR white / 203 nits for PQ.
 *
 * @param out_cg libultrahdr gamut matching out_rgba (required for accurate encode).
 * @return 0 OK, non-zero on failure. Sets out_has_gainmap if libavif reports a gain map.
 */
template<class Sample>
int linearize_avif_image(avifImage* image,std::vector<Sample>& out_rgba,unsigned& w,unsigned& h,
                        int* out_has_gainmap,int* out_transfer,uhdr_color_gamut_t* out_cg,unsigned max_edge,std::vector<float>* sdr=nullptr,const std::vector<float>* gainCodes=nullptr) {
    avifResult r=AVIF_RESULT_OK;
    if (!image || image->width == 0 || image->height == 0) {
        return -6;
    }
    if (image->width > 16384 || image->height > 16384) {
        return -7;
    }
    if (!scale_avif_to_max_edge(image, max_edge)) {
        return -11;
    }
    w = image->width;
    h = image->height;

    if (apply_iso_gain_map(image, out_rgba, out_has_gainmap, out_transfer, out_cg,sdr,gainCodes)) {
        return 0;
    }
    // An advertised HDR rendition must not silently become an SDR base when
    // reconstruction fails.
    if (image->gainMap && image->gainMap->image) {
        ALOGE("Cannot reconstruct the AVIF HDR gain map");
        return -12;
    }

    const avifTransferCharacteristics tc = image->transferCharacteristics;
    if (out_transfer) *out_transfer = static_cast<int>(tc);
    const bool is_pq = (tc == AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084);
    const bool is_hlg = (tc == AVIF_TRANSFER_CHARACTERISTICS_HLG);
    const avifColorPrimaries primaries = image->colorPrimaries;
    const bool is_bt2020 = (primaries == AVIF_COLOR_PRIMARIES_BT2020);

    // Preserve source gamut when libultrahdr can tag it (accuracy-first).
    // Policy B (convert → BT.709) only if we ever tag 709 while source was BT.2020.
    uhdr_color_gamut_t cg = map_avif_primaries_to_uhdr_cg(primaries, is_pq || is_hlg);
    const bool need_bt2020_to_709 = (cg == UHDR_CG_BT_709 && is_bt2020);
    if (out_cg) *out_cg = cg;

    avifRGBImage rgb;
    memset(&rgb, 0, sizeof(rgb));
    avifRGBImageSetDefaults(&rgb, image);
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.depth = 16;
    rgb.maxThreads = avif_decode_threads();
    rgb.chromaUpsampling = AVIF_CHROMA_UPSAMPLING_BEST_QUALITY;

    r = avifRGBImageAllocatePixels(&rgb);
    if (r != AVIF_RESULT_OK) {
        ALOGE("avifRGBImageAllocatePixels: %s", avifResultToString(r));
        return -8;
    }
    r = avifImageYUVToRGB(image, &rgb);
    if (r != AVIF_RESULT_OK) {
        ALOGE("avifImageYUVToRGB: %s", avifResultToString(r));
        avifRGBImageFreePixels(&rgb);
        return -9;
    }

    const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
    out_rgba.resize(pixels * 4);
    const uint8_t* row_base = rgb.pixels;
    const uint32_t row_bytes = rgb.rowBytes;

    // libavif converts YUV to encoded RGB; it does not apply the image ICC.
    // For SDR the complete ICC (TRC, white point and primaries) takes priority.
    // PQ/HLG use CICP explicitly: HDR ICCs can contain an SDR fallback TRC.
    if (image->icc.size && !is_pq && !is_hlg) {
        skcms_ICCProfile source{}, destination{};
        bool ok = image->icc.size <= 16u * 1024u * 1024u &&
            skcms_Parse(image->icc.data, image->icc.size, &source) &&
            make_linear_bt2020_profile(&destination);
        for (unsigned y = 0; ok && y < h; ++y) {
            ok = skcms_Transform(row_base + static_cast<size_t>(y) * row_bytes,
                skcms_PixelFormat_RGBA_16161616LE, skcms_AlphaFormat_Unpremul, &source,
                out_rgba.data() + static_cast<size_t>(y) * w * 4,
                working_format<Sample>(), skcms_AlphaFormat_Unpremul, &destination, w);
        }
        avifRGBImageFreePixels(&rgb);
        if (!ok) {
            ALOGE("AVIF ICC conversion failed");
            return -13;
        }
        if (out_cg) *out_cg = UHDR_CG_BT_2100;
        if (out_transfer) *out_transfer = 0;
        return 0;
    }

    const auto& linear = avif_linear_lut(tc);
    hdr_parallel_pixels(h, hdr_pixel_jobs(pixels, avif_decode_threads()), [&](size_t begin, size_t end, unsigned) {
        for (size_t y = begin; y < end; y++) {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(row_base + static_cast<size_t>(y) * row_bytes);
            for (unsigned x = 0; x < w; x++) {
                const uint16_t* p = row + x * 4;
                // 16-bit full range normalized (transfer-encoded if PQ/HLG)
                float an = p[3] / 65535.f;
                float rl = linear[p[0]], gl = linear[p[1]], bl = linear[p[2]];

                if (is_hlg) hlg_display_rgb(rl,gl,bl,cg);

                // Policy B only: values must match the BT.709 tag. Preserve path never rematrixes.
                if (need_bt2020_to_709) {
                    linear_bt2020_to_bt709(rl, gl, bl);
                }

                // Clamp to Ultra HDR LINEAR nominal max (10000/203).
                auto clamp_hf = [](float v) { return sanitize_working<Sample>(v); };
                rl = clamp_hf(rl);
                gl = clamp_hf(gl);
                bl = clamp_hf(bl);
                if (an < 0.f) an = 0.f;
                if (an > 1.f) an = 1.f;

                const size_t i = static_cast<size_t>(y) * w + x;
                out_rgba[i * 4 + 0] = store_working<Sample>(rl);
                out_rgba[i * 4 + 1] = store_working<Sample>(gl);
                out_rgba[i * 4 + 2] = store_working<Sample>(bl);
                out_rgba[i * 4 + 3] = store_working<Sample>(an);
            }
        }
    });

    // DCI-P3 has a different white point from Display P3. Other CICP gamuts
    // likewise need a real transform instead of assigning a nearby gamut tag.
    if (primaries != AVIF_COLOR_PRIMARIES_BT709 &&
        primaries != AVIF_COLOR_PRIMARIES_BT2020 &&
        primaries != AVIF_COLOR_PRIMARIES_SMPTE432 &&
        primaries != AVIF_COLOR_PRIMARIES_UNKNOWN &&
        primaries != AVIF_COLOR_PRIMARIES_UNSPECIFIED) {
        float xy[8];
        avifColorPrimariesGetValues(primaries, xy);
        skcms_ICCProfile source{}, destination{};
        skcms_Init(&source);
        skcms_SetTransferFunction(&source, skcms_Identity_TransferFunction());
        skcms_Matrix3x3 matrix{};
        bool ok = skcms_PrimariesToXYZD50(xy[0], xy[1], xy[2], xy[3], xy[4], xy[5], xy[6], xy[7], &matrix);
        skcms_SetXYZD50(&source, &matrix);
        ok = ok && make_linear_bt2020_profile(&destination) &&
            skcms_Transform(out_rgba.data(), working_format<Sample>(), skcms_AlphaFormat_Unpremul,
                &source, out_rgba.data(), working_format<Sample>(), skcms_AlphaFormat_Unpremul,
                &destination, pixels);
        if (!ok) {
            avifRGBImageFreePixels(&rgb);
            return -14;
        }
        cg = UHDR_CG_BT_2100;
        if (out_cg) *out_cg = cg;
    }

    ALOGI("AVIF %ux%u tc=%d primaries=%d pq=%d hlg=%d cg=%d rematrix709=%d gainmap=%d", w, h,
          (int)tc, (int)primaries, is_pq ? 1 : 0, is_hlg ? 1 : 0, (int)cg,
          need_bt2020_to_709 ? 1 : 0, out_has_gainmap ? *out_has_gainmap : 0);

    avifRGBImageFreePixels(&rgb);
    return 0;
}

template<class Sample>
int decode_avif_impl(const uint8_t* data, size_t len, std::vector<Sample>& out_rgba,
                              unsigned& w, unsigned& h, int* out_has_gainmap,
                              int* out_transfer /* 16=PQ, 18=HLG, else other */,
                              uhdr_color_gamut_t* out_cg, unsigned max_edge, std::vector<uint8_t>* out_exif = nullptr,std::vector<float>* sdr=nullptr) {
    if (out_exif) out_exif->clear();
    if (out_has_gainmap) *out_has_gainmap = 0;
    if (out_transfer) *out_transfer = 0;
    if (out_cg) *out_cg = UHDR_CG_BT_709;
    if (!data || len == 0) return -1;

    avifDecoder* dec = avifDecoderCreate();
    if (!dec) return -2;
    dec->imageSizeLimit = 80000000;
    dec->ignoreExif = out_exif ? AVIF_FALSE : AVIF_TRUE;
    dec->ignoreXMP = AVIF_TRUE;
    // Default decode skips the gain-map item. ISO 21496-1 AVIF needs those pixels.
    dec->imageContentToDecode = AVIF_IMAGE_CONTENT_ALL;
    // dav1d API 7 uses this as the tile-thread pool (one still, so frame delay stays 1).
    dec->maxThreads = avif_decode_threads();
    avifResult r = avifDecoderSetIOMemory(dec, data, len);
    if (r != AVIF_RESULT_OK) {
        ALOGE("avifDecoderSetIOMemory: %s", avifResultToString(r));
        avifDecoderDestroy(dec);
        return -3;
    }
    r = avifDecoderParse(dec);
    if (r != AVIF_RESULT_OK && out_exif) {
        // Optional malformed EXIF must not turn a previously readable AVIF into an error.
        dec->ignoreExif = AVIF_TRUE;
        r = avifDecoderParse(dec);
    }
    if (r != AVIF_RESULT_OK) {
        ALOGE("avifDecoderParse: %s", avifResultToString(r));
        avifDecoderDestroy(dec);
        return -4;
    }
    if (out_exif && dec->image && dec->image->exif.data && dec->image->exif.size <= 1024 * 1024) {
        const auto& exif = dec->image->exif;
        size_t offset = 0;
        if (avifGetExifTiffHeaderOffset(exif.data, exif.size, &offset) == AVIF_RESULT_OK && offset < exif.size) {
            out_exif->assign(exif.data + offset, exif.data + exif.size);
        }
    }

    r = avifDecoderNextImage(dec);
    if (r != AVIF_RESULT_OK) {
        ALOGE("avifDecoderNextImage: %s", avifResultToString(r));
        avifDecoderDestroy(dec);
        return -5;
    }

    const int result=linearize_avif_image(dec->image,out_rgba,w,h,out_has_gainmap,out_transfer,out_cg,max_edge,sdr);
    avifDecoderDestroy(dec);return result;
}

}  // namespace


int decode_avif_to_linear_f16(const uint8_t* data,size_t size,std::vector<uint16_t>& rgba,unsigned& w,unsigned& h,
    int* gain,int* transfer,uhdr_color_gamut_t* gamut,unsigned edge,std::vector<uint8_t>* exif) {
    return decode_avif_impl(data,size,rgba,w,h,gain,transfer,gamut,edge,exif);
}

int decode_avif_to_linear_f32(const uint8_t* data,size_t size,std::vector<float>& rgba,unsigned& w,unsigned& h,
    int* gain,int* transfer,uhdr_color_gamut_t* gamut,std::vector<uint8_t>* exif,std::vector<float>* sdr) {
    return decode_avif_impl(data,size,rgba,w,h,gain,transfer,gamut,0,exif,sdr);
}

template<class Sample>
int decode_heif_linear(const uint8_t* data,size_t size,std::vector<Sample>& rgba,unsigned& w,unsigned& h,
    uhdr_color_gamut_t* gamut,unsigned edge,std::vector<uint8_t>* exif,std::vector<float>* sdr=nullptr) {
    std::vector<float> gainCodes;auto image=decode_heif_avif(data,size,edge,nullptr,nullptr,&gainCodes);
    if(exif){exif->clear();if(image->exif.size)exif->assign(image->exif.data,image->exif.data+image->exif.size);}
    int gain=0,transfer=0;
    return linearize_avif_image(image.get(),rgba,w,h,&gain,&transfer,gamut,edge,sdr,gainCodes.empty()?nullptr:&gainCodes);
}
int decode_heif_to_linear_f32(const uint8_t* data,size_t size,std::vector<float>& rgba,unsigned& w,unsigned& h,uhdr_color_gamut_t* gamut,std::vector<uint8_t>* exif,std::vector<float>* sdr) {
    return decode_heif_linear(data,size,rgba,w,h,gamut,0,exif,sdr);
}
int decode_heif_to_linear_f16(const uint8_t* data,size_t size,std::vector<uint16_t>& rgba,unsigned& w,unsigned& h,uhdr_color_gamut_t* gamut,unsigned edge) {
    return decode_heif_linear(data,size,rgba,w,h,gamut,edge,nullptr);
}

const std::vector<float>& pq_linear_lut() {return avif_linear_lut(AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084);}

// PNG samples have already been expanded by libpng, without gamma conversion.
template<class Sample> static void cicp_rgb_to_linear_impl(const uint8_t* codes,size_t count,unsigned bits,unsigned primaries,unsigned transfer,
                        std::vector<Sample>& rgba,uhdr_color_gamut_t& gamut) {
    gamut=primaries==9?UHDR_CG_BT_2100:primaries==12?UHDR_CG_DISPLAY_P3:UHDR_CG_BT_709;
    const auto& lut=avif_linear_lut(static_cast<avifTransferCharacteristics>(transfer));
    rgba.resize(count*4);
    hdr_parallel_pixels(count,hdr_pixel_jobs(count),[&](size_t begin,size_t end,unsigned) {
        for(size_t i=begin;i<end;++i) {
            uint16_t p[4];
            if(bits==16)std::memcpy(p,codes+i*8,8);
            else for(unsigned c=0;c<4;++c)p[c]=uint16_t(codes[i*4+c])*257;
            float r=lut[p[0]],g=lut[p[1]],b=lut[p[2]];
            if(transfer==18)hlg_display_rgb(r,g,b,gamut);
            rgba[i*4]=store_working<Sample>(r);rgba[i*4+1]=store_working<Sample>(g);
            rgba[i*4+2]=store_working<Sample>(b);rgba[i*4+3]=store_working<Sample>(p[3]/65535.f);
        }
    });
}

void cicp_rgb_to_linear(const uint8_t* codes,size_t count,unsigned bits,unsigned primaries,unsigned transfer,
                        std::vector<float>& rgba,uhdr_color_gamut_t& gamut) {
    cicp_rgb_to_linear_impl(codes,count,bits,primaries,transfer,rgba,gamut);
}

void cicp_rgb_to_linear(const uint8_t* codes,size_t count,unsigned bits,unsigned primaries,unsigned transfer,
                        std::vector<uint16_t>& rgba,uhdr_color_gamut_t& gamut) {
    cicp_rgb_to_linear_impl(codes,count,bits,primaries,transfer,rgba,gamut);
}
