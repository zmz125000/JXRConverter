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

namespace {
struct AnimationSampleAxis {
    double left, right;
    unsigned first, last;
    bool interpolate;

    double weight(unsigned i) const {
        if(interpolate)return i==first?1.-(left-first):left-first;
        return std::min(right,double(i)+1.)-std::max(left,double(i));
    }
};

AnimationSampleAxis animation_sample_axis(double pixel,double step,unsigned extent) {
    if(step<=1.) {
        const double center=std::clamp((pixel+.5)*step-.5,0.,double(extent-1));
        const unsigned first=unsigned(center);
        return {center,center,first,std::min(first+1,extent-1),true};
    }
    const double left=pixel*step;
    const double right=std::min(double(extent),(pixel+1.)*step);
    return {left,right,std::min(unsigned(left),extent-1),std::min(unsigned(std::ceil(right))-1,extent-1),false};
}

std::array<float,4> sample_animation_frame(const AnimationFrame& frame,
                                          const AnimationSampleAxis& x,const AnimationSampleAxis& y) {
    // Interpolate enlargements and area-filter reductions in premultiplied linear light.
    double r=0,g=0,b=0,alpha=0,area=0;
    for(unsigned row=y.first;row<=y.last;++row) {
        const double wy=y.weight(row);
        for(unsigned col=x.first;col<=x.last;++col) {
            const double weight=wy*x.weight(col);
            const auto* p=frame.rgba.data()+(size_t(row)*frame.width+col)*4;
            const double coverage=weight*unit(p[3]);
            if(coverage>0) {
                if(std::isfinite(p[0]))r+=coverage*p[0];
                if(std::isfinite(p[1]))g+=coverage*p[1];
                if(std::isfinite(p[2]))b+=coverage*p[2];
                alpha+=coverage;
            }
            area+=weight;
        }
    }
    if(alpha<=0||area<=0)return {};
    return {float(r/alpha),float(g/alpha),float(b/alpha),float(alpha/area)};
}

double animation_offset(unsigned canvas,double frame,unsigned align) {
    const double gap=double(canvas)-frame;
    return std::copysign(std::floor(std::abs(gap)*align/2.),gap);
}

bool animation_flattens_alpha(const AnimationEncoding& format,const Options& o) {
    return format.video||o.transparency_background==TransparencyBackground::black_remove_alpha||
           o.transparency_background==TransparencyBackground::white_remove_alpha;
}

bool animation_white_background(const AnimationEncoding& format,const Options& o) {
    return o.transparency_background==TransparencyBackground::white_remove_alpha||
           (format.video&&o.transparency_background==TransparencyBackground::white);
}
}

