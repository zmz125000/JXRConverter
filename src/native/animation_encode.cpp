#include "animation.h"
#include "codec_api.h"
#include "image_size.h"
#include "render_analysis.h"
#include "skiv_tone_map.h"
#include "ultrahdr/icc.h"
#include <avif/avif.h>
#include <jxl/encode_cxx.h>
#include <jxl/thread_parallel_runner_cxx.h>
#include <webp/encode.h>
#include <webp/mux.h>
#include <zlib.h>
#include <array>
#include <fstream>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace jxrc {
namespace {
void require(bool ok, const char* message) {
    if(!ok)throw std::runtime_error(message);
}
bool floating(AnimationTransfer transfer) {
    return transfer==AnimationTransfer::linear||transfer==AnimationTransfer::scrgb;
}
unsigned cicpTransfer(AnimationTransfer transfer) {
    switch(transfer) {
    case AnimationTransfer::pq:return 16;
    case AnimationTransfer::hlg:return 18;
    case AnimationTransfer::linear:case AnimationTransfer::scrgb:return 8;
    default:return 13;
    }
}
const skiv::Matrix& matrix(unsigned primaries) {
    return primaries==9?skiv::rgb2020_xyz:primaries==12?skiv::rgbp3_xyz:skiv::rgb709_xyz;
}
float unit(float value) {
    return std::isfinite(value)?std::clamp(value,0.f,1.f):0.f;
}
void append16(std::vector<uint8_t>& bytes, unsigned value) {
    bytes.insert(bytes.end(),{uint8_t(value>>8),uint8_t(value)});
}
void append32(std::vector<uint8_t>& bytes, uint32_t value) {
    append16(bytes,value>>16);
    append16(bytes,value);
}
void write(std::ostream& file, const uint8_t* bytes, size_t size) {
    if(size)file.write(reinterpret_cast<const char*>(bytes),std::streamsize(size));
    require(bool(file),"Cannot write animation output.");
}

void chunk(std::ostream& file, const char* name, const uint8_t* bytes, size_t size) {
    std::vector<uint8_t> field;
    append32(field,uint32_t(size));
    write(file,field.data(),field.size());
    write(file,reinterpret_cast<const uint8_t*>(name),4);
    write(file,bytes,size);
    auto crc=crc32(0,reinterpret_cast<const Bytef*>(name),4);
    if(size)crc=crc32(crc,bytes,uInt(size));
    field.clear();
    append32(field,uint32_t(crc));
    write(file,field.data(),field.size());
}

void chunk(std::ostream& file, const char* name, const std::vector<uint8_t>& bytes) {
    chunk(file,name,bytes.data(),bytes.size());
}

struct Deflater {
    z_stream stream{};
    Deflater() {require(deflateInit(&stream,Z_DEFAULT_COMPRESSION)==Z_OK,"Cannot initialize PNG compression.");}
    ~Deflater() {deflateEnd(&stream);}
};

// Stream each frame as an independent PNG datastream. Update the animation
// header with the actual frame count after decoding the last input.
class ApngWriter {
public:
    ApngWriter(std::ostream& file, const AnimationEncoding& format)
        : file_(file), format_(format) {
        constexpr uint8_t signature[]={137,80,78,71,13,10,26,10};
        write(file_,signature,sizeof(signature));
        std::vector<uint8_t> header;
        append32(header,format.width);
        append32(header,format.height);
        header.insert(header.end(),{uint8_t(format.bits),6,0,0,0});
        chunk(file_,"IHDR",header);
        chunk(file_,"cICP",{uint8_t(format.primaries),uint8_t(cicpTransfer(format.transfer)),0,1});
        if(format.transfer==AnimationTransfer::sdr) {
            if(format.primaries==1)chunk(file_,"sRGB",{1});
            else {
                auto profile=ultrahdr::IccHelper::writeIccProfile(UHDR_CT_SRGB,UHDR_CG_BT_2100);
                require(profile&&profile->getLength()>ultrahdr::kICCIdentifierSize,"Cannot create PNG animation color profile.");
                const auto length=uLong(profile->getLength()-ultrahdr::kICCIdentifierSize);
                const auto* data=static_cast<const Bytef*>(profile->getData())+ultrahdr::kICCIdentifierSize;
                std::vector<uint8_t> payload={'B','T','.','2','0','2','0',0,0};
                const auto headerSize=payload.size();
                uLongf compressed=compressBound(length);
                payload.resize(headerSize+compressed);
                require(compress(payload.data()+headerSize,&compressed,data,length)==Z_OK,"Cannot compress PNG animation color profile.");
                payload.resize(headerSize+compressed);
                chunk(file_,"iCCP",payload);
            }
        }
        if(format.transfer==AnimationTransfer::pq||format.transfer==AnimationTransfer::hlg) {
            lightOffset_=file_.tellp();
            chunk(file_,"cLLI",std::vector<uint8_t>(8));
        }
        countOffset_=file_.tellp();
        chunk(file_,"acTL",std::vector<uint8_t>(8));
    }
    void add(const std::vector<uint16_t>& rgba) {
        const unsigned bytesPerPixel=format_.bits/2;
        const size_t rowSize=size_t(format_.width)*bytesPerPixel+1;
        require(rowSize<=std::numeric_limits<uInt>::max(),"Animation PNG row is too wide.");
        std::vector<uint8_t> control;
        append32(control,nextSequence());
        append32(control,format_.width);
        append32(control,format_.height);
        append32(control,0);
        append32(control,0);
        auto numerator=format_.delay_us;
        unsigned denominator=1000000;
        const auto divisor=std::gcd(numerator,denominator);
        numerator/=divisor;
        denominator/=divisor;
        if(numerator>65535||denominator>65535) {
            denominator=std::clamp(unsigned(65535.*1000000./format_.delay_us),1u,10000u);
            numerator=unsigned(std::lround(format_.delay_us/1000000.*denominator));
        }
        append16(control,numerator);
        append16(control,denominator);
        // Replace the entire canvas, including transparent pixels.
        control.insert(control.end(),{0,0});
        chunk(file_,"fcTL",control);

        std::vector<uint8_t> row(rowSize), encoded(65536+(frames_?4:0));
        const auto offset=encoded.size()-65536;
        Deflater compressor;
        for(unsigned y=0;y<format_.height;++y) {
            const auto* source=rgba.data()+size_t(y)*format_.width*4;
            row[0]=1; // Sub filter, applied to the big-endian sample bytes.
            for(size_t i=1;i<rowSize;++i) {
                const auto byte=[&](size_t index) {
                    if(format_.bits==8)return uint8_t((source[index]+128u)/257u);
                    return uint8_t(source[index/2]>>(index%2==0?8:0));
                };
                row[i]=uint8_t(byte(i-1)-(i>bytesPerPixel?byte(i-1-bytesPerPixel):0));
            }
            compressor.stream.next_in=row.data();
            compressor.stream.avail_in=uInt(row.size());
            const int flush=y==format_.height-1?Z_FINISH:Z_NO_FLUSH;
            int result;
            do {
                compressor.stream.next_out=encoded.data()+offset;
                compressor.stream.avail_out=65536;
                result=deflate(&compressor.stream,flush);
                require(result==Z_OK||result==Z_STREAM_END,"PNG frame compression failed.");
                const size_t size=65536-compressor.stream.avail_out;
                if(size) {
                    if(frames_) {
                        const auto sequence=nextSequence();
                        for(unsigned byte=0;byte<4;++byte)encoded[byte]=uint8_t(sequence>>(24-byte*8));
                    }
                    chunk(file_,frames_?"fdAT":"IDAT",encoded.data(),offset+size);
                }
            } while(compressor.stream.avail_in||(flush==Z_FINISH&&result!=Z_STREAM_END));
        }
        ++frames_;

        if(lightOffset_!=std::streampos(-1)) {
            std::vector<float> linear;
            double sum=0;
            for(unsigned y=0;y<format_.height;++y) {
                uhdr_color_gamut_t gamut;
                cicp_rgb_to_linear(reinterpret_cast<const uint8_t*>(rgba.data()+size_t(y)*format_.width*4),
                                   format_.width,16,format_.primaries,cicpTransfer(format_.transfer),linear,gamut);
                for(size_t p=0;p<linear.size();p+=4) {
                    const double nits=std::clamp(double(std::max({linear[p],linear[p+1],linear[p+2]}))*kSdrWhiteNits,
                                                 0.,format_.transfer==AnimationTransfer::pq?10000.:1000.);
                    peak_=std::max(peak_,nits);
                    sum+=nits;
                }
            }
            average_=std::max(average_,sum/(double(format_.width)*format_.height));
        }
    }
    void finish() {
        chunk(file_,"IEND",{});
        if(lightOffset_!=std::streampos(-1)) {
            std::vector<uint8_t> light;
            append32(light,uint32_t(std::llround(peak_*10000.)));
            append32(light,uint32_t(std::llround(average_*10000.)));
            file_.seekp(lightOffset_);
            chunk(file_,"cLLI",light);
        }
        file_.seekp(countOffset_);
        std::vector<uint8_t> count;
        append32(count,frames_);
        append32(count,0);
        chunk(file_,"acTL",count);
    }
private:
    uint32_t nextSequence() {
        require(sequence_<std::numeric_limits<uint32_t>::max(),"PNG animation has too many frame chunks.");
        return sequence_++;
    }
    std::ostream& file_;
    AnimationEncoding format_;
    std::streampos countOffset_{}, lightOffset_{-1};
    uint32_t frames_=0, sequence_=0;
    double peak_=0, average_=0;
};

void avifCheck(avifResult result) {
    if(result!=AVIF_RESULT_OK)throw std::runtime_error(std::string("AVIF animation: ")+avifResultToString(result));
}
void jxlCheck(JxlEncoderStatus result) {
    require(result==JXL_ENC_SUCCESS,"JPEG XL animation encoding failed.");
}

struct WebPPixels {
    WebPPicture picture{};
    WebPPixels() {require(WebPPictureInit(&picture)!=0,"Cannot initialize WebP frame.");}
    ~WebPPixels() {WebPPictureFree(&picture);}
};
struct WebPBytes {
    WebPData data{};
    ~WebPBytes() {WebPDataClear(&data);}
};
struct AvifBytes {
    avifRWData data=AVIF_DATA_EMPTY;
    ~AvifBytes() {avifRWDataFree(&data);}
};
}

