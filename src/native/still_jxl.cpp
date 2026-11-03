// Derived from LocalViewer; see NOTICE and COPYING. Android entry points removed.
#include "hdr_encode.h"
#include "hdr_parallel.h"


#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <type_traits>
#include <jxl/encode_cxx.h>
#include <avif/avif.h>
#include <jxl/thread_parallel_runner_cxx.h>

namespace {
struct JxlFileCloser {
    void operator()(FILE* file) const { fclose(file); }
};
}

static int write_jxl_output(JxlEncoder* encoder, const char* path) {
    std::unique_ptr<FILE, JxlFileCloser> file(fopen(path, "wb"));
    if (!file) return -65;
    // Stream encoded bytes directly to disk; no second complete compressed file.
    std::array<uint8_t, 65536> output{};
    for (;;) {
        uint8_t* next = output.data();
        size_t available = output.size();
        const auto status = JxlEncoderProcessOutput(encoder, &next, &available);
        const size_t count = output.size() - available;
        if (count && fwrite(output.data(), 1, count, file.get()) != count) return -66;
        if (status == JXL_ENC_SUCCESS) break;
        if (status != JXL_ENC_NEED_MORE_OUTPUT) return -67;
    }
    return fflush(file.get()) == 0 ? 0 : -66;
}

