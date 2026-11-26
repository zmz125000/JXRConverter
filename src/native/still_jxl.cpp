#include "output_file.h"
#include "image_size.h"
// Derived from LocalViewer; see NOTICE and COPYING. Android entry points removed.
#include "hdr_encode.h"
#include "hdr_parallel.h"
#include "render_analysis.h"


#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <type_traits>
#include <jxl/encode_cxx.h>
#include <jxl/gain_map.h>
#include <ultrahdr/gainmapmetadata.h>
#include <avif/avif.h>
#include <jxl/thread_parallel_runner_cxx.h>

namespace {
struct JxlFileCloser {
    void operator()(FILE* file) const { fclose(file); }
};
}

static int write_jxl_output(JxlEncoder* encoder, const char* path, std::vector<uint8_t>* codestream = nullptr) {
    std::unique_ptr<FILE, JxlFileCloser> file(path ? open_output_file(path) : nullptr);
    if (!file && !codestream) return -65;
    // Stream encoded bytes directly to disk; no second complete compressed file.
    std::array<uint8_t, 65536> output{};
    for (;;) {
        uint8_t* next = output.data();
        size_t available = output.size();
        const auto status = JxlEncoderProcessOutput(encoder, &next, &available);
        const size_t count = output.size() - available;
        if (codestream) codestream->insert(codestream->end(), output.data(), next);
        else if (count && fwrite(output.data(), 1, count, file.get()) != count) return -66;
        if (status == JXL_ENC_SUCCESS) break;
        if (status != JXL_ENC_NEED_MORE_OUTPUT) return -67;
    }
    return !file || fflush(file.get()) == 0 ? 0 : -66;
}