std::vector<uint16_t> animation_rgba16(const std::vector<float>& pixels) {
    std::vector<uint16_t> result(pixels.size());
    std::transform(pixels.begin(),pixels.end(),result.begin(),[](float v){return uint16_t(std::lround(unit(v)*65535.f));});
    return result;
}

std::vector<float> compose_animation_frame(AnimationFrame&& frame, const AnimationEncoding& format,
                                            unsigned horizontal, unsigned vertical, const Options& o) {
    require(fits_working_image(format.width,format.height),"Invalid animation canvas size.");
    if(frame.width>format.width||frame.height>format.height) {
        const double scale=std::min(double(format.width)/frame.width,double(format.height)/frame.height);
        const unsigned edge=std::max(1u,unsigned(std::floor(std::max(frame.width,frame.height)*scale)));
        scale_rgba_f32_max_edge(frame.rgba,frame.width,frame.height,edge);
    }
    require(frame.width<=format.width&&frame.height<=format.height,"Frame does not fit the animation canvas.");
    const unsigned x=(format.width-frame.width)*horizontal/2;
    const unsigned y=(format.height-frame.height)*vertical/2;
    const bool flatten=format.video||o.transparency_background==TransparencyBackground::black_remove_alpha||
                       o.transparency_background==TransparencyBackground::white_remove_alpha;
    const bool white=o.transparency_background==TransparencyBackground::white_remove_alpha||
                     (format.video&&o.transparency_background==TransparencyBackground::white);
    const auto gamut=format.primaries==9?UHDR_CG_BT_2100:UHDR_CG_BT_709;
    const auto fromXyz=skiv::inverse(matrix(format.primaries));
    const auto& toXyz=matrix(frame.primaries);
    const auto encode=[&](float r,float g,float b,float a) {
        if(format.transfer==AnimationTransfer::hlg)linear_to_hlg_rgb(r,g,b,gamut);
        const auto channel=[&](float v) {
            if(!std::isfinite(v))return 0.f;
            switch(format.transfer) {
            case AnimationTransfer::pq:return linear_to_pq_code(v);
            case AnimationTransfer::hlg:return unit(v);
            case AnimationTransfer::sdr:return format.video?bt709_oetf(v):srgb_oetf(v);
            case AnimationTransfer::scrgb:return v*(kSdrWhiteNits/80.f);
            case AnimationTransfer::linear:return v;
            }
            return 0.f;
        };
        return std::array<float,4>{channel(r),channel(g),channel(b),a};
    };
    const bool inPlace=frame.width==format.width&&frame.height==format.height;
    auto canvas=inPlace?std::move(frame.rgba):std::vector<float>(size_t(format.width)*format.height*4);
    if(!inPlace&&flatten) {
        const float fill=white?1.f:0.f;
        const auto background=encode(fill,fill,fill,1);
        for(size_t i=0;i<canvas.size();i+=4)std::copy(background.begin(),background.end(),canvas.data()+i);
    }
    const unsigned jobs=std::clamp(o.threads,1u,4u);
    hdr_parallel_pixels(size_t(frame.width)*frame.height,jobs,[&](size_t begin,size_t end,unsigned) {
        for(size_t pixel=begin;pixel<end;++pixel) {
            const auto* p=(inPlace?canvas:frame.rgba).data()+pixel*4;
            const double alpha=unit(p[3]);
            skiv::RGB rgb={p[0],p[1],p[2]};
            if(frame.primaries!=format.primaries)rgb=skiv::transform(fromXyz,skiv::transform(toXyz,rgb));
            if(flatten)for(auto& c:rgb)c=c*alpha+(white?1.-alpha:0.);
            const auto code=encode(float(rgb[0]),float(rgb[1]),float(rgb[2]),flatten?1.f:float(alpha));
            const size_t target=(size_t(y+pixel/frame.width)*format.width+x+pixel%frame.width)*4;
            std::copy(code.begin(),code.end(),canvas.data()+target);
        }
    });
    return canvas;
}