static int encode_jxl_pixels(unsigned w, unsigned h, const void* rgba, uhdr_color_gamut_t cg,
                             const char* path, StillEncodeQuality quality, JxlDataType type,
                             JxlTransferFunction transfer, float intensity, const StillSourceColor* source = nullptr) {
    const size_t pixels = static_cast<size_t>(w) * h;
    if (!rgba || !path || !pixels) return -60;
    const unsigned input_channels = source ? source->channels : 4;
    const bool alpha = input_channels == 2 || input_channels == 4;
    const bool gray = input_channels <= 2 || (source && source->icc_size >= 20 && !std::memcmp(source->icc + 16, "GRAY", 4));
    auto encoder = JxlEncoderMake(nullptr);
    const unsigned threads = std::min(4u, hdr_pixel_jobs(pixels));
    auto runner = JxlThreadParallelRunnerMake(nullptr, threads);
    if (!encoder || !runner || JxlEncoderSetParallelRunner(encoder.get(), JxlThreadParallelRunner, runner.get()) != JXL_ENC_SUCCESS) return -61;
    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = w;
    info.ysize = h;
    info.bits_per_sample = source ? source->bits : type == JXL_TYPE_UINT8 ? 8 : type == JXL_TYPE_FLOAT ? 32 : 16;
    if (source) info.orientation = static_cast<JxlOrientation>(source->orientation);
    info.exponent_bits_per_sample = source ? source->exponent_bits : type == JXL_TYPE_FLOAT ? 8 : type == JXL_TYPE_FLOAT16 ? 5 : 0;
    info.num_color_channels = gray ? 1 : 3;
    info.num_extra_channels = alpha ? 1 : 0;
    info.alpha_bits = alpha ? info.bits_per_sample : 0;
    info.alpha_exponent_bits = alpha ? info.exponent_bits_per_sample : 0;
    info.alpha_premultiplied = JXL_FALSE;
    const float distance = quality.jxl_distance();
    const bool compression = distance > 0.f;
    info.uses_original_profile = compression ? JXL_FALSE : JXL_TRUE;
    info.intensity_target = intensity;
    JxlColorEncoding color{};
    color.color_space = gray ? JXL_COLOR_SPACE_GRAY : JXL_COLOR_SPACE_RGB;
    color.white_point = JXL_WHITE_POINT_D65;
    color.primaries = cg == UHDR_CG_BT_2100 ? JXL_PRIMARIES_2100 : cg == UHDR_CG_DISPLAY_P3 ? JXL_PRIMARIES_P3 : JXL_PRIMARIES_SRGB;
    color.transfer_function = transfer;
    if (source) {
        if (source->primaries == 12 || source->primaries == 11) {
            color.primaries = JXL_PRIMARIES_P3;
            color.white_point = source->primaries == 11 ? JXL_WHITE_POINT_DCI : JXL_WHITE_POINT_D65;
        } else if (source->primaries == 1 || source->primaries == 9) {
            color.primaries = static_cast<JxlPrimaries>(source->primaries);
        } else if (source->primaries == 0 || source->primaries == 2) {
            // Unspecified CICP is rendered as sRGB; JXL has no "unknown" encoding.
            color.primaries = JXL_PRIMARIES_SRGB;
        } else if (!source->icc_size) {
            float xy[8];avifColorPrimariesGetValues(static_cast<avifColorPrimaries>(source->primaries), xy);
            color.primaries = JXL_PRIMARIES_CUSTOM;color.white_point = JXL_WHITE_POINT_CUSTOM;
            color.primaries_red_xy[0]=xy[0];color.primaries_red_xy[1]=xy[1];
            color.primaries_green_xy[0]=xy[2];color.primaries_green_xy[1]=xy[3];
            color.primaries_blue_xy[0]=xy[4];color.primaries_blue_xy[1]=xy[5];
            color.white_point_xy[0]=xy[6];color.white_point_xy[1]=xy[7];
        }
        color.transfer_function = static_cast<JxlTransferFunction>(source->transfer);
        if (source->transfer == 4 || source->transfer == 5) {
            color.transfer_function = JXL_TRANSFER_FUNCTION_GAMMA;
            color.gamma = source->transfer == 4 ? 1.0/2.2 : 1.0/2.8;
        } else if (source->transfer == 6 || source->transfer == 14 || source->transfer == 15) {
            color.transfer_function = JXL_TRANSFER_FUNCTION_709;
        } else if (source->transfer == 0 || source->transfer == 2) {
            color.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
        }
    }
    color.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
    if (JxlEncoderSetBasicInfo(encoder.get(), &info) != JXL_ENC_SUCCESS ||
        (source && source->icc_size ?
            JxlEncoderSetICCProfile(encoder.get(), source->icc, source->icc_size) :
            JxlEncoderSetColorEncoding(encoder.get(), &color)) != JXL_ENC_SUCCESS) return -62;
    auto* frame = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
    // Lossless keeps original-profile / fast-decode coding. High/mid/low use
    // perceptual XYB at distances 0.5/1.0/1.5, preserving primaries, transfer
    // and HDR intensity target. Alpha remains lossless in every preset.
    if (!frame || JxlEncoderSetFrameLossless(frame, compression ? JXL_FALSE : JXL_TRUE) != JXL_ENC_SUCCESS ||
        JxlEncoderSetFrameDistance(frame, distance) != JXL_ENC_SUCCESS ||
        (alpha && JxlEncoderSetExtraChannelDistance(frame, 0, 0.f) != JXL_ENC_SUCCESS) ||
        JxlEncoderFrameSettingsSetOption(frame, JXL_ENC_FRAME_SETTING_EFFORT, compression ? 5 : 3) != JXL_ENC_SUCCESS ||
        JxlEncoderFrameSettingsSetOption(frame, JXL_ENC_FRAME_SETTING_DECODING_SPEED, compression ? 0 : 2) != JXL_ENC_SUCCESS) return -63;
    // libjxl 0.12's weighted predictor can overflow its signed residual when
    // lossless float32 bit patterns cross zero. A zero predictor with no MA
    // learning stores those patterns exactly without reducing pixel precision.
    if (!compression && info.bits_per_sample == 32 && info.exponent_bits_per_sample == 8) {
        // Effort 3 forces a weighted DC tree even with an explicit predictor.
        if (JxlEncoderFrameSettingsSetOption(frame, JXL_ENC_FRAME_SETTING_EFFORT, 4) != JXL_ENC_SUCCESS ||
            JxlEncoderFrameSettingsSetOption(frame, JXL_ENC_FRAME_SETTING_MODULAR_PREDICTOR, 0) != JXL_ENC_SUCCESS ||
            JxlEncoderFrameSettingsSetFloatOption(frame, JXL_ENC_FRAME_SETTING_MODULAR_MA_TREE_LEARNING_PERCENT, 0.f) != JXL_ENC_SUCCESS) return -63;
    }
    const unsigned output_channels = (gray ? 1u : 3u) + (alpha ? 1u : 0u);
    const JxlPixelFormat format{output_channels, type, JXL_NATIVE_ENDIAN, 0};
    std::vector<uint8_t> gray_alpha;
    const size_t sample_bytes = type == JXL_TYPE_UINT8 ? 1 : type == JXL_TYPE_FLOAT ? 4 : 2;
    const void* input = rgba;
    if (gray && input_channels != output_channels) {
        gray_alpha.resize(pixels * sample_bytes * output_channels);
        for (size_t i=0;i<pixels;++i) {
            std::memcpy(gray_alpha.data()+i*sample_bytes*output_channels, static_cast<const uint8_t*>(rgba)+i*sample_bytes*input_channels, sample_bytes);
            if (alpha) std::memcpy(gray_alpha.data()+(i*output_channels+1)*sample_bytes,
                static_cast<const uint8_t*>(rgba)+(i*input_channels+input_channels-1)*sample_bytes, sample_bytes);
        }
        input = gray_alpha.data();
    }
    if (quality.exif_size) {
        if (JxlEncoderUseContainer(encoder.get(), JXL_TRUE) != JXL_ENC_SUCCESS ||
            JxlEncoderUseBoxes(encoder.get()) != JXL_ENC_SUCCESS) return -62;
        std::vector<uint8_t> box(4 + quality.exif_size, 0);
        std::copy(quality.exif, quality.exif + quality.exif_size, box.begin() + 4);
        if (JxlEncoderAddBox(encoder.get(), "Exif", box.data(), box.size(), JXL_FALSE) != JXL_ENC_SUCCESS) return -62;
        JxlEncoderCloseBoxes(encoder.get());
    }
    if (JxlEncoderAddImageFrame(frame, &format, input, pixels * sample_bytes * output_channels) != JXL_ENC_SUCCESS) return -64;
    JxlEncoderCloseInput(encoder.get());
    return write_jxl_output(encoder.get(), path);
}