std::vector<float> compose_animation_frame(AnimationFrame&& frame, const AnimationEncoding& format,
                                            const AnimationPlacement& placement,const Options& o,
                                            std::vector<float>& stack) {
    require(fits_working_image(format.width,format.height)&&fits_working_image(frame.width,frame.height),
            "Invalid animation frame or canvas size.");
    const double fit=std::min(double(format.width)/frame.width,double(format.height)/frame.height);
    double scale=fit;
    switch(placement.resize) {
    case AnimationResize::off:
        if(fit<1.) {
            const unsigned edge=std::max(1u,unsigned(std::floor(std::max(frame.width,frame.height)*fit)));
            scale_rgba_f32_max_edge(frame.rgba,frame.width,frame.height,edge);
        }
        scale=1.;
        break;
    case AnimationResize::fit_up:break;
    case AnimationResize::fit_down:scale=std::min(1.,fit);break;
    case AnimationResize::fill_up:
    case AnimationResize::fill_down:
        scale=std::max(double(format.width)/frame.width,double(format.height)/frame.height);
        break;
    }
    const double width=std::max(1.,std::round(frame.width*scale));
    const double height=std::max(1.,std::round(frame.height*scale));
    const double left=animation_offset(format.width,width,placement.horizontal);
    const double top=animation_offset(format.height,height,placement.vertical);
    const bool resample=width!=frame.width||height!=frame.height;
    const auto count=size_t(format.width)*format.height;
    const bool inPlace=!resample&&frame.width==format.width&&frame.height==format.height;
    auto canvas=inPlace?std::move(frame.rgba):std::vector<float>(count*4);
    const bool stacking=placement.stack!=AnimationStack::off;
    const bool updateStack=stacking&&(stack.empty()||placement.stack==AnimationStack::previous);
    if(stacking&&stack.empty())stack.resize(count*4);

    const bool flatten=animation_flattens_alpha(format,o);
    const bool white=animation_white_background(format,o);
    const auto gamut=format.primaries==9?UHDR_CG_BT_2100:UHDR_CG_BT_709;
    const auto fromXyz=skiv::inverse(matrix(format.primaries));
    const auto& toXyz=matrix(frame.primaries);
    const auto channel=[&](float v) {
        if(!std::isfinite(v))return 0.f;
        switch(format.transfer) {
        case AnimationTransfer::pq:return linear_to_pq_code(v);
        case AnimationTransfer::hlg:return unit(v);
        case AnimationTransfer::sdr:return format.video?sdr_video_oetf(v,format.video_gamma):srgb_oetf(v);
        case AnimationTransfer::scrgb:return v*(kSdrWhiteNits/80.f);
        case AnimationTransfer::linear:return v;
        }
        return 0.f;
    };
    const unsigned jobs=std::clamp(o.threads,1u,4u);
    hdr_parallel_pixels(format.height,jobs,[&](size_t begin,size_t end,unsigned) {
        for(size_t row=begin;row<end;++row) {
            const double y=double(row)-top;
            const bool inRow=y>=0&&y<height;
            const auto sy=inRow&&resample?animation_sample_axis(y,frame.height/height,frame.height):AnimationSampleAxis{};
            for(unsigned col=0;col<format.width;++col) {
                const double x=col-left;
                const bool inside=inRow&&x>=0&&x<width;
                if(!inside&&!stacking&&!flatten)continue;
                const size_t i=(row*format.width+col)*4;
                auto* out=canvas.data()+i;
                std::array<float,4> pixel{};
                if(inside) {
                    if(resample)pixel=sample_animation_frame(frame,animation_sample_axis(x,frame.width/width,frame.width),sy);
                    else {
                        const auto* source=inPlace?out:frame.rgba.data()+(size_t(y)*frame.width+size_t(x))*4;
                        std::copy_n(source,4,pixel.begin());
                    }
                }
                double alpha=unit(pixel[3]);
                skiv::RGB rgb={pixel[0],pixel[1],pixel[2]};
                if(inside&&frame.primaries!=format.primaries)rgb=skiv::transform(fromXyz,skiv::transform(toXyz,rgb));
                if(stacking) {
                    auto* background=stack.data()+i;
                    const double uncovered=1.-alpha;
                    for(unsigned c=0;c<3;++c) {
                        const double source=alpha>0&&std::isfinite(rgb[c])?rgb[c]*alpha:0.;
                        rgb[c]=source+background[c]*uncovered;
                    }
                    alpha+=background[3]*uncovered;
                    if(updateStack) {
                        for(unsigned c=0;c<3;++c)background[c]=float(rgb[c]);
                        background[3]=float(alpha);
                    }
                    if(flatten) {
                        if(white)for(auto& c:rgb)c+=1.-alpha;
                    } else if(alpha>0)for(auto& c:rgb)c/=alpha;
                } else if(flatten) {
                    for(auto& c:rgb)c=(alpha>0?c*alpha:0.)+(white?1.-alpha:0.);
                }
                float r=float(rgb[0]),g=float(rgb[1]),b=float(rgb[2]);
                if(format.transfer==AnimationTransfer::hlg)linear_to_hlg_rgb(r,g,b,gamut);
                out[0]=channel(r);out[1]=channel(g);out[2]=channel(b);out[3]=flatten?1.f:float(alpha);
            }
        }
    });
    return canvas;
}

std::vector<uint8_t> animation_rgba8(const std::vector<float>& pixels) {
    std::vector<uint8_t> result(pixels.size());
    std::transform(pixels.begin(),pixels.end(),result.begin(),[](float v){return uint8_t(std::lround(unit(v)*255.f));});
    return result;
}

std::vector<uint8_t> place_animation_rgba8(AnimationFrame&& frame, const AnimationEncoding& format,
                                         const AnimationPlacement& placement,const Options& o) {
    require(fits_working_image(format.width,format.height)&&frame.width<=format.width&&frame.height<=format.height,
            "Invalid animation frame or canvas size.");
    if(frame.width==format.width&&frame.height==format.height)return std::move(frame.rgba8);
    const bool flatten=animation_flattens_alpha(format,o);
    const bool white=animation_white_background(format,o);
    std::vector<uint8_t> canvas(size_t(format.width)*format.height*4,flatten&&white?255:0);
    if(flatten&&!white)for(size_t i=3;i<canvas.size();i+=4)canvas[i]=255;
    const auto left=unsigned(animation_offset(format.width,frame.width,placement.horizontal));
    const auto top=unsigned(animation_offset(format.height,frame.height,placement.vertical));
    const auto stride=size_t(frame.width)*4;
    for(unsigned row=0;row<frame.height;++row)
        std::copy_n(frame.rgba8.data()+row*stride,stride,canvas.data()+(size_t(top+row)*format.width+left)*4);
    return canvas;
}