std::vector<uint8_t> animation_rgba8(const std::vector<float>& pixels) {
    std::vector<uint8_t> result(pixels.size());
    std::transform(pixels.begin(),pixels.end(),result.begin(),[](float v){return uint8_t(std::lround(unit(v)*255.f));});
    return result;
}

struct AnimationEncoder::Impl {
    const AnimationEncoding format;
    const StillEncodeQuality quality;
    std::ofstream file;
    std::unique_ptr<ApngWriter> png;
    std::unique_ptr<WebPAnimEncoder,decltype(&WebPAnimEncoderDelete)> webp{nullptr,WebPAnimEncoderDelete};
    std::unique_ptr<avifEncoder,decltype(&avifEncoderDestroy)> avif{nullptr,avifEncoderDestroy};
    std::unique_ptr<avifImage,decltype(&avifImageDestroy)> image{nullptr,avifImageDestroy};
    std::array<uint8_t,65536> jxlBuffer{};
    JxlThreadParallelRunnerPtr runner;
    JxlEncoderPtr jxl;
    unsigned frames=0;

    Impl(const std::filesystem::path& path, const AnimationEncoding& encoding, const Options& o)
        : format(encoding),quality(int(o.quality.preset),o.quality.jxl_distance,o.quality.codec_quality,o.quality.avif_depth),
          file(path,std::ios::binary) {
        require(bool(file),"Cannot open animation output.");
        require(fits_working_image(format.width,format.height),"Invalid animation dimensions.");
        const unsigned threads=std::clamp(o.threads,1u,4u);
        switch(format.format) {
        case AnimationFormat::png:
            png=std::make_unique<ApngWriter>(file,format);
            break;
        case AnimationFormat::webp: {
            require(format.width<=WEBP_MAX_DIMENSION&&format.height<=WEBP_MAX_DIMENSION,"WebP animation exceeds the codec dimensions.");
            WebPAnimEncoderOptions settings{};
            require(WebPAnimEncoderOptionsInit(&settings)!=0,"Cannot initialize WebP animation.");
            webp.reset(WebPAnimEncoderNew(int(format.width),int(format.height),&settings));
            require(bool(webp),"Cannot create WebP animation.");
            break;
        }
        case AnimationFormat::avif:
            avif.reset(avifEncoderCreate());
            image.reset(avifImageCreate(format.width,format.height,format.bits,AVIF_PIXEL_FORMAT_YUV444));
            require(bool(avif)&&bool(image),"Cannot create AVIF animation.");
            avif->codecChoice=AVIF_CODEC_CHOICE_AOM;
            avif->timescale=1000000;
            avif->quality=quality.avif();
            avif->qualityAlpha=AVIF_QUALITY_LOSSLESS;
            avif->maxThreads=int(threads);
            avif->speed=6;
            image->colorPrimaries=avifColorPrimaries(format.primaries);
            image->transferCharacteristics=avifTransferCharacteristics(cicpTransfer(format.transfer));
            image->matrixCoefficients=quality.avif_lossless()?AVIF_MATRIX_COEFFICIENTS_IDENTITY:
                format.primaries==9?AVIF_MATRIX_COEFFICIENTS_BT2020_NCL:AVIF_MATRIX_COEFFICIENTS_BT709;
            image->yuvRange=AVIF_RANGE_FULL;
            if(format.transfer==AnimationTransfer::sdr)
                require(complete_avif_color_metadata(image.get()),"Cannot set AVIF animation color profile.");
            break;
        case AnimationFormat::jxl: {
            const bool isFloat=floating(format.transfer), lossy=quality.jxl_distance()>0.f;
            runner=JxlThreadParallelRunnerMake(nullptr,threads);
            jxl=JxlEncoderMake(nullptr);
            require(bool(jxl)&&bool(runner),"Cannot create JPEG XL animation.");
            jxlCheck(JxlEncoderSetParallelRunner(jxl.get(),JxlThreadParallelRunner,runner.get()));
            JxlEncoderOutputProcessor output{};
            output.opaque=this;
            output.get_buffer=[](void* context,size_t* size)->void* {
                auto& self=*static_cast<Impl*>(context);
                *size=self.file?self.jxlBuffer.size():0;
                return *size?self.jxlBuffer.data():nullptr;
            };
            output.release_buffer=[](void* context,size_t size) {
                auto& self=*static_cast<Impl*>(context);
                self.file.write(reinterpret_cast<const char*>(self.jxlBuffer.data()),std::streamsize(size));
            };
            output.seek=[](void* context,uint64_t position) {
                static_cast<Impl*>(context)->file.seekp(std::streamoff(position));
            };
            output.set_finalized_position=[](void*,uint64_t){};
            jxlCheck(JxlEncoderSetOutputProcessor(jxl.get(),output));
            JxlBasicInfo info;
            JxlEncoderInitBasicInfo(&info);
            info.xsize=format.width;
            info.ysize=format.height;
            info.bits_per_sample=info.alpha_bits=isFloat?32:format.bits;
            info.exponent_bits_per_sample=info.alpha_exponent_bits=isFloat?8:0;
            info.num_extra_channels=1;
            info.uses_original_profile=!lossy;
            info.intensity_target=format.transfer==AnimationTransfer::scrgb?80.f:
                format.transfer==AnimationTransfer::hlg?1000.f:format.transfer==AnimationTransfer::pq?kMaxNits:kSdrWhiteNits;
            info.have_animation=JXL_TRUE;
            info.animation.tps_numerator=1000000;
            info.animation.tps_denominator=1;
            info.animation.num_loops=0;
            if(isFloat||format.width>262144||format.height>262144||uint64_t(format.width)*format.height>268435456u)
                jxlCheck(JxlEncoderSetCodestreamLevel(jxl.get(),10));
            jxlCheck(JxlEncoderSetBasicInfo(jxl.get(),&info));
            JxlColorEncoding color;
            JxlColorEncodingSetToSRGB(&color,JXL_FALSE);
            color.primaries=JxlPrimaries(format.primaries);
            color.transfer_function=JxlTransferFunction(cicpTransfer(format.transfer));
            jxlCheck(JxlEncoderSetColorEncoding(jxl.get(),&color));
            break;
        }
        default:throw std::invalid_argument("Unsupported native animation format.");
        }
    }