// SDR output is transfer-encoded integer RGB, independent of working precision.
static int encode_jxl_sdr_f32(unsigned w,unsigned h,const float* rgba,uhdr_color_gamut_t cg,
                              const char* path,StillEncodeQuality quality) {
    const size_t samples=size_t(w)*h*4;
    const bool sixteen=quality.source_bits>8;
    auto pack=[&](auto& output,unsigned maximum) {
        for(size_t i=0;i<samples;++i) {
            float value=rgba[i];if(!std::isfinite(value))value=0.f;
            value=i%4==3?std::clamp(value,0.f,1.f):srgb_oetf(value);
            output[i]=static_cast<typename std::decay_t<decltype(output)>::value_type>(std::lround(value*maximum));
        }
    };
    if(sixteen) {
        std::vector<uint16_t> encoded(samples);pack(encoded,65535);
        return encode_jxl_pixels(w,h,encoded.data(),cg,path,quality,JXL_TYPE_UINT16,JXL_TRANSFER_FUNCTION_SRGB,kSdrWhiteNits);
    }
    std::vector<uint8_t> encoded(samples);pack(encoded,255);
    return encode_jxl_pixels(w,h,encoded.data(),cg,path,quality,JXL_TYPE_UINT8,JXL_TRANSFER_FUNCTION_SRGB,kSdrWhiteNits);
}

