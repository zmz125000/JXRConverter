#include "jxrc/converter.hpp"
#include "jxrc/version.hpp"
#include "reader_pixels.h"
#include "codec_api.h"
#include "working_pixels.h"
#include "input_format.h"
#include "regular_decode.h"
#include "hdr_parallel.h"
#include "linear_color_profile.h"
#include "skcms.h"
#include "ultrahdr/jpegdecoderhelper.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <tuple>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

// Ported RAW packing consults a per-call capture flag for its paired SDR render.
namespace { thread_local bool paired_sdr = false; }
bool export_pixels_keep_sdr() { return paired_sdr; }
bool export_pixels_capture() { return true; }
bool retain_reader_pixels(std::vector<uint8_t>&) { return false; }
bool retain_reader_pixels(std::vector<uint16_t>&) { return false; }
namespace jxrc {
namespace {
void require(bool condition,const std::string& message) { if(!condition) throw std::runtime_error(message); }
void range(double value,double low,double high,const char* name) {
    if(!std::isfinite(value)||value<low||value>high) throw std::invalid_argument(std::string(name)+" out of range");
}
bool linear_jxl(Target t) {return t==Target::jxl_linear||t==Target::jxl_scrgb;}
bool hdr_target(Target t) {return t==Target::jpeg_gainmap||linear_jxl(t)||t==Target::avif_pq||t==Target::avif_gainmap||t==Target::jxl_pq;}
std::vector<uint8_t> read(const std::filesystem::path& path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    require(bool(f),"Cannot open input: "+path.u8string());
    const auto n=f.tellg();require(n>0&&n<=512LL*1024*1024,"Input must be 1 byte to 512 MiB");
    std::vector<uint8_t> bytes(static_cast<size_t>(n));f.seekg(0);
    require(bool(f.read(reinterpret_cast<char*>(bytes.data()),bytes.size())),"Cannot read input");return bytes;
}
struct Staging {
    std::filesystem::path directory, file;
    explicit Staging(const std::filesystem::path& output) {
        static std::atomic<uint64_t> sequence{0};
        auto parent=output.parent_path();if(parent.empty())parent=".";
        for(unsigned i=0;i<100;++i) {
            directory=parent/(".jxrc-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(sequence++));
            if(std::filesystem::create_directory(directory)){file=directory/"output";return;}
        }
        throw std::runtime_error("Cannot create conversion staging directory");
    }
    ~Staging() {std::error_code ec;std::filesystem::remove_all(directory,ec);}
    void publish(const std::filesystem::path& output,bool overwrite) {
        if(!overwrite) {std::filesystem::create_hard_link(file,output);return;}
#ifdef _WIN32
        require(MoveFileExW(file.c_str(),output.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),"Cannot publish output");
#else
        std::filesystem::rename(file,output);
#endif
    }
};
int kind(const std::vector<uint8_t>& b) {
    if(b.size()>2&&b[0]==255&&b[1]==216)return 0;
    if(is_avif_input(b.data(),b.size()))return 1;
    if(b.size()>=2&&((b[0]==255&&b[1]==10)||(b.size()>=12&&!std::memcmp(b.data(),"\0\0\0\x0cJXL \r\n\x87\n",12))))return 3;
    if(b.size()>=4&&b[0]=='I'&&b[1]=='I'&&b[2]==0xbc&&b[3]==1)return 4;
    if(is_regular_image(b.data(),b.size()))return 6;
    return 5; // Sensor formats are validated by LibRaw, never guessed from suffix.
}
template<class Sample>
void jpeg_linear(const std::vector<uint8_t>& bytes,std::vector<Sample>& pixels,unsigned& w,unsigned& h,uhdr_color_gamut_t& gamut,bool& hdr) {
    using Decoder=std::unique_ptr<uhdr_codec_private_t,void(*)(uhdr_codec_private_t*)>;
    Decoder dec(uhdr_create_decoder(),uhdr_release_decoder);
    require(bool(dec),"Cannot allocate JPEG decoder");
    uhdr_compressed_image_t compressed{const_cast<uint8_t*>(bytes.data()),bytes.size(),bytes.size(),UHDR_CG_UNSPECIFIED,UHDR_CT_UNSPECIFIED,UHDR_CR_FULL_RANGE};
        auto status=uhdr_dec_set_out_img_format(dec.get(),UHDR_IMG_FMT_64bppRGBAHalfFloat);
        require(status.error_code==UHDR_CODEC_OK,"Ultra HDR output format failed");
        status=uhdr_dec_set_out_color_transfer(dec.get(),UHDR_CT_LINEAR);
        require(status.error_code==UHDR_CODEC_OK,"Ultra HDR output transfer failed");
    if(uhdr_dec_set_image(dec.get(),&compressed).error_code==UHDR_CODEC_OK&&uhdr_dec_probe(dec.get()).error_code==UHDR_CODEC_OK) {
        require(uhdr_dec_get_image_width(dec.get())>0&&uhdr_dec_get_image_height(dec.get())>0&&uint64_t(uhdr_dec_get_image_width(dec.get()))*uhdr_dec_get_image_height(dec.get())<=80000000,"JPEG dimensions exceed limit");
        status=uhdr_decode(dec.get());
        require(status.error_code==UHDR_CODEC_OK,std::string("Ultra HDR decode failed: ")+(status.has_detail?status.detail:"no codec detail"));
        const auto* image=uhdr_get_decoded_image(dec.get());require(image,"Missing Ultra HDR pixels");
        w=image->w;h=image->h;gamut=image->cg;hdr=true;pixels.resize(size_t(w)*h*4);
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w*4;++x)
            pixels[size_t(y)*w*4+x]=store_working<Sample>(half_to_float(static_cast<const uint16_t*>(image->planes[0])[size_t(y)*image->stride[0]*4+x]));
        return;
    }
    ultrahdr::JpegDecoderHelper jpeg;
    require(jpeg.parseImage(bytes.data(),bytes.size()).error_code==UHDR_CODEC_OK,"JPEG parse failed");
    require(uint64_t(jpeg.getDecompressedImageWidth())*jpeg.getDecompressedImageHeight()<=80000000,"JPEG dimensions exceed limit");
    require(jpeg.decompressImage(bytes.data(),bytes.size(),ultrahdr::DECODE_STREAM).error_code==UHDR_CODEC_OK,"JPEG decode failed");
    const auto image=jpeg.getDecompressedImage();w=image.w;h=image.h;
    require(w&&h,"Empty JPEG");
    const unsigned channels=image.fmt==UHDR_IMG_FMT_8bppYCbCr400?1:image.fmt==UHDR_IMG_FMT_24bppRGB888?3:image.fmt==UHDR_IMG_FMT_32bppRGBA8888?4:0;
    require(channels!=0,"Unsupported JPEG pixel layout");
    skcms_ICCProfile source=*skcms_sRGB_profile(),destination;
    const auto icc=source_jpeg_icc(bytes.data(),bytes.size());
    if(!icc.empty())require(skcms_Parse(icc.data(),icc.size(),&source),"Invalid JPEG ICC profile");
    require(make_linear_bt2020_profile(&destination),"Cannot build working profile");gamut=UHDR_CG_BT_2100;
    std::vector<uint8_t> rgba(size_t(w)*h*4);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
        const auto* p=static_cast<const uint8_t*>(image.planes[0])+size_t(y)*image.stride[0]*channels+x*channels;
        const size_t i=(size_t(y)*w+x)*4;
        for(unsigned c=0;c<3;++c)rgba[i+c]=p[channels==1?0:c];
        rgba[i+3]=255;
    }
    pixels.resize(size_t(w)*h*4);
    require(skcms_Transform(rgba.data(),skcms_PixelFormat_RGBA_8888,skcms_AlphaFormat_Unpremul,&source,pixels.data(),working_format<Sample>(),skcms_AlphaFormat_Unpremul,&destination,size_t(w)*h),"JPEG color transform failed");
}
}
void validate(const Options& o) {
    range(static_cast<int>(o.mode),0,1,"mode");range(static_cast<int>(o.target),0,8,"target");
    range(static_cast<int>(o.quality.preset),0,4,"preset");
    range(o.quality.jxl_distance,0,3,"JXL distance");range(o.quality.codec_quality,1,100,"codec quality");
    if(o.quality.avif_depth!=10&&o.quality.avif_depth!=12)throw std::invalid_argument("AVIF depth must be 10 or 12");
    range(o.threads,0,65535,"threads");
    range(o.exposure,-3,3,"exposure");range(o.max_edge,0,65535,"max edge");
    const auto& r=o.raw;range(static_cast<int>(r.hdr_mode),0,2,"RAW HDR mode");range(r.exposure,-3,3,"RAW exposure");range(r.white_balance,0,8,"white balance");range(r.kelvin,2000,12000,"kelvin");
    range(r.highlight_stops,0,3,"highlight stops");range(r.shoulder,0,1,"shoulder");range(r.target_nits,1000,10000,"target nits");
    range(r.shadows,-1,1,"shadows");range(r.midtones,-1,1,"midtones");range(r.highlights,-1,1,"highlights");
    if(o.jpeg_transcoding&&(o.target!=Target::jxl||o.exposure||o.max_edge))throw std::invalid_argument("JPEG transcoding requires SDR JXL without exposure or resizing");
    if(o.mode==Mode::preserve) {
        const RawControls d;
        if(o.exposure||o.max_edge||r.hdr_mode!=RawHdrMode::source||r.exposure||r.white_balance||r.kelvin!=d.kelvin||r.highlight_stops||r.shoulder!=d.shoulder||r.target_nits!=d.target_nits||r.shadows||r.midtones||r.highlights||!r.camera_look||r.linear_hdr||!o.extended_bt2020)
            throw std::invalid_argument("Pixel and RAW controls require --mode process");
        if(o.target==Target::jxl_pq||o.target==Target::avif_pq)
            throw std::invalid_argument("An explicit transfer conversion requires --mode process; preserve uses the source transfer");
    }
}
struct ConversionSession::Impl {
    std::filesystem::path input;
    std::vector<uint8_t> bytes,exif;
    std::vector<float> linear,raw_sdr;
    unsigned w=0,h=0,bits=32,input_width=0,input_height=0;
    bool half_jxr=false,scrgb=false,integer_sdr=false;
    uhdr_color_gamut_t gamut=UHDR_CG_BT_709;
    int format=-1;
    bool ready=false;
    Options previous;
};
namespace {
bool scrgb_output(const Options& o) {return o.target==Target::jxl_scrgb;}
bool convert_gamut(const Options& o) {return o.convert_gamut && !scrgb_output(o);}
auto processing_key(const Options& o,int format) {
    const auto& r=o.raw;
    const int raw_kind=format==5?(o.target==Target::jpeg_gainmap||o.target==Target::avif_gainmap?2:hdr_target(o.target)?1:0):0;
    return std::make_tuple(o.mode,o.exposure,o.max_edge,o.keep_exif,format==3&&o.extended_bt2020,(format==3||format==4)&&convert_gamut(o),raw_kind,
        format==4 && (linear_jxl(o.target) || (o.mode==Mode::preserve && o.target==Target::jxl)) && !o.exposure && !o.max_edge && (!convert_gamut(o) || o.mode==Mode::preserve),
        r.hdr_mode,r.exposure,r.white_balance,r.kelvin,r.highlight_stops,r.shoulder,r.target_nits,
        r.shadows,r.midtones,r.highlights,r.camera_look,r.linear_hdr);
}
}
ConversionSession::ConversionSession(const std::filesystem::path& input):impl_(new Impl) {impl_->input=input;}
ConversionSession::~ConversionSession()=default;
Result convert(const std::filesystem::path& input,const std::filesystem::path& output,const Options& o) {
    return ConversionSession(input).convert(output,o);
}
Result ConversionSession::convert(const std::filesystem::path& output,const Options& o) {
    auto& cache=*impl_;const auto& input=cache.input;

    JxrcThreadBudget thread_budget(o.threads);
    validate(o);require(!output.empty(),"Empty output path");
    require(std::filesystem::is_regular_file(input),"Input must be a regular file");
    std::error_code ec;
    require(!std::filesystem::equivalent(input,output,ec),"Input and output refer to the same file");
    if(!o.overwrite && std::filesystem::exists(std::filesystem::symlink_status(output))) {
        Result skipped;skipped.skipped=true;return skipped;
    }
    if(cache.format<0){
        auto source=read(input);const int format=kind(source);
        if(format!=6 && format!=5){const auto info=inspect_bytes(source);cache.bits=info.bits;cache.input_width=info.width;cache.input_height=info.height;cache.half_jxr=format==4&&info.hdr&&info.bits==16;cache.integer_sdr=!info.hdr&&(format==0||format==1||(format==3&&is_integer_jxl(source.data(),source.size())));}
        cache.bytes=std::move(source);cache.format=format;
    }
    const auto& bytes=cache.bytes;const int format=cache.format;Staging stage(output);
    const auto path=stage.file.u8string();
    StillEncodeQuality q(static_cast<int>(o.quality.preset),o.quality.jxl_distance,o.quality.codec_quality,o.quality.avif_depth,o.quality.downsize_gainmap);
    Result result;
    std::unique_ptr<RegularImage> regular;
    bool direct_sdr=false;
    if(o.mode==Mode::process && (o.target==Target::jxl||o.target==Target::avif||o.target==Target::jpeg) && !o.exposure && !o.max_edge) {
        if(format==6) {
            regular=std::make_unique<RegularImage>(decode_regular(bytes.data(),bytes.size()));
            direct_sdr=!regular->hdr();
        } else direct_sdr=cache.integer_sdr;
    }
    if(direct_sdr && format==1 && o.target==Target::avif && cache.bits>q.avif_depth())direct_sdr=false;
    const bool direct_jxl=format==3 && (linear_jxl(o.target) || (o.mode==Mode::preserve && o.target==Target::jxl)) &&
        !o.exposure && !o.max_edge && (!convert_gamut(o) || o.mode==Mode::preserve) && is_scrgb_jxl(bytes.data(),bytes.size());
    if(o.mode==Mode::preserve && linear_jxl(o.target))require(format==4||direct_jxl,"Preserved linear HDR JXL requires a scRGB JXR source");
    if(o.mode==Mode::preserve && format==4)require(o.target==Target::jxl||linear_jxl(o.target),"JXR preservation requires linear JXL output");
    bool source_done=false;
    if(direct_sdr || direct_jxl || (o.mode==Mode::preserve && format!=4)) {
        require(format==0||format==1||format==3||format==6,"Source preservation supports JPEG, AVIF, JXL and regular raster images; use --mode process for RAW/JXR");
        int info[5]{};const int target=o.target==Target::jpeg||o.target==Target::jpeg_gainmap?0:(o.target==Target::jxl||direct_jxl)?1:2;
        int rc=0;
        if(o.jpeg_transcoding && format==0) {
            require(is_uhdr_image(const_cast<uint8_t*>(bytes.data()),static_cast<int>(bytes.size()))!=1,"JPEG coefficient transcoding cannot retain a gain map");
            ultrahdr::JpegDecoderHelper parser;
            require(parser.parseImage(bytes.data(),bytes.size()).error_code==UHDR_CODEC_OK,"JPEG header parse failed");
            info[0]=parser.getDecompressedImageWidth();info[1]=parser.getDecompressedImageHeight();info[3]=13;
            require(info[0]>0&&info[1]>0&&uint64_t(info[0])*info[1]<=80000000,"JPEG dimensions exceed limit");
            rc=transcode_jpeg(bytes.data(),bytes.size(),path.c_str(),o.keep_exif);
            require(rc==0,"JPEG coefficient transcoding failed (codec "+std::to_string(rc)+")");
            rc=1;
        } else {
            if(format==6) {
                if(!regular)regular=std::make_unique<RegularImage>(decode_regular(bytes.data(),bytes.size()));
                rc=convert_regular_source(*regular,target,path.c_str(),q,o.keep_exif,info);
            } else rc=convert_source(bytes.data(),bytes.size(),format,target,path.c_str(),q,o.keep_exif,info);
            require(rc>0 || (rc==0&&direct_sdr),"Source conversion unavailable or failed (codec "+std::to_string(rc)+"); no pixel-processing fallback was used");
        }
        if(rc>0) {
            source_done=true;
            result.width=info[0];result.height=info[1];result.gainmap=info[2];result.transfer=info[3];
            if(direct_sdr) {
                // Source-preserving exports can carry orientation structurally. Report
                // display dimensions, as the processed path does after rotation.
                if(regular) {
                    result.width=regular->width;result.height=regular->height;
                    if(regular->orientation>=5)std::swap(result.width,result.height);
                } else {result.width=cache.input_width;result.height=cache.input_height;}
            }
            if(o.target==Target::jpeg_gainmap||o.target==Target::avif_gainmap)require(result.gainmap,"Source has no transferable gain map; use --mode process to generate one");
            result.encoding=direct_sdr?"sdr":direct_jxl?"linear":result.gainmap?"gainmap":result.transfer==16?"pq":result.transfer==18?"hlg":"source";
        }
    }
    if(!source_done) {
        auto& linear=cache.linear;auto& raw_sdr=cache.raw_sdr;auto& exif=cache.exif;
        auto& w=cache.w;auto& h=cache.h;auto& gamut=cache.gamut;
        if(!cache.ready || processing_key(cache.previous,format)!=processing_key(o,format)) {
            cache.ready=false;cache.scrgb=false;linear.clear();raw_sdr.clear();exif.clear();w=h=0;gamut=UHDR_CG_BT_709;
            bool hdr=false;int gain=0,transfer=0,rc=0;unsigned raster_orientation=1;
            if(format==0)jpeg_linear(bytes,linear,w,h,gamut,hdr);
            else if(format==1)rc=decode_avif_to_linear_f32(bytes.data(),bytes.size(),linear,w,h,&gain,&transfer,&gamut,o.keep_exif?&exif:nullptr);
            else if(format==3)rc=decode_jxl_to_linear_f32(bytes.data(),bytes.size(),linear,w,h,&gamut,&hdr,&transfer,(is_scrgb_jxl(bytes.data(),bytes.size())?convert_gamut(o):o.extended_bt2020),o.keep_exif?&exif:nullptr);
            else if(format==4) {
                if((linear_jxl(o.target) || (o.mode==Mode::preserve && o.target==Target::jxl)) && !o.exposure && !o.max_edge && (!convert_gamut(o) || o.mode==Mode::preserve))
                    cache.scrgb=decode_jxr_scrgb(bytes.data(),bytes.size(),linear,w,h,cache.half_jxr);
                require(o.mode!=Mode::preserve || cache.scrgb,"JXR source is not preservable scRGB; use --mode process");
                if(!cache.scrgb)rc=decode_jxr_f32(bytes.data(),bytes.size(),linear,w,h,&gamut,convert_gamut(o));
            }
            else if(format==6) {
                const auto raster=regular?std::move(*regular):decode_regular(bytes.data(),bytes.size());
                require(!o.keep_exif || raster.format!="TIFF","TIFF EXIF retention is not implemented yet");
                w=raster.width;h=raster.height;raster_orientation=raster.orientation;cache.bits=raster.bits;
                if(o.keep_exif)exif=raster.exif;
                regular_to_linear(raster,linear,gamut);
            }
            else {
                int cg=0;const bool previous=paired_sdr;paired_sdr=o.target==Target::jpeg_gainmap||o.target==Target::avif_gainmap;
                struct Reset{bool value;~Reset(){paired_sdr=value;}} reset{previous};
                rc=decode_raw_f32(bytes.data(),bytes.size(),o.raw,hdr_target(o.target),o.max_edge,linear,raw_sdr,w,h,cg);
                gamut=cg==2?UHDR_CG_BT_2100:cg==1?UHDR_CG_DISPLAY_P3:UHDR_CG_BT_709;
            }
            require(rc==0&&w&&h&&uint64_t(w)*h<=80000000&&
                    linear.size()==size_t(w)*h*4,
                    "Image decode failed (codec "+std::to_string(rc)+")");
            require(!(o.keep_exif&&(format==4||format==5)),"EXIF retention for processed JXR/RAW is not implemented yet");
            // JXL decoder emits a 4-byte box offset before TIFF; normalize orientation after decode.
            if(format==3&&!exif.empty()) {
                require(exif.size()>=4,"Invalid JXL EXIF");
                const size_t offset=4+(uint32_t(exif[0])<<24|uint32_t(exif[1])<<16|uint32_t(exif[2])<<8|exif[3]);
                require(offset<=exif.size(),"Invalid JXL EXIF offset");exif.erase(exif.begin(),exif.begin()+offset);
            }
            const int orient=format==6?int(raster_orientation):source_orientation(bytes.data(),bytes.size(),format,format==0&&o.keep_exif?&exif:nullptr);
            require(orient>=1&&orient<=8,"Unsupported image orientation/crop");
            if(orient!=1) {
                const unsigned dw=orient>=5?h:w,dh=orient>=5?w:h;
                std::vector<float> oriented(linear.size());
                for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
                    unsigned dx=x,dy=y;
                    switch(orient) {
                        case 2:dx=w-1-x;break;
                        case 3:dx=w-1-x;dy=h-1-y;break;
                        case 4:dy=h-1-y;break;
                        case 5:dx=y;dy=x;break;
                        case 6:dx=h-1-y;dy=x;break;
                        case 7:dx=h-1-y;dy=w-1-x;break;
                        case 8:dx=y;dy=w-1-x;break;
                    }
                    std::copy_n(linear.data()+(size_t(y)*w+x)*4,4,oriented.data()+(size_t(dy)*dw+dx)*4);
                }
                linear.swap(oriented);w=dw;h=dh;
            }
            normalize_exif(exif);
            if(o.exposure) {
                require(raw_sdr.empty(),"Use --raw-exposure when exporting RAW gain maps");
                const float multiplier=std::exp2(o.exposure);
                for(size_t i=0;i<linear.size();++i)if(i%4!=3)linear[i]*=multiplier;
            }
            if(format!=5&&o.max_edge)scale_rgba_f32_max_edge(linear,w,h,o.max_edge);
            cache.previous=o;cache.ready=true;
        }
        q.exif=exif.data();q.exif_size=exif.size();q.source_bits=cache.bits;
        const int cg=gamut==UHDR_CG_BT_2100?2:gamut==UHDR_CG_DISPLAY_P3?1:0;
        int rc;
        if(cache.scrgb) {
            StillSourceColor color;color.bits=cache.half_jxr?16:32;color.exponent_bits=cache.half_jxr?5:8;
            color.transfer=8;color.primaries=1;color.intensity_target=80.f;
            if(cache.half_jxr) {
                std::vector<uint16_t> half(linear.size());
                std::transform(linear.begin(),linear.end(),half.begin(),float_to_half);
                rc=encode_jxl_source(w,h,half.data(),true,color,path.c_str(),q);
            } else rc=encode_jxl_source(w,h,linear.data(),true,color,path.c_str(),q);
        } else if(scrgb_output(o)) {
            // Working pixels use 203 nits per unit. scRGB uses 80 nits and
            // BT.709 primaries; retain signed/extended samples and straight alpha.
            // Encode a copy so other targets can reuse the prepared render.
            auto scrgb=linear;
            for(size_t i=0;i<scrgb.size();i+=4) {
                float& r=scrgb[i];float& g=scrgb[i+1];float& b=scrgb[i+2];
                if(gamut==UHDR_CG_BT_2100)linear_bt2020_to_bt709(r,g,b);
                else if(gamut==UHDR_CG_DISPLAY_P3)linear_p3_to_bt709(r,g,b);
                r*=kSdrWhiteNits/80.f;g*=kSdrWhiteNits/80.f;b*=kSdrWhiteNits/80.f;
            }
            StillSourceColor color;color.bits=32;color.exponent_bits=8;
            color.transfer=8;color.primaries=1;color.intensity_target=80.f;
            rc=encode_jxl_source(w,h,scrgb.data(),true,color,path.c_str(),q);
        } else rc=encode_still_export_f32(linear.data(),w,h,cg,static_cast<int>(o.target),path.c_str(),q,raw_sdr.empty()?nullptr:raw_sdr.data());
        require(rc==0,"Image encode failed (codec "+std::to_string(rc)+")");
        result.width=w;result.height=h;result.gainmap=o.target==Target::jpeg_gainmap||o.target==Target::avif_gainmap;
        result.transfer=o.target==Target::avif_pq||o.target==Target::jxl_pq?16:0;
        result.encoding=result.gainmap?"gainmap":result.transfer==16?"pq":(linear_jxl(o.target)||cache.scrgb)?"linear":"sdr";
    }
    require(std::filesystem::file_size(stage.file)>0,"Encoder produced no output");stage.publish(output,o.overwrite);return result;
}
Preview preview(const std::filesystem::path& input,unsigned edge) {
    require(edge>0&&edge<=1024,"Preview edge must be 1..1024");
    JxrcThreadBudget budget(1);
    auto bytes=read(input);const int format=kind(bytes);
    std::vector<uint16_t> linear;std::vector<uint8_t> raw_pixels;
    unsigned w=0,h=0,raster_orientation=1;uhdr_color_gamut_t gamut=UHDR_CG_BT_709;bool hdr=false;int gain=0,transfer=0,rc=0;
    if(format==0)jpeg_linear(bytes,linear,w,h,gamut,hdr);
    else if(format==1)rc=decode_avif_to_linear_f16(bytes.data(),bytes.size(),linear,w,h,&gain,&transfer,&gamut,edge,nullptr);
    else if(format==3)rc=decode_jxl_to_linear_f16(bytes.data(),bytes.size(),linear,w,h,&gamut,&hdr,&transfer,false,edge,false,nullptr,nullptr);
    else if(format==4)rc=decode_jxr_memory(bytes.data(),bytes.size(),linear,w,h,&gamut,std::min(edge,768u));
    else if(format==6) {
        const auto raster=decode_regular(bytes.data(),bytes.size());
        w=raster.width;h=raster.height;raster_orientation=raster.orientation;regular_to_linear(raster,linear,gamut);
    }
    else {
        std::vector<uint8_t> jpeg;bool embedded=false;
        if(raw_jpeg_thumbnail(bytes.data(),bytes.size(),jpeg)) {
            try {
                jpeg_linear(jpeg,linear,w,h,gamut,hdr);
                const int orient=source_orientation(jpeg.data(),jpeg.size(),0,nullptr);
                require(orient>=1 && orient<=8,"Invalid RAW thumbnail orientation");
                raster_orientation=unsigned(orient);embedded=true;
            } catch(const std::exception&) {linear.clear();w=h=0;}
        }
        if(!embedded) {
            int cg=0;rc=decode_raw(bytes.data(),bytes.size(),RawControls{},false,edge,raw_pixels,w,h,cg);
            gamut=cg==2?UHDR_CG_BT_2100:cg==1?UHDR_CG_DISPLAY_P3:UHDR_CG_BT_709;
            if(rc==0){linear.resize(size_t(w)*h*4);std::memcpy(linear.data(),raw_pixels.data(),linear.size()*2);}
        }
    }
    require(rc==0&&w&&h&&linear.size()==size_t(w)*h*4,"Thumbnail decode failed");
    scale_rgba_f16_max_edge(linear,w,h,edge);
    const int orient=(format==6||format==5)?int(raster_orientation):source_orientation(bytes.data(),bytes.size(),format,nullptr);
    require(orient>=1&&orient<=8,"Unsupported thumbnail orientation/crop");
    Preview output;output.width=orient>=5?h:w;output.height=orient>=5?w:h;output.rgba.resize(size_t(w)*h*4);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
        unsigned dx=x,dy=y;
        switch(orient){case 2:dx=w-1-x;break;case 3:dx=w-1-x;dy=h-1-y;break;case 4:dy=h-1-y;break;
            case 5:dx=y;dy=x;break;case 6:dx=h-1-y;dy=x;break;case 7:dx=h-1-y;dy=w-1-x;break;case 8:dx=y;dy=w-1-x;break;}
        const size_t i=(size_t(y)*w+x)*4,o=(size_t(dy)*output.width+dx)*4;
        float r=half_to_float(linear[i]),g=half_to_float(linear[i+1]),b=half_to_float(linear[i+2]);
        if(gamut==UHDR_CG_DISPLAY_P3)linear_p3_to_bt709(r,g,b);
        if(gamut==UHDR_CG_BT_2100)linear_bt2020_to_bt709(r,g,b);
        // A simple display-only shoulder keeps bright thumbnails readable.
        const float peak=std::max({r,g,b,1.f});
        const float scale=peak>1.f?1.f/peak:1.f;
        output.rgba[o]=linear_to_srgb_u8(r*scale);output.rgba[o+1]=linear_to_srgb_u8(g*scale);output.rgba[o+2]=linear_to_srgb_u8(b*scale);
        output.rgba[o+3]=uint8_t(std::clamp(half_to_float(linear[i+3]),0.f,1.f)*255.f+0.5f);
    }
    return output;
}
std::string version() {return JXRC_VERSION;}
} // namespace jxrc