    int webpTime() const {
        const uint64_t ms=(uint64_t(frames)*format.delay_us+500)/1000;
        require(ms<=uint64_t(std::numeric_limits<int>::max()),"WebP animation duration is too long.");
        return int(ms);
    }
    void add(const std::vector<float>& pixels) {
        if(png)png->add(animation_rgba16(pixels));
        else if(webp) {
            WebPConfig settings{};
            require(WebPConfigInit(&settings)!=0,"Cannot initialize WebP compression.");
            settings.lossless=quality.webp()==100;
            settings.quality=settings.lossless?75.f:float(quality.webp());
            WebPPixels frame;
            frame.picture.use_argb=settings.lossless;
            frame.picture.width=int(format.width);
            frame.picture.height=int(format.height);
            const auto rgba=animation_rgba8(pixels);
            require(WebPPictureImportRGBA(&frame.picture,rgba.data(),int(format.width)*4)!=0,"Cannot import WebP frame.");
            if(!WebPAnimEncoderAdd(webp.get(),&frame.picture,webpTime(),&settings))
                throw std::runtime_error(WebPAnimEncoderGetError(webp.get()));
        } else if(avif) {
            avifRGBImage rgb;
            avifRGBImageSetDefaults(&rgb,image.get());
            rgb.format=AVIF_RGB_FORMAT_RGBA;
            // libavif accepts full-range RGB16 and scales to the AV1 depth.
            auto words=animation_rgba16(pixels);
            rgb.depth=16;
            rgb.pixels=reinterpret_cast<uint8_t*>(words.data());
            require(format.width<=std::numeric_limits<uint32_t>::max()/8,"AVIF animation row is too wide.");
            rgb.rowBytes=format.width*8;
            avifCheck(avifImageRGBToYUV(image.get(),&rgb));
            avifCheck(avifEncoderAddImage(avif.get(),image.get(),format.delay_us,AVIF_ADD_IMAGE_FLAG_NONE));
        } else {
            if(frames)jxlCheck(JxlEncoderFlushInput(jxl.get()));
            auto* frame=JxlEncoderFrameSettingsCreate(jxl.get(),nullptr);
            require(frame!=nullptr,"Cannot create JPEG XL frame.");
            const float distance=quality.jxl_distance();
            jxlCheck(JxlEncoderSetFrameLossless(frame,distance==0));
            jxlCheck(JxlEncoderSetFrameDistance(frame,distance));
            const bool losslessFloat=distance==0&&floating(format.transfer);
            jxlCheck(JxlEncoderFrameSettingsSetOption(frame,JXL_ENC_FRAME_SETTING_EFFORT,losslessFloat?4:distance==0?3:5));
            if(losslessFloat) {
                // As with still JXL, avoid signed residual overflow across zero.
                jxlCheck(JxlEncoderFrameSettingsSetOption(frame,JXL_ENC_FRAME_SETTING_MODULAR_PREDICTOR,0));
                jxlCheck(JxlEncoderFrameSettingsSetFloatOption(frame,JXL_ENC_FRAME_SETTING_MODULAR_MA_TREE_LEARNING_PERCENT,0.f));
            }
            JxlFrameHeader header;
            JxlEncoderInitFrameHeader(&header);
            header.duration=format.delay_us;
            jxlCheck(JxlEncoderSetFrameHeader(frame,&header));
            if(floating(format.transfer)) {
                const JxlPixelFormat input{4,JXL_TYPE_FLOAT,JXL_NATIVE_ENDIAN,0};
                jxlCheck(JxlEncoderAddImageFrame(frame,&input,pixels.data(),pixels.size()*sizeof(float)));
            } else if(format.bits==8) {
                const auto rgba=animation_rgba8(pixels);
                const JxlPixelFormat input{4,JXL_TYPE_UINT8,JXL_NATIVE_ENDIAN,0};
                jxlCheck(JxlEncoderAddImageFrame(frame,&input,rgba.data(),rgba.size()));
            } else {
                const auto rgba=animation_rgba16(pixels);
                const JxlPixelFormat input{4,JXL_TYPE_UINT16,JXL_NATIVE_ENDIAN,0};
                jxlCheck(JxlEncoderAddImageFrame(frame,&input,rgba.data(),rgba.size()*sizeof(uint16_t)));
            }
        }
        ++frames;
    }