// The caller owns straight-alpha linear half samples. Normalize HDR to its
// declared luminance for linear JXL. PQ codes retain the absolute 10000-nit
// reference, while intensity_target reports the measured content peak (MaxCLL).
int encode_jxl_f16(unsigned w, unsigned h, uint16_t* rgba, uhdr_color_gamut_t cg,
                   bool hdr, const char* path, StillEncodeQuality quality, bool pq) {
    const size_t pixels = static_cast<size_t>(w) * h;
    if (!rgba || !path || !pixels) return -60;
    if(!hdr&&!pq) {
        std::vector<float> linear(pixels*4);
        std::transform(rgba,rgba+linear.size(),linear.begin(),half_to_float);
        return encode_jxl_sdr_f32(w,h,linear.data(),cg,path,quality);
    }
    float peak = pq ? kMaxLinear : 1.f;
    float pq_intensity = 1.f;
    if (hdr || pq) {
        std::array<float, 8> peaks{};
        const unsigned jobs = hdr_pixel_jobs(pixels);
        hdr_parallel_pixels(pixels, jobs, [&](size_t begin, size_t end, unsigned job) {
            float p = pq ? 0.f : 1.f;
            for (size_t i = begin; i < end; ++i) {
                for (unsigned c = 0; c < 3; ++c) {
                    const float v = half_to_float(rgba[4 * i + c]);
                    if (std::isfinite(v)) p = std::max(p, v);
                }
            }
            peaks[job] = p;
        });
        const float found = *std::max_element(peaks.begin(), peaks.end());
        if (pq) pq_intensity = std::clamp(found * kSdrWhiteNits, 1.f, kMaxNits);
        else peak = std::clamp(found, 400.f / kSdrWhiteNits, kMaxLinear);
    }
    const float intensity = pq ? pq_intensity : hdr ? peak * kSdrWhiteNits : kSdrWhiteNits;
    // One table also bounds invalid input and avoids per-channel floating-point
    // conversion in the image loop. Alpha stays straight; PQ converts its
    // storage to the same uint16 precision as the color channels.
    std::vector<uint16_t> table(65536);
    for (size_t code = 0; code < table.size(); ++code) {
        float v = half_to_float(static_cast<uint16_t>(code));
        if (!std::isfinite(v) || v < 0.f) v = 0.f;
        table[code] = pq ? static_cast<uint16_t>(std::lround(linear_to_pq_code(v) * 65535.f)) :
                           float_to_half(std::min(v, peak) / peak);
    }
    hdr_parallel_pixels(pixels, hdr_pixel_jobs(pixels), [&](size_t begin, size_t end, unsigned) {
        for (size_t i = begin; i < end; ++i) {
            for (unsigned c = 0; c < 3; ++c) rgba[4 * i + c] = table[rgba[4 * i + c]];
            if (pq) {
                float alpha = half_to_float(rgba[4 * i + 3]);
                if (!std::isfinite(alpha)) alpha = 0.f;
                rgba[4 * i + 3] = static_cast<uint16_t>(std::lround(std::clamp(alpha, 0.f, 1.f) * 65535.f));
            }
        }
    });
    return encode_jxl_pixels(w, h, rgba, cg, path, quality,
                             pq ? JXL_TYPE_UINT16 : JXL_TYPE_FLOAT16,
                             pq ? JXL_TRANSFER_FUNCTION_PQ : JXL_TRANSFER_FUNCTION_LINEAR, intensity);
}

