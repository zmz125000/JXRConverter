#include "jxrc/converter.hpp"
#include "jxrc/version.hpp"
#include "reader_pixels.h"
#include "codec_api.h"
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
bool hdr_target(Target t) {return t==Target::jpeg_gainmap||t==Target::jxl_linear||t==Target::avif_pq||t==Target::avif_gainmap||t==Target::jxl_pq;}
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
    if(b.size()>=12&&!std::memcmp(b.data()+4,"ftyp",4))return 1;
    if(b.size()>=2&&((b[0]==255&&b[1]==10)||(b.size()>=12&&!std::memcmp(b.data(),"\0\0\0\x0cJXL \r\n\x87\n",12))))return 3;
    if(b.size()>=4&&b[0]=='I'&&b[1]=='I'&&b[2]==0xbc&&b[3]==1)return 4;
    return 5; // Sensor formats are validated by LibRaw, never guessed from suffix.
}
void jpeg_linear(const std::vector<uint8_t>& bytes,std::vector<uint16_t>& pixels,unsigned& w,unsigned& h,uhdr_color_gamut_t& gamut,bool& hdr) {
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
        for(unsigned y=0;y<h;++y)std::memcpy(pixels.data()+size_t(y)*w*4,static_cast<const uint16_t*>(image->planes[0])+size_t(y)*image->stride[0]*4,size_t(w)*8);
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
    require(skcms_Transform(rgba.data(),skcms_PixelFormat_RGBA_8888,skcms_AlphaFormat_Unpremul,&source,pixels.data(),skcms_PixelFormat_RGBA_hhhh,skcms_AlphaFormat_Unpremul,&destination,size_t(w)*h),"JPEG color transform failed");
}
}
void validate(const Options& o) {
    range(static_cast<int>(o.mode),0,1,"mode");range(static_cast<int>(o.target),0,7,"target");
    range(static_cast<int>(o.quality.preset),0,4,"preset");
    range(o.quality.jxl_distance,0,3,"JXL distance");range(o.quality.codec_quality,1,100,"codec quality");
    if(o.quality.avif_depth!=10&&o.quality.avif_depth!=12)throw std::invalid_argument("AVIF depth must be 10 or 12");
    range(o.threads,0,65535,"threads");
    range(o.exposure,-3,3,"exposure");range(o.max_edge,0,65535,"max edge");
    const auto& r=o.raw;range(static_cast<int>(r.hdr_mode),0,2,"RAW HDR mode");range(r.exposure,-3,3,"RAW exposure");range(r.white_balance,0,8,"white balance");range(r.kelvin,2000,12000,"kelvin");
    range(r.highlight_stops,0,3,"highlight stops");range(r.shoulder,0,1,"shoulder");range(r.target_nits,1000,10000,"target nits");
    range(r.shadows,-1,1,"shadows");range(r.midtones,-1,1,"midtones");range(r.highlights,-1,1,"highlights");
    if(o.jpeg_transcoding&&(o.mode!=Mode::preserve||o.target!=Target::jxl))throw std::invalid_argument("JPEG transcoding requires preserve mode and JXL target");
    if(o.mode==Mode::preserve) {
        const RawControls d;
        if(o.exposure||o.max_edge||r.hdr_mode!=RawHdrMode::source||r.exposure||r.white_balance||r.kelvin!=d.kelvin||r.highlight_stops||r.shoulder!=d.shoulder||r.target_nits!=d.target_nits||r.shadows||r.midtones||r.highlights||!r.camera_look||r.linear_hdr||!o.extended_bt2020)
            throw std::invalid_argument("Pixel and RAW controls require --mode process");
        if(o.target==Target::jxl_linear||o.target==Target::jxl_pq||o.target==Target::avif_pq)
            throw std::invalid_argument("An explicit transfer conversion requires --mode process; preserve uses the source transfer");
    }
}
Result convert(const std::filesystem::path& input,const std::filesystem::path& output,const Options& o) {
    JxrcThreadBudget thread_budget(o.threads);
    validate(o);require(!output.empty(),"Empty output path");
    require(std::filesystem::is_regular_file(input),"Input must be a regular file");
    std::error_code ec;
    require(!std::filesystem::equivalent(input,output,ec),"Input and output refer to the same file");
    require(o.overwrite||!std::filesystem::exists(output),"Output exists; pass --overwrite to replace it");
    auto bytes=read(input);const int format=kind(bytes);Staging stage(output);
    const auto path=stage.file.u8string();
    StillEncodeQuality q(static_cast<int>(o.quality.preset),o.quality.jxl_distance,o.quality.codec_quality,o.quality.avif_depth,o.quality.downsize_gainmap);
    Result result;
    if(o.mode==Mode::preserve) {
        require(format==0||format==1||format==3,"Source preservation supports JPEG, AVIF and JXL; use --mode process for RAW/JXR");
        int info[5]{};const int target=o.target==Target::jpeg||o.target==Target::jpeg_gainmap?0:o.target==Target::jxl?1:2;
        int rc=0;
        if(o.jpeg_transcoding) {
            require(format==0,"JPEG transcoding requires a JPEG source");
            require(is_uhdr_image(bytes.data(),static_cast<int>(bytes.size()))!=1,"JPEG coefficient transcoding cannot retain a gain map");
            ultrahdr::JpegDecoderHelper parser;
            require(parser.parseImage(bytes.data(),bytes.size()).error_code==UHDR_CODEC_OK,"JPEG header parse failed");
            info[0]=parser.getDecompressedImageWidth();info[1]=parser.getDecompressedImageHeight();info[3]=13;
            require(info[0]>0&&info[1]>0&&uint64_t(info[0])*info[1]<=80000000,"JPEG dimensions exceed limit");
            rc=transcode_jpeg(bytes.data(),bytes.size(),path.c_str(),o.keep_exif);
            require(rc==0,"JPEG coefficient transcoding failed (codec "+std::to_string(rc)+")");
        } else {
            rc=convert_source(bytes.data(),bytes.size(),format,target,path.c_str(),q,o.keep_exif,info);
            require(rc>0,"Source conversion unavailable or failed (codec "+std::to_string(rc)+"); no pixel-processing fallback was used");
        }
        result.width=info[0];result.height=info[1];result.gainmap=info[2];result.transfer=info[3];
        if(o.target==Target::jpeg_gainmap||o.target==Target::avif_gainmap)require(result.gainmap,"Source has no transferable gain map; use --mode process to generate one");
        result.encoding=result.gainmap?"gainmap":result.transfer==16?"pq":result.transfer==18?"hlg":"source";
    } else {
        std::vector<uint16_t> linear;std::vector<uint8_t> raw_pixels,exif;unsigned w=0,h=0;uhdr_color_gamut_t gamut=UHDR_CG_BT_709;
        bool hdr=false;int gain=0,transfer=0,rc=0;size_t sdr_offset=0;
        if(format==0)jpeg_linear(bytes,linear,w,h,gamut,hdr);
        else if(format==1)rc=decode_avif_to_linear_f16(bytes.data(),bytes.size(),linear,w,h,&gain,&transfer,&gamut,0,o.keep_exif?&exif:nullptr);
        else if(format==3)rc=decode_jxl_to_linear_f16(bytes.data(),bytes.size(),linear,w,h,&gamut,&hdr,&transfer,false,0,o.extended_bt2020,o.keep_exif?&exif:nullptr,nullptr);
        else if(format==4)rc=decode_jxr_memory(bytes.data(),bytes.size(),linear,w,h,&gamut);
        else {
            int cg=0;const bool previous=paired_sdr;paired_sdr=o.target==Target::jpeg_gainmap||o.target==Target::avif_gainmap;
            struct Reset{bool value;~Reset(){paired_sdr=value;}} reset{previous};
            rc=decode_raw(bytes.data(),bytes.size(),o.raw,hdr_target(o.target),o.max_edge,raw_pixels,w,h,cg);
            gamut=cg==2?UHDR_CG_BT_2100:cg==1?UHDR_CG_DISPLAY_P3:UHDR_CG_BT_709;
            if(rc==0) {linear.resize(size_t(w)*h*4);std::memcpy(linear.data(),raw_pixels.data(),linear.size()*2);if(raw_pixels.size()>linear.size()*2)sdr_offset=linear.size()*2;}
        }
        require(rc==0&&w&&h&&uint64_t(w)*h<=80000000&&linear.size()==size_t(w)*h*4,"Image decode failed (codec "+std::to_string(rc)+")");
        require(!(o.keep_exif&&(format==4||format==5)),"EXIF retention for processed JXR/RAW is not implemented yet");
        // JXL decoder emits a 4-byte box offset before TIFF; normalize orientation after decode.
        if(format==3&&!exif.empty()) {
            require(exif.size()>=4,"Invalid JXL EXIF");
            const size_t offset=4+(uint32_t(exif[0])<<24|uint32_t(exif[1])<<16|uint32_t(exif[2])<<8|exif[3]);
            require(offset<=exif.size(),"Invalid JXL EXIF offset");exif.erase(exif.begin(),exif.begin()+offset);
        }
        const int orient=source_orientation(bytes.data(),bytes.size(),format,format==0&&o.keep_exif?&exif:nullptr);
        require(orient>=1&&orient<=8,"Unsupported image orientation/crop");
        if(orient!=1) {
            const unsigned dw=orient>=5?h:w,dh=orient>=5?w:h;
            std::vector<uint16_t> oriented(linear.size());
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
        q.exif=exif.data();q.exif_size=exif.size();
        if(o.exposure) {
            require(!sdr_offset,"Use --raw-exposure when exporting RAW gain maps");
            const float multiplier=std::exp2(o.exposure);
            for(size_t i=0;i<linear.size();++i)if(i%4!=3)linear[i]=float_to_half(half_to_float(linear[i])*multiplier);
        }
        if(format!=5&&o.max_edge)scale_rgba_f16_max_edge(linear,w,h,o.max_edge);
        const uint8_t* data=reinterpret_cast<const uint8_t*>(linear.data());size_t size=linear.size()*2;
        if(sdr_offset) {data=raw_pixels.data();size=raw_pixels.size();}
        const int cg=gamut==UHDR_CG_BT_2100?2:gamut==UHDR_CG_DISPLAY_P3?1:0;
        rc=encode_still_export(data,size,w,h,true,cg,static_cast<int>(o.target),false,path.c_str(),sdr_offset,q,false);
        require(rc==0,"Image encode failed (codec "+std::to_string(rc)+")");
        result.width=w;result.height=h;result.gainmap=o.target==Target::jpeg_gainmap||o.target==Target::avif_gainmap;
        result.transfer=o.target==Target::avif_pq||o.target==Target::jxl_pq?16:0;
        result.encoding=result.gainmap?"gainmap":result.transfer==16?"pq":o.target==Target::jxl_linear?"linear":"sdr";
    }
    require(std::filesystem::file_size(stage.file)>0,"Encoder produced no output");stage.publish(output,o.overwrite);return result;
}
Preview preview(const std::filesystem::path& input,unsigned edge) {
    require(edge>0&&edge<=1024,"Preview edge must be 1..1024");
    JxrcThreadBudget budget(1);
    auto bytes=read(input);const int format=kind(bytes);
    std::vector<uint16_t> linear;std::vector<uint8_t> raw_pixels;
    unsigned w=0,h=0;uhdr_color_gamut_t gamut=UHDR_CG_BT_709;bool hdr=false;int gain=0,transfer=0,rc=0;
    if(format==0)jpeg_linear(bytes,linear,w,h,gamut,hdr);
    else if(format==1)rc=decode_avif_to_linear_f16(bytes.data(),bytes.size(),linear,w,h,&gain,&transfer,&gamut,edge,nullptr);
    else if(format==3)rc=decode_jxl_to_linear_f16(bytes.data(),bytes.size(),linear,w,h,&gamut,&hdr,&transfer,false,edge,false,nullptr,nullptr);
    else if(format==4)rc=decode_jxr_memory(bytes.data(),bytes.size(),linear,w,h,&gamut);
    else {
        int cg=0;rc=decode_raw(bytes.data(),bytes.size(),RawControls{},false,edge,raw_pixels,w,h,cg);
        gamut=cg==2?UHDR_CG_BT_2100:cg==1?UHDR_CG_DISPLAY_P3:UHDR_CG_BT_709;
        if(rc==0){linear.resize(size_t(w)*h*4);std::memcpy(linear.data(),raw_pixels.data(),linear.size()*2);}
    }
    require(rc==0&&w&&h&&linear.size()==size_t(w)*h*4,"Thumbnail decode failed");
    scale_rgba_f16_max_edge(linear,w,h,edge);
    const int orient=source_orientation(bytes.data(),bytes.size(),format,nullptr);
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