static int encode_jxl_pixels(unsigned w, unsigned h, const void* rgba, uhdr_color_gamut_t cg,
                             const char* path, StillEncodeQuality quality, JxlDataType type,
                             JxlTransferFunction transfer, float intensity, const StillSourceColor* source = nullptr,
                             const std::vector<uint8_t>* gain_map_box = nullptr,
                             std::vector<uint8_t>* codestream = nullptr) {
    const size_t pixels = static_cast<size_t>(w) * h;
    if (!rgba || (!path && !codestream) || !pixels) return -60;
    const unsigned input_channels = source ? source->channels : 4;
    const bool cmyk = source && source->cmyk;
    const bool alpha = !cmyk && !quality.remove_alpha && (input_channels == 2 || input_channels == 4);
    const bool gray = input_channels <= 2 || (source && source->gray) || (source && source->icc_size >= 20 && !std::memcmp(source->icc + 16, "GRAY", 4));
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
    info.num_extra_channels = alpha || cmyk ? 1 : 0;
    info.alpha_bits = alpha ? info.bits_per_sample : 0;
    info.alpha_exponent_bits = alpha ? info.exponent_bits_per_sample : 0;
    info.alpha_premultiplied = JXL_FALSE;
    const float distance = quality.jxl_distance();
    const bool compression = distance > 0.f;
    // Preserve source ICC bytes rather than allowing libjxl to replace a
    // representable profile with structured color metadata. Use Modular for
    // original-profile lossy frames; non-XYB VarDCT has known artifact issues.
    // Encodes without a source ICC retain perceptual XYB for lossy presets.
    const bool keep_icc = source && source->icc_size > 0;
    info.uses_original_profile = (!compression || keep_icc) ? JXL_TRUE : JXL_FALSE;
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
    if(cmyk && JxlEncoderSetCodestreamLevel(encoder.get(),10)!=JXL_ENC_SUCCESS)return -62;
    if (JxlEncoderSetBasicInfo(encoder.get(), &info) != JXL_ENC_SUCCESS ||
        (source && source->icc_size ?
            JxlEncoderSetICCProfile(encoder.get(), source->icc, source->icc_size) :
            JxlEncoderSetColorEncoding(encoder.get(), &color)) != JXL_ENC_SUCCESS) return -62;
    if(cmyk) {
        JxlExtraChannelInfo black;JxlEncoderInitExtraChannelInfo(JXL_CHANNEL_BLACK,&black);
        black.bits_per_sample=info.bits_per_sample;black.exponent_bits_per_sample=info.exponent_bits_per_sample;
        if(JxlEncoderSetExtraChannelInfo(encoder.get(),0,&black)!=JXL_ENC_SUCCESS)return -62;
    }
    auto* frame = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
    // Lossless and ICC-preserving lossy use original-profile / modular coding.
    // High/mid/low without an ICC use perceptual XYB at distances 0.5/1.0/1.5,
    // preserving primaries, transfer and HDR intensity target. Alpha remains
    // lossless in every preset.
    if (!frame || JxlEncoderSetFrameLossless(frame, compression ? JXL_FALSE : JXL_TRUE) != JXL_ENC_SUCCESS ||
        JxlEncoderSetFrameDistance(frame, distance) != JXL_ENC_SUCCESS ||
        (compression && keep_icc && JxlEncoderFrameSettingsSetOption(frame, JXL_ENC_FRAME_SETTING_MODULAR, 1) != JXL_ENC_SUCCESS) ||
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
    if (input_channels != output_channels) {
        gray_alpha.resize(pixels * sample_bytes * output_channels);
        for (size_t i=0;i<pixels;++i) {
            std::memcpy(gray_alpha.data()+i*sample_bytes*output_channels, static_cast<const uint8_t*>(rgba)+i*sample_bytes*input_channels, sample_bytes*(gray?1:3));
            if (alpha) std::memcpy(gray_alpha.data()+(i*output_channels+1)*sample_bytes,
                static_cast<const uint8_t*>(rgba)+(i*input_channels+input_channels-1)*sample_bytes, sample_bytes);
        }
        input = gray_alpha.data();
    }
    if (quality.exif_size || gain_map_box) {
        if (JxlEncoderUseContainer(encoder.get(), JXL_TRUE) != JXL_ENC_SUCCESS ||
            JxlEncoderUseBoxes(encoder.get()) != JXL_ENC_SUCCESS) return -62;
        if (quality.exif_size) {
            std::vector<uint8_t> box(4 + quality.exif_size, 0);
            std::copy(quality.exif, quality.exif + quality.exif_size, box.begin() + 4);
            if (JxlEncoderAddBox(encoder.get(), "Exif", box.data(), box.size(), JXL_FALSE) != JXL_ENC_SUCCESS) return -62;
        }
        if (gain_map_box &&
            JxlEncoderAddBox(encoder.get(), "jhgm", gain_map_box->data(), gain_map_box->size(), JXL_FALSE) != JXL_ENC_SUCCESS) return -62;
        JxlEncoderCloseBoxes(encoder.get());
    }
    if (JxlEncoderAddImageFrame(frame, &format, input, pixels * sample_bytes * output_channels) != JXL_ENC_SUCCESS) return -64;
    if(cmyk) {
        std::vector<uint8_t> black(pixels*sample_bytes);
        for(size_t i=0;i<pixels;++i)std::memcpy(black.data()+i*sample_bytes,static_cast<const uint8_t*>(rgba)+(i*4+3)*sample_bytes,sample_bytes);
        const JxlPixelFormat black_format{1,type,JXL_NATIVE_ENDIAN,0};
        if(JxlEncoderSetExtraChannelBuffer(frame,&black_format,black.data(),black.size(),0)!=JXL_ENC_SUCCESS)return -64;
    }
    JxlEncoderCloseInput(encoder.get());
    return write_jxl_output(encoder.get(), path, codestream);
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
                   bool hdr,const char* path,StillEncodeQuality quality,bool pq,bool hlg) {
    const size_t pixels=static_cast<size_t>(w)*h;
    if(!rgba||!path||!w||!h||pixels>SIZE_MAX/16||(pq&&hlg))return -60;
    if(hlg) {
        std::vector<uint16_t> encoded(pixels*4);
        hdr_parallel_pixels(pixels,hdr_pixel_jobs(pixels),[&](size_t begin,size_t end,unsigned){
            for(size_t i=begin;i<end;++i) {
                float r=rgba[i*4],g=rgba[i*4+1],b=rgba[i*4+2];
                linear_to_hlg_rgb(r,g,b,cg);
                encoded[i*4]=uint16_t(std::lround(r*65535.f));
                encoded[i*4+1]=uint16_t(std::lround(g*65535.f));
                encoded[i*4+2]=uint16_t(std::lround(b*65535.f));
                const float a=std::isfinite(rgba[i*4+3])?std::clamp(rgba[i*4+3],0.f,1.f):0.f;
                encoded[i*4+3]=uint16_t(std::lround(a*65535.f));
            }
        });
        return encode_jxl_pixels(w,h,encoded.data(),cg,path,quality,JXL_TYPE_UINT16,JXL_TRANSFER_FUNCTION_HLG,1000.f);
    }
    if(!hdr&&!pq)return encode_jxl_sdr_f32(w,h,rgba,cg,path,quality);
    auto* analysis=quality.analysis&&quality.analysis->matches(rgba,pixels,cg)?quality.analysis:nullptr;
    const bool collect=pq&&analysis&&!analysis->pq_ready();
    std::array<PqAnalysisAccumulator,8> statistics{};
    float peak=pq?kMaxLinear:1.f;
    float pq_intensity=1.f;
    if((hdr||pq)&&!collect) {
        float found=1.f;
        if(analysis)found=analysis->peak();
        else {
            const unsigned jobs=hdr_pixel_jobs(pixels);
            std::array<float,8> peaks{};
            hdr_parallel_pixels(pixels,jobs,[&](size_t begin,size_t end,unsigned job){
                float local=pq?0.f:1.f;
                for(size_t i=begin;i<end;++i)for(unsigned c=0;c<3;++c)
                    if(std::isfinite(rgba[4*i+c]))local=std::max(local,rgba[4*i+c]);
                peaks[job]=local;
            });
            found=*std::max_element(peaks.begin(),peaks.end());
        }
        if(pq)pq_intensity=std::clamp(found*kSdrWhiteNits,1.f,kMaxNits);
        else peak=std::max(found,400.f/kSdrWhiteNits);
    }
    std::vector<float> encoded(pixels*4);
    hdr_parallel_pixels(pixels,hdr_pixel_jobs(pixels),[&](size_t begin,size_t end,unsigned job){
        for(size_t i=begin;i<end;++i) {
            float raw_peak=0.f,code_peak=0.f;
            for(unsigned c=0;c<3;++c) {
                float v=rgba[4*i+c];if(!std::isfinite(v))v=0.f;
                if(collect)raw_peak=std::max(raw_peak,v);
                if(pq||!hdr)v=std::clamp(v,0.f,peak);
                encoded[4*i+c]=pq?linear_to_pq_code(v):v/peak;
                if(collect)code_peak=std::max(code_peak,encoded[4*i+c]);
            }
            if(collect)statistics[job].observe(raw_peak,code_peak);
            float alpha=rgba[4*i+3];if(!std::isfinite(alpha))alpha=0.f;
            encoded[4*i+3]=std::clamp(alpha,0.f,1.f);
        }
    });
    if(collect) {
        analysis->record_pq(statistics);
        pq_intensity=std::clamp(analysis->peak()*kSdrWhiteNits,1.f,kMaxNits);
    }
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

namespace {
constexpr float kJxlGainOffset = 1.f / 64.f;

struct GainMapRange {
    std::array<float, 3> min{};
    std::array<float, 3> max{};
    float peak = 1.f;
};

static float bounded_linear(float value, float maximum) {
    return std::isfinite(value) ? std::clamp(value, 0.f, maximum) : 0.f;
}

template<class Sample>
int encode_jxl_with_gain_map(unsigned w, unsigned h, const float* rgba, uhdr_color_gamut_t gamut,
                            const char* path, StillEncodeQuality quality, const float* sdr_base) {
    constexpr float sample_max = std::numeric_limits<Sample>::max();
    constexpr auto base_type = sizeof(Sample) == 1 ? JXL_TYPE_UINT8 : JXL_TYPE_UINT16;
    const size_t pixels = size_t(w) * h;
    const unsigned jobs = hdr_pixel_jobs(pixels);
    std::vector<Sample> base(pixels * 4);
    std::array<GainMapRange, 8> ranges{};
    // Compute gain against the actual quantized SDR base in the same primaries.
    const auto log_gain = [&](size_t i, unsigned c) {
        const float alt = bounded_linear(rgba[i*4+c], kMaxLinear);
        const float sdr = srgb_eotf(base[i*4+c] / sample_max);
        return std::log2((alt + kJxlGainOffset) / (sdr + kJxlGainOffset));
    };
    hdr_parallel_pixels(pixels, jobs, [&](size_t begin, size_t end, unsigned job) {
        auto& range = ranges[job];
        for (size_t i = begin; i < end; ++i) {
            const float alpha = bounded_linear(rgba[i*4+3], 1.f);
            base[i*4+3] = static_cast<Sample>(std::lround(alpha * sample_max));
            for (unsigned c = 0; c < 3; ++c) {
                const float value = (sdr_base ? sdr_base : rgba)[i*4+c];
                base[i*4+c] = static_cast<Sample>(std::lround(srgb_oetf(value) * sample_max));
                if (!base[i*4+3]) continue;
                const float gain = log_gain(i, c);
                range.min[c] = std::min(range.min[c], gain);
                range.max[c] = std::max(range.max[c], gain);
                range.peak = std::max(range.peak, bounded_linear(rgba[i*4+c], kMaxLinear));
            }
        }
    });
    auto range = ranges[0];
    for (unsigned job = 1; job < jobs; ++job) {
        for (unsigned c = 0; c < 3; ++c) {
            range.min[c] = std::min(range.min[c], ranges[job].min[c]);
            range.max[c] = std::max(range.max[c], ranges[job].max[c]);
        }
        range.peak = std::max(range.peak, ranges[job].peak);
    }
    // Round the endpoints outwards to the same fractions stored in metadata.
    constexpr uint32_t denominator = 65536;
    ultrahdr::uhdr_gainmap_metadata_frac metadata{};
    metadata.useBaseColorSpace = true;
    metadata.baseHdrHeadroomD = 1;
    metadata.alternateHdrHeadroomN = static_cast<uint32_t>(std::lround(std::log2(range.peak) * denominator));
    metadata.alternateHdrHeadroomD = denominator;
    for (unsigned c = 0; c < 3; ++c) {
        metadata.gainMapMinN[c] = static_cast<int32_t>(std::floor(range.min[c] * denominator));
        metadata.gainMapMaxN[c] = static_cast<int32_t>(std::ceil(range.max[c] * denominator));
        metadata.gainMapMinD[c] = metadata.gainMapMaxD[c] = denominator;
        metadata.gainMapGammaN[c] = metadata.gainMapGammaD[c] = 1;
        metadata.baseOffsetN[c] = metadata.alternateOffsetN[c] = 1;
        metadata.baseOffsetD[c] = metadata.alternateOffsetD[c] = 64;
        range.min[c] = float(metadata.gainMapMinN[c]) / denominator;
        range.max[c] = float(metadata.gainMapMaxN[c]) / denominator;
    }
    std::vector<uint8_t> metadata_bytes;
    if (metadata.encodeGainmapMetadata(&metadata, metadata_bytes).error_code != UHDR_CODEC_OK) return -68;

    // The nested codestream must also meet level 5's dimension and area bounds.
    unsigned scale = quality.jxl_gain_map_scale();
    unsigned map_w = 1 + (w - 1) / scale;
    unsigned map_h = 1 + (h - 1) / scale;
    while (map_w > (1u << 18) || map_h > (1u << 18) || uint64_t(map_w) * map_h > (1u << 28)) {
        scale *= 2;
        map_w = 1 + (w - 1) / scale;
        map_h = 1 + (h - 1) / scale;
    }
    const size_t map_pixels = size_t(map_w) * map_h;
    std::vector<uint16_t> gain(map_pixels * 3);
    hdr_parallel_pixels(map_pixels, hdr_pixel_jobs(map_pixels), [&](size_t begin, size_t end, unsigned) {
        for (size_t i = begin; i < end; ++i) {
            const unsigned x = unsigned(i % map_w) * scale;
            const unsigned y = unsigned(i / map_w) * scale;
            std::array<float, 3> sum{};
            unsigned count = 0;
            for (unsigned row = y; row - y < std::min(scale, h - y); ++row) {
                for (unsigned col = x; col - x < std::min(scale, w - x); ++col) {
                    const size_t pixel = size_t(row) * w + col;
                    if (!base[pixel*4+3]) continue;
                    for (unsigned c = 0; c < 3; ++c) sum[c] += log_gain(pixel, c);
                    ++count;
                }
            }
            for (unsigned c = 0; c < 3; ++c) {
                const float avg = count ? sum[c] / count : 0.f;
                const float span = range.max[c] - range.min[c];
                const float code = span > 0.f ? std::clamp((avg - range.min[c]) / span, 0.f, 1.f) : 0.f;
                gain[i*3+c] = static_cast<uint16_t>(std::lround(code * 65535.f));
            }
        }
    });
    StillSourceColor gain_color;
    gain_color.channels = 3;
    // Lossless Modular above 12 bits requires a level-10 container, whereas
    // jhgm carries a bare JPEG XL codestream. libjxl scales the 16-bit input.
    gain_color.bits = 12;
    gain_color.transfer = JXL_TRANSFER_FUNCTION_LINEAR;
    std::vector<uint8_t> codestream;
    const int rc = encode_jxl_pixels(map_w, map_h, gain.data(), UHDR_CG_BT_709, nullptr, StillEncodeQuality(0),
        JXL_TYPE_UINT16, JXL_TRANSFER_FUNCTION_LINEAR, kSdrWhiteNits, &gain_color, nullptr, &codestream);
    if (rc != 0) return rc;
    std::vector<uint16_t>().swap(gain);
    if (codestream.size() < 2 || codestream[0] != 0xff || codestream[1] != 0x0a ||
        codestream.size() > UINT32_MAX) return -68;

    JxlGainMapBundle bundle{};
    bundle.gain_map_metadata_size = static_cast<uint16_t>(metadata_bytes.size());
    bundle.gain_map_metadata = metadata_bytes.data();
    // libjxl 0.12.0 calls memcpy even for an absent, zero-length alternate ICC.
    bundle.alt_icc = metadata_bytes.data();
    bundle.has_color_encoding = JXL_TRUE;
    bundle.color_encoding.color_space = JXL_COLOR_SPACE_RGB;
    bundle.color_encoding.white_point = JXL_WHITE_POINT_D65;
    bundle.color_encoding.primaries = gamut == UHDR_CG_BT_2100 ? JXL_PRIMARIES_2100 :
        gamut == UHDR_CG_DISPLAY_P3 ? JXL_PRIMARIES_P3 : JXL_PRIMARIES_SRGB;
    bundle.color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_PQ;
    bundle.color_encoding.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
    bundle.gain_map_size = static_cast<uint32_t>(codestream.size());
    bundle.gain_map = codestream.data();
    size_t bundle_size = 0;
    if (!JxlGainMapGetBundleSize(&bundle, &bundle_size)) return -68;
    std::vector<uint8_t> box(bundle_size);
    if (!JxlGainMapWriteBundle(&bundle, box.data(), box.size(), nullptr)) return -68;
    std::vector<uint8_t>().swap(codestream);

    return encode_jxl_pixels(w, h, base.data(), gamut, path, quality, base_type,
        JXL_TRANSFER_FUNCTION_SRGB, kSdrWhiteNits, nullptr, &box);
}
} // namespace

int encode_jxl_gain_map(unsigned w, unsigned h, const float* rgba, uhdr_color_gamut_t gamut,
                        const char* path, StillEncodeQuality quality, const float* sdr_base) {
    if (!rgba || !path || !fits_working_image(w,h)) return -60;
    if (quality.source_bits > 8) return encode_jxl_with_gain_map<uint16_t>(w, h, rgba, gamut, path, quality, sdr_base);
    return encode_jxl_with_gain_map<uint8_t>(w, h, rgba, gamut, path, quality, sdr_base);
}