int encode_jxl_f32(unsigned w,unsigned h,const float* rgba,uhdr_color_gamut_t cg,
                   bool hdr,const char* path,StillEncodeQuality quality,bool pq) {
    const size_t pixels=static_cast<size_t>(w)*h;
    if(!rgba||!path||!pixels)return -60;
    if(!hdr&&!pq)return encode_jxl_sdr_f32(w,h,rgba,cg,path,quality);
    float peak=pq?kMaxLinear:1.f;
    float pq_intensity=1.f;
    if(hdr||pq) {
        float found=1.f;
        const unsigned jobs=hdr_pixel_jobs(pixels);
        std::array<float,8> peaks{};
        hdr_parallel_pixels(pixels,jobs,[&](size_t begin,size_t end,unsigned job){
            float local=pq?0.f:1.f;
            for(size_t i=begin;i<end;++i)for(unsigned c=0;c<3;++c)
                if(std::isfinite(rgba[4*i+c]))local=std::max(local,rgba[4*i+c]);
            peaks[job]=local;
        });
        found=*std::max_element(peaks.begin(),peaks.end());
        if(pq)pq_intensity=std::clamp(found*kSdrWhiteNits,1.f,kMaxNits);
        else peak=std::max(found,400.f/kSdrWhiteNits);
    }
    std::vector<float> encoded(pixels*4);
    hdr_parallel_pixels(pixels,hdr_pixel_jobs(pixels),[&](size_t begin,size_t end,unsigned){
        for(size_t i=begin;i<end;++i) {
            for(unsigned c=0;c<3;++c) {
                float v=rgba[4*i+c];if(!std::isfinite(v))v=0.f;
                if(pq||!hdr)v=std::clamp(v,0.f,peak);
                encoded[4*i+c]=pq?linear_to_pq_code(v):v/peak;
            }
            float alpha=rgba[4*i+3];if(!std::isfinite(alpha))alpha=0.f;
            encoded[4*i+3]=std::clamp(alpha,0.f,1.f);
        }
    });
    const float intensity=pq?pq_intensity:hdr?peak*kSdrWhiteNits:kSdrWhiteNits;
    if(pq || (hdr && quality.jxl_half)) {
        std::vector<uint16_t> packed(encoded.size());
        std::transform(encoded.begin(),encoded.end(),packed.begin(),[&](float v){
            return pq?static_cast<uint16_t>(std::lround(std::clamp(v,0.f,1.f)*65535.f)):float_to_half(v);
        });
        return encode_jxl_pixels(w,h,packed.data(),cg,path,quality,pq?JXL_TYPE_UINT16:JXL_TYPE_FLOAT16,
            pq?JXL_TRANSFER_FUNCTION_PQ:JXL_TRANSFER_FUNCTION_LINEAR,intensity);
    }
    return encode_jxl_pixels(w,h,encoded.data(),cg,path,quality,JXL_TYPE_FLOAT,
        pq?JXL_TRANSFER_FUNCTION_PQ:JXL_TRANSFER_FUNCTION_LINEAR,intensity);
}

int encode_jxl_u8(unsigned w, unsigned h, const uint8_t* rgba, uhdr_color_gamut_t cg,
                   const char* path, StillEncodeQuality quality) {
    return encode_jxl_pixels(w, h, rgba, cg, path, quality, JXL_TYPE_UINT8,
                             JXL_TRANSFER_FUNCTION_SRGB, kSdrWhiteNits);
}

// JPEG transcoding preserves compressed coefficients and reconstruction metadata.
// No RGB conversion or distance setting is involved.
int encode_jxl_source(unsigned w, unsigned h, const void* rgba, bool sixteen,
                      const StillSourceColor& color, const char* path, StillEncodeQuality quality) {
    return encode_jxl_pixels(w, h, rgba, UHDR_CG_BT_709, path, quality,
        color.exponent_bits ? (color.bits <= 16 ? JXL_TYPE_FLOAT16 : JXL_TYPE_FLOAT) : sixteen ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8,
        static_cast<JxlTransferFunction>(color.transfer),
        color.intensity_target > 0.f ? color.intensity_target : color.transfer == 16 ? 10000.f : color.transfer == 18 ? 1000.f : kSdrWhiteNits, &color);
}

int transcode_jpeg(const uint8_t* data, size_t size, const char* path, bool keep_exif) {
    auto encoder = JxlEncoderMake(nullptr);
    auto runner = JxlThreadParallelRunnerMake(nullptr, jxrc_limit_threads(std::min(4u,std::max(1u,std::thread::hardware_concurrency()))));
    if (!encoder || !runner || JxlEncoderSetParallelRunner(encoder.get(),JxlThreadParallelRunner,runner.get())!=JXL_ENC_SUCCESS ||
        JxlEncoderStoreJPEGMetadata(encoder.get(),keep_exif?JXL_TRUE:JXL_FALSE)!=JXL_ENC_SUCCESS) return -61;
    auto* frame=JxlEncoderFrameSettingsCreate(encoder.get(),nullptr);
    if (!frame || JxlEncoderFrameSettingsSetOption(frame,JXL_ENC_FRAME_SETTING_JPEG_KEEP_EXIF,keep_exif?1:0)!=JXL_ENC_SUCCESS ||
        JxlEncoderAddJPEGFrame(frame,data,size)!=JXL_ENC_SUCCESS) return -68;
    JxlEncoderCloseInput(encoder.get());
    return write_jxl_output(encoder.get(),path);
}