    void finish(const std::function<bool()>& cancelled) {
        require(frames>0,"No animation frames were decoded.");
        require(!cancelled(),"Animation conversion cancelled.");
        if(png)png->finish();
        else if(webp) {
            WebPBytes result;
            require(WebPAnimEncoderAdd(webp.get(),nullptr,webpTime(),nullptr)!=0&&
                    WebPAnimEncoderAssemble(webp.get(),&result.data)!=0,"Cannot finalize WebP animation.");
            write(file,result.data.bytes,result.data.size);
        } else if(avif) {
            AvifBytes result;
            avifCheck(avifEncoderFinish(avif.get(),&result.data));
            write(file,result.data.data,result.data.size);
        } else {
            JxlEncoderCloseInput(jxl.get());
            jxlCheck(JxlEncoderFlushInput(jxl.get()));
        }
        file.close();
        require(bool(file),"Cannot close animation output.");
    }
};

AnimationEncoder::AnimationEncoder(const std::filesystem::path& path, const AnimationEncoding& encoding, const Options& options)
    : impl_(std::make_unique<Impl>(path,encoding,options)) {}
AnimationEncoder::~AnimationEncoder()=default;
void AnimationEncoder::add(const std::vector<float>& frame) {impl_->add(frame);}
void AnimationEncoder::finish(const std::function<bool()>& cancelled) {impl_->finish(cancelled);}

}