std::vector<uint16_t> animation_video_rgba16(const std::vector<uint8_t>& pixels,SdrVideoGamma gamma) {
    // The direct raster path receives sRGB. Convert its codes directly to
    // the video transfer without allocating a floating-point frame.
    std::array<uint16_t,256> table{};
    for(unsigned i=0;i<table.size();++i)
        table[i]=gamma==SdrVideoGamma::srgb?uint16_t(i*257):
            uint16_t(std::lround(sdr_video_oetf(srgb_eotf(i/255.f),gamma)*65535.f));
    std::vector<uint16_t> result(pixels.size());
    for(size_t i=0;i<pixels.size();i+=4) {
        for(unsigned c=0;c<3;++c)result[i+c]=table[pixels[i+c]];
        result[i+3]=uint16_t(pixels[i+3])*257;
    }
    return result;
}

void clear_gif_canvas_between_frames(const std::filesystem::path& path,const std::function<bool()>& cancelled) {
    std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);
    const auto check=[&]{require(bool(file),"Cannot update GIF frame disposal.");};
    const auto byte=[&] {
        const int value=file.get();
        check();
        return unsigned(value);
    };
    const auto skipBlocks=[&] {
        for(unsigned size=byte();size;size=byte())file.ignore(size);
    };
    const auto paletteSize=[](unsigned packed){return 3u<<(1+(packed&7));};
    std::array<unsigned char,13> header{};
    file.read(reinterpret_cast<char*>(header.data()),header.size());
    check();
    require(header[0]=='G'&&header[1]=='I'&&header[2]=='F'&&(header[10]&128),"Invalid GIF animation header.");
    file.ignore(paletteSize(header[10]));
    for(unsigned block=byte();block!=0x3b;block=byte()) {
        if(cancelled())throw std::runtime_error("Animation conversion cancelled");
        if(block==0x21) {
            if(byte()!=0xf9){skipBlocks();continue;}
            require(byte()==4,"Invalid GIF frame control block.");
            const auto offset=file.tellg();
            std::array<char,4> control{};
            file.read(control.data(),control.size());
            check();
            // The palette filter reserves a transparent index even for opaque
            // frames. FFmpeg can omit its flag on frames using the global palette.
            if(!(control[0]&1))control[3]=char(header[11]);
            control[0]=char((control[0]&~0x1c)|0x09); // Restore to transparent background.
            file.seekp(offset);
            file.write(control.data(),control.size());
            file.seekg(offset+std::streamoff(control.size()));
            require(byte()==0,"Invalid GIF frame control terminator.");
        } else if(block==0x2c) {
            file.ignore(8); // Frame position and dimensions.
            const auto packed=byte();
            if(packed&128)file.ignore(paletteSize(packed));
            file.ignore(1); // LZW code size.
            skipBlocks();
        } else throw std::runtime_error("Invalid GIF animation block.");
    }
    file.flush();
    check();
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
            add(animation_rgba8(pixels));
            return;
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

    void add(const std::vector<uint8_t>& rgba) {
        require(bool(webp),"Byte frames require WebP animation output.");
        WebPConfig settings{};
        require(WebPConfigInit(&settings)!=0,"Cannot initialize WebP compression.");
        settings.lossless=quality.webp()==100;
        settings.quality=settings.lossless?75.f:float(quality.webp());
        WebPPixels frame;
        frame.picture.use_argb=settings.lossless;
        frame.picture.width=int(format.width);
        frame.picture.height=int(format.height);
        require(WebPPictureImportRGBA(&frame.picture,rgba.data(),int(format.width)*4)!=0,"Cannot import WebP frame.");
        if(!WebPAnimEncoderAdd(webp.get(),&frame.picture,webpTime(),&settings))
            throw std::runtime_error(WebPAnimEncoderGetError(webp.get()));
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
            if(format.primaries==9) {
                // WebP carries wide-gamut SDR in its container ICC chunk.
                auto profile=ultrahdr::IccHelper::writeIccProfile(UHDR_CT_SRGB,UHDR_CG_BT_2100);
                const size_t prefix=ultrahdr::kICCIdentifierSize;
                require(profile&&profile->getLength()>prefix,"Cannot create WebP animation color profile.");
                const WebPData icc{static_cast<const uint8_t*>(profile->getData())+prefix,profile->getLength()-prefix};
                const std::unique_ptr<WebPMux,decltype(&WebPMuxDelete)> mux(WebPMuxCreate(&result.data,0),WebPMuxDelete);
                WebPBytes tagged;
                require(mux&&WebPMuxSetChunk(mux.get(),"ICCP",&icc,0)==WEBP_MUX_OK&&
                        WebPMuxAssemble(mux.get(),&tagged.data)==WEBP_MUX_OK,"Cannot store WebP animation color profile.");
                write(file,tagged.data.bytes,tagged.data.size);
            } else write(file,result.data.bytes,result.data.size);
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
void AnimationEncoder::add(const std::vector<uint8_t>& frame) {impl_->add(frame);}
void AnimationEncoder::finish(const std::function<bool()>& cancelled) {impl_->finish(cancelled);}

}
