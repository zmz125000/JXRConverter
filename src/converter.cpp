#include "native/image_orientation.h"
#include "native/image_size.h"
#include "native/animation.h"
#include "native/animation_decode.h"
#include "jxrc/converter.hpp"
#include "jxrc/version.hpp"
#include "reader_pixels.h"
#include "codec_api.h"
#include "render_analysis.h"
#include "native/sdr_mapping.h"
#include "native/staged_output.h"
#include "working_pixels.h"
#include "input_format.h"
#include "heif_decode.h"
#include "gain_map.h"
#include "regular_decode.h"
#include "exif_metadata.h"
#include "hdr_parallel.h"
#include "linear_color_profile.h"
#include "skcms.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <tuple>
#include <stdexcept>

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
bool hlg_target(Target t) {return t==Target::png_hlg||t==Target::jxl_hlg||t==Target::avif_hlg;}
bool gain_target(Target t) {return t==Target::jpeg_gainmap||t==Target::avif_gainmap||t==Target::jxl_gainmap;}
bool hdr_target(Target t) {return gain_target(t)||linear_jxl(t)||t==Target::avif_pq||t==Target::jxl_pq||t==Target::png_hdr||hlg_target(t);}
std::vector<uint8_t> read(const std::filesystem::path& path,const std::function<bool()>& cancelled={}) {
    if(cancelled&&cancelled())throw std::runtime_error("Image read cancelled");
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    require(bool(f),"Cannot open input: "+path.u8string());
    const auto n=f.tellg();require(n>0&&n<=512LL*1024*1024,"Input must be 1 byte to 512 MiB");
    std::vector<uint8_t> bytes(static_cast<size_t>(n));f.seekg(0);
    for(size_t offset=0;offset<bytes.size();) {
        if(cancelled&&cancelled())throw std::runtime_error("Image read cancelled");
        const size_t count=cancelled?std::min<size_t>(256*1024,bytes.size()-offset):bytes.size()-offset;
        require(bool(f.read(reinterpret_cast<char*>(bytes.data()+offset),count)),"Cannot read input");offset+=count;
    }
    if(cancelled&&cancelled())throw std::runtime_error("Image read cancelled");
    return bytes;
}
int kind(const std::vector<uint8_t>& b) {
    if(b.size()>2&&b[0]==255&&b[1]==216)return 0;
    if(is_avif_input(b.data(),b.size()))return 1;
    if(is_heif_input(b.data(),b.size()))return 7;
    if(b.size()>=2&&((b[0]==255&&b[1]==10)||(b.size()>=12&&!std::memcmp(b.data(),"\0\0\0\x0cJXL \r\n\x87\n",12))))return 3;
    if(b.size()>=4&&b[0]=='I'&&b[1]=='I'&&b[2]==0xbc&&b[3]==1)return 4;
    if(is_regular_image(b.data(),b.size()))return 6;
    return 5; // Sensor formats are validated by LibRaw, never guessed from suffix.
}
// Preview-only reduction, before allocating color-transform intermediates.
// Encoded integer rasters are box filtered; half-float HDR is averaged as float.
template<class Code, bool Half = false>
std::vector<Code> reduce_preview(const Code* source,unsigned& w,unsigned& h,size_t stride,unsigned channels,unsigned edge) {
    const unsigned longest=std::max(w,h);
    if(!edge||longest<=edge)return {};
    const unsigned nw=std::max(1u,unsigned(uint64_t(w)*edge/longest));
    const unsigned nh=std::max(1u,unsigned(uint64_t(h)*edge/longest));
    std::vector<Code> result(size_t(nw)*nh*channels);
    for(unsigned y=0;y<nh;++y)for(unsigned x=0;x<nw;++x) {
        const unsigned x0=unsigned(uint64_t(x)*w/nw),x1=unsigned(uint64_t(x+1)*w/nw);
        const unsigned y0=unsigned(uint64_t(y)*h/nh),y1=unsigned(uint64_t(y+1)*h/nh);
        double sums[4]={};
        for(unsigned sy=y0;sy<y1;++sy)for(unsigned sx=x0;sx<x1;++sx)for(unsigned c=0;c<channels;++c) {
            const auto value=source[size_t(sy)*stride+size_t(sx)*channels+c];
            if constexpr(Half)sums[c]+=half_to_float(value);else sums[c]+=value;
        }
        const double count=double(x1-x0)*(y1-y0);
        for(unsigned c=0;c<channels;++c) {
            if constexpr(Half)result[(size_t(y)*nw+x)*channels+c]=float_to_half(float(sums[c]/count));
            else result[(size_t(y)*nw+x)*channels+c]=Code(sums[c]/count+0.5);
        }
    }
    w=nw;h=nh;return result;
}
template<class Sample>
void jpeg_linear(const std::vector<uint8_t>& bytes,std::vector<Sample>& pixels,unsigned& w,unsigned& h,uhdr_color_gamut_t& gamut,bool& hdr,unsigned thumbnail_edge=0,std::vector<float>* sdr=nullptr) {
    using Decoder=std::unique_ptr<uhdr_codec_private_t,void(*)(uhdr_codec_private_t*)>;
    Decoder dec(uhdr_create_decoder(),uhdr_release_decoder);
    require(bool(dec),"Cannot allocate JPEG decoder");
    uhdr_compressed_image_t compressed{const_cast<uint8_t*>(bytes.data()),bytes.size(),bytes.size(),UHDR_CG_UNSPECIFIED,UHDR_CT_UNSPECIFIED,UHDR_CR_FULL_RANGE};
        auto status=uhdr_dec_set_out_img_format(dec.get(),UHDR_IMG_FMT_64bppRGBAHalfFloat);
        require(status.error_code==UHDR_CODEC_OK,"Ultra HDR output format failed");
        status=uhdr_dec_set_out_color_transfer(dec.get(),UHDR_CT_LINEAR);
        require(status.error_code==UHDR_CODEC_OK,"Ultra HDR output transfer failed");
    if(uhdr_dec_set_image(dec.get(),&compressed).error_code==UHDR_CODEC_OK&&uhdr_dec_probe(dec.get()).error_code==UHDR_CODEC_OK) {
        require(uhdr_dec_get_image_width(dec.get())>0&&uhdr_dec_get_image_height(dec.get())>0&&fits_working_image(uhdr_dec_get_image_width(dec.get()),uhdr_dec_get_image_height(dec.get())),"JPEG dimensions exceed limit");
        if constexpr(std::is_same_v<Sample,float>) {
            auto source=decode_jpeg_gain_map(bytes.data(),bytes.size());
            require(bool(source)&&reconstruct_gain_map_f32(source.get(),pixels,gamut,sdr),"JPEG gain-map F32 reconstruction failed");
            w=source->width;h=source->height;hdr=true;
            if(thumbnail_edge)scale_rgba_f32_max_edge(pixels,w,h,thumbnail_edge);
            return;
        }
        status=uhdr_decode(dec.get());
        require(status.error_code==UHDR_CODEC_OK,std::string("Ultra HDR decode failed: ")+(status.has_detail?status.detail:"no codec detail"));
        const auto* decoded=uhdr_get_decoded_image(dec.get());require(decoded,"Missing Ultra HDR pixels");
        auto reduced=*decoded;const auto* image=&reduced;
        w=image->w;h=image->h;gamut=image->cg;hdr=true;
        auto preview_pixels=reduce_preview<uint16_t,true>(static_cast<const uint16_t*>(image->planes[0]),w,h,size_t(image->stride[0])*4,4,thumbnail_edge);
        if(!preview_pixels.empty()){reduced.w=w;reduced.h=h;reduced.planes[0]=preview_pixels.data();reduced.stride[0]=w;}
        pixels.resize(size_t(w)*h*4);
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w*4;++x)
            pixels[size_t(y)*w*4+x]=store_working<Sample>(half_to_float(static_cast<const uint16_t*>(image->planes[0])[size_t(y)*image->stride[0]*4+x]));
        return;
    }
    auto image=decode_jpeg(bytes.data(),bytes.size());w=image.width;h=image.height;hdr=false;
    auto reduced=reduce_preview(image.pixels.data(),w,h,size_t(w)*4,4,thumbnail_edge);
    if(!reduced.empty())image.pixels=std::move(reduced);
    image.width=w;image.height=h;
    regular_to_linear(image,pixels,gamut);
}
}
void validate(const Options& o) {
    range(static_cast<int>(o.transparency_background),0,3,"transparency background");
    range(static_cast<int>(o.gain_map_reconstruction),0,2,"gain map reconstruction");
    range(static_cast<int>(o.mode),0,1,"mode");range(static_cast<int>(o.target),0,static_cast<int>(Target::jxl_gainmap),"target");
    range(static_cast<int>(o.quality.preset),0,4,"preset");
    range(o.quality.jxl_distance,0,3,"JXL distance");range(o.quality.codec_quality,1,100,"codec quality");
    if(o.quality.avif_depth!=8&&o.quality.avif_depth!=10&&o.quality.avif_depth!=12)throw std::invalid_argument("AVIF depth must be 8, 10 or 12");
    if(o.quality.png_depth!=8&&o.quality.png_depth!=16)throw std::invalid_argument("PNG depth must be 8 or 16");
    range(o.threads,0,65535,"threads");
    range(o.exposure,-3,3,"exposure");range(o.max_edge,0,65535,"max edge");
    range(static_cast<int>(o.sdr_tone_mapper),0,static_cast<int>(SdrToneMapper::mpcvr),"SDR tone mapper");range(static_cast<int>(o.sdr_color_mapping),0,2,"SDR color mapping");
    range(o.sdr_shoulder,0,0.99,"SDR shoulder");range(o.sdr_hdr_range_nits,204,10000,"SDR HDR range");
    range(o.sdr_shadows,-1,1,"SDR shadows");range(o.sdr_midtones,-1,1,"SDR midtones");range(o.sdr_highlights,-1,1,"SDR highlights");
    const auto& r=o.raw;range(static_cast<int>(r.hdr_mode),0,2,"RAW HDR mode");range(r.exposure,-3,3,"RAW exposure");range(r.white_balance,0,8,"white balance");range(r.kelvin,2000,12000,"kelvin");
    range(r.highlight_stops,0,3,"highlight stops");range(r.shoulder,0,1,"shoulder");range(r.target_nits,1000,10000,"target nits");
    range(r.shadows,-1,1,"shadows");range(r.midtones,-1,1,"midtones");range(r.highlights,-1,1,"highlights");
    if(o.jpeg_transcoding&&(o.target!=Target::jxl||o.exposure||o.max_edge||o.srgb_clamp))throw std::invalid_argument("JPEG transcoding requires SDR JXL without exposure, resizing or sRGB clamping");
    if(o.mode==Mode::preserve) {
        const RawControls d;
        if(o.srgb_clamp||o.exposure||o.max_edge||o.sdr_tone_map||r.hdr_mode!=RawHdrMode::source||r.exposure||r.auto_exposure||r.white_balance||r.kelvin!=d.kelvin||r.highlight_stops||r.shoulder!=d.shoulder||r.target_nits!=d.target_nits||r.shadows||r.midtones||r.highlights||!r.camera_look||r.linear_hdr||!o.extended_bt2020)
            throw std::invalid_argument("Pixel and RAW controls require --mode process");
        if(o.target==Target::jxl_gainmap)throw std::invalid_argument("JXL gain-map output requires Process pixels mode");
        if(o.target==Target::jxl_pq||o.target==Target::avif_pq||o.target==Target::png_hdr||hlg_target(o.target))
            throw std::invalid_argument("An explicit transfer conversion requires --mode process; preserve uses the source transfer");
    }
}
struct ConversionSession::Impl {
    std::filesystem::path input;
    std::vector<uint8_t> bytes,exif;
    std::vector<float> linear,sdr_base;
    std::vector<float> mapped_sdr;
    float mapped_shoulder=-1.f,mapped_range=-1.f;
    SdrToneMapper mapped_mapper=SdrToneMapper::extended_reinhard;
    SdrColorMapping mapped_color_mapping=SdrColorMapping::max_rgb;
    std::array<float,3> mapped_zones{};
    bool mapped_auto_range=false,mapped_auto_curve=false;
    int mapped_cap=0;
    RenderAnalysis analysis;
    unsigned w=0,h=0,bits=32,input_width=0,input_height=0;
    bool half_jxr=false,scrgb=false,integer_sdr=false,gainmap=false,source_hdr=false;
    uhdr_color_gamut_t gamut=UHDR_CG_BT_709;
    int format=-1;
    bool ready=false,animated=false;
    std::string source_transfer;
    Options previous;
};
namespace {
bool remove_alpha(const Options& o) {return o.transparency_background==TransparencyBackground::white_remove_alpha||o.transparency_background==TransparencyBackground::black_remove_alpha;}
bool white_background(const Options& o) {return o.transparency_background==TransparencyBackground::white||o.transparency_background==TransparencyBackground::white_remove_alpha;}
bool scrgb_output(const Options& o) {return o.target==Target::jxl_scrgb;}
bool convert_gamut(const Options& o) {return o.convert_gamut && !o.srgb_clamp && !scrgb_output(o);}
bool prepare_animation_rgb8(RegularImage& raster,unsigned primaries,bool opaque_required) {
    if(raster.bits!=8||raster.channels!=4||raster.transfer!=13||
       !raster.floating_pixels.empty())return false;
    if(opaque_required)for(size_t i=3;i<raster.pixels.size();i+=4)if(raster.pixels[i]!=255)return false;
    if(raster.icc.empty()&&raster.primaries==primaries)return true;

    // Normalize to the animation's common gamut while keeping compatible
    // SDR frames in 8-bit RGBA, including when the list contains HDR frames.
    skcms_ICCProfile source{},destination{};
    const skcms_ICCProfile* src=skcms_sRGB_profile();
    const skcms_ICCProfile* dst=src;
    if(!raster.icc.empty()) {
        if(!skcms_Parse(raster.icc.data(),raster.icc.size(),&source))return false;
        src=&source;
    } else if(raster.primaries!=1) {
        if(raster.primaries!=9||!make_bt2020_profile(&source,skcms_sRGB_TransferFunction()))return false;
        src=&source;
    }
    if(primaries==9) {
        if(!make_bt2020_profile(&destination,skcms_sRGB_TransferFunction()))return false;
        dst=&destination;
    }
    const auto format=skcms_PixelFormat_RGBA_8888;
    const auto alpha=skcms_AlphaFormat_Unpremul;
    return skcms_Transform(raster.pixels.data(),format,alpha,src,
                           raster.pixels.data(),format,alpha,dst,raster.pixels.size()/4);
}

auto processing_key(const Options& o,int format) {
    const auto& r=o.raw;
    const int raw_kind=format==5?(gain_target(o.target)?2:hdr_target(o.target)?1:0):0;
    return std::make_tuple(o.gain_map_reconstruction==GainMapReconstruction::full_image,remove_alpha(o),remove_alpha(o)&&white_background(o),o.mode,format==5?0.f:o.exposure,o.max_edge,o.keep_exif,o.srgb_clamp,format==3&&o.extended_bt2020&&!o.srgb_clamp,(format==3||format==4)&&convert_gamut(o),raw_kind,
        format==4 && (linear_jxl(o.target) || (o.mode==Mode::preserve && o.target==Target::jxl)) && !o.exposure && !o.max_edge && (!convert_gamut(o) || o.mode==Mode::preserve),
        r.hdr_mode,r.exposure,r.white_balance,r.kelvin,r.highlight_stops,r.shoulder,r.target_nits,
        r.shadows,r.midtones,r.highlights,r.camera_look,r.linear_hdr,r.auto_exposure,r.auto_exposure_protect_highlights);
}
}
ConversionSession::ConversionSession(const std::filesystem::path& input):impl_(new Impl) {impl_->input=input;}
ConversionSession::~ConversionSession()=default;
Result convert(const std::filesystem::path& input,const std::filesystem::path& output,const Options& o) {
    return ConversionSession(input).convert(output,o);
}
Result ConversionSession::convert(const std::filesystem::path& output,const Options& o) {
    return convert_frames(output,o,false,{},{});
}

void ConversionSession::load(const std::function<bool()>& cancelled) {
    auto& cache=*impl_;
    if(cache.format>=0)return;
    require(std::filesystem::is_regular_file(cache.input),"Input must be a regular file");
    auto source=read(cache.input,cancelled);
    const int format=kind(source);
    const bool sequence_capable_raster=format==6&&
        ((source.size()>=6&&!std::memcmp(source.data(),"GIF",3))||
         (source.size()>=12&&!std::memcmp(source.data()+8,"WEBP",4))||
         (source.size()>=8&&!std::memcmp(source.data(),"\x89PNG\r\n\x1a\n",8)));
    if(format!=5&&(format!=6||sequence_capable_raster)) {
        const auto info=inspect_bytes(source);
        cache.gainmap=info.gainmap;cache.source_hdr=info.hdr;cache.bits=info.bits;
        cache.input_width=info.width;cache.input_height=info.height;
        cache.half_jxr=format==4&&info.hdr&&info.bits==16;
        cache.animated=info.animated;cache.source_transfer=info.transfer;
        cache.integer_sdr=!info.hdr&&(format==0||format==1||format==7||
            (format==3&&!info.animated&&is_integer_jxl(source.data(),source.size())));
    }
    cache.bytes=std::move(source);cache.format=format;
}

void ConversionSession::read_frames(const Options& o,const AnimationFrameReader& frame,
                                    const std::function<bool()>& cancelled) {
    const auto& cache=*impl_;const auto& bytes=cache.bytes;
    JxrcThreadBudget budget(o.threads);
    if(cache.format==6) {
        require(decode_raster_animation(bytes.data(),bytes.size(),frame,false,cancelled),"Unsupported raster animation");
        return;
    }
    const int rc=cache.format==1?decode_avif_animation(bytes.data(),bytes.size(),frame,cancelled):
        decode_jxl_animation(bytes.data(),bytes.size(),
            is_scrgb_jxl(bytes.data(),bytes.size())?convert_gamut(o):(o.extended_bt2020&&!o.srgb_clamp),frame,cancelled);
    require(rc==0,"Animation decode failed (codec "+std::to_string(rc)+")");
}

Result ConversionSession::convert_frames(const std::filesystem::path& output,const Options& o,bool rename_conflicts,
                                          const std::function<bool()>& cancelled,
                                          const std::function<void(const std::filesystem::path&)>& published) {
    validate(o);
    require(!output.empty(),"Empty output path");
    load(cancelled);
    if(!impl_->animated) {
        const auto result=run(output,o,nullptr,cancelled);
        if(published&&!result.skipped)published(output);
        return result;
    }

    auto frame_options=o;
    if(o.mode==Mode::preserve) {
        // Coalesced JXL/AVIF frames and blended APNG frames may need encoding
        // from linear working pixels. Retain the source transfer when possible.
        const bool pq=impl_->source_transfer=="PQ",hlg=impl_->source_transfer=="HLG";
        if(impl_->source_hdr) {
            require(o.target==Target::png||o.target==Target::jxl||o.target==Target::avif||linear_jxl(o.target),
                "This target cannot preserve the animated HDR source; use Process pixels mode");
            if(o.target==Target::jxl)
                frame_options.target=pq?Target::jxl_pq:hlg?Target::jxl_hlg:Target::jxl_linear;
            if(o.target==Target::png)frame_options.target=hlg?Target::png_hlg:Target::png_hdr;
            if(o.target==Target::avif)frame_options.target=hlg?Target::avif_hlg:Target::avif_pq;
        }
        frame_options.mode=Mode::process;
        frame_options.jpeg_transcoding=false;
    }
    Result first;
    bool wrote=false;
    unsigned index=0;
    read_frames(frame_options,[&](DecodedAnimationFrame&& frame) {
        if(cancelled&&cancelled())throw std::runtime_error("Animation conversion cancelled");
        const auto number=std::to_string(++index);
        auto stem=output.stem();
        stem+="_frame_"+std::string(number.size()<6?6-number.size():0,'0')+number;
        const auto path_for=[&](unsigned suffix) {
            auto name=stem;
            if(suffix>1)name+="_"+std::to_string(suffix);
            name+=output.extension();
            return output.parent_path()/name;
        };
        auto path=path_for(1);
        if(rename_conflicts) {
            unsigned suffix=2;
            while(std::filesystem::exists(std::filesystem::symlink_status(path)))path=path_for(suffix++);
        }
        const auto result=run(path,frame_options,nullptr,cancelled,nullptr,&frame);
        if(!wrote)first=result;
        if(!result.skipped) {
            wrote=true;
            if(published)published(path);
        }
    },cancelled);
    require(index!=0,"Animation contains no image frames");
    return first;
}

AnimationFrame AnimationImageRenderer::render(const std::filesystem::path& input,const Options& options,
                                      const AnimationEncoding& encoding,const AnimationPlacement& placement,
                                      const std::function<bool()>& cancelled) {
    ConversionSession session(input);
    return render_frame(session,options,encoding,placement,cancelled,nullptr);
}

void AnimationImageRenderer::render_frames(const std::filesystem::path& input,const Options& options,
                                           const AnimationEncoding& encoding,const AnimationPlacement& placement,
                                           const std::function<void(AnimationFrame&&)>& consume,
                                           const std::function<bool()>& cancelled) {
    ConversionSession session(input);
    session.load(cancelled);
    if(!session.impl_->animated) {
        consume(render_frame(session,options,encoding,placement,cancelled,nullptr));
        return;
    }
    session.read_frames(options,[&](DecodedAnimationFrame&& frame) {
        consume(render_frame(session,options,encoding,placement,cancelled,&frame));
    },cancelled);
}

AnimationFrame AnimationImageRenderer::render_frame(ConversionSession& session,const Options& options,
                                      const AnimationEncoding& encoding,const AnimationPlacement& placement,
                                      const std::function<bool()>& cancelled,DecodedAnimationFrame* decoded) {
    auto o=options;
    o.mode=Mode::process;
    const bool sdr=encoding.transfer==AnimationTransfer::sdr;
    o.target=sdr?Target::png:Target::png_hdr;
    o.keep_exif=false;
    o.jpeg_transcoding=false;
    // The animation's shared gamut is applied after HDR tone mapping.
    o.srgb_clamp=false;
    AnimationFrame frame;
    const bool direct=sdr&&(encoding.primaries==1||encoding.primaries==9)&&
        (encoding.video||encoding.format==AnimationFormat::gif||encoding.format==AnimationFormat::webp)&&
        placement.stack==AnimationStack::off&&placement.resize==AnimationResize::off;
    session.run({},o,&frame,cancelled,direct?&encoding:nullptr,decoded);
    return frame;
}
Result ConversionSession::run(const std::filesystem::path& output,const Options& o,AnimationFrame* frame,
                              const std::function<bool()>& cancelled,const AnimationEncoding* direct_frame,DecodedAnimationFrame* decoded) {
    auto& cache=*impl_;const auto& input=cache.input;

    JxrcThreadBudget thread_budget(o.threads);
    validate(o);require(frame||!output.empty(),"Empty output path");
    std::error_code ec;
    require(frame||!std::filesystem::equivalent(input,output,ec),"Input and output refer to the same file");
    if(!frame&&!o.overwrite && std::filesystem::exists(std::filesystem::symlink_status(output))) {
        Result skipped;skipped.skipped=true;return skipped;
    }
    load(cancelled);
    const auto& bytes=cache.bytes;
    const int format=decoded?6:cache.format;
    if(decoded)cache.ready=false;
    std::optional<Staging> stage;
    if(!frame)stage.emplace(output);
    const auto path=stage?stage->file.u8string():std::string{};
    StillEncodeQuality q(static_cast<int>(o.quality.preset),o.quality.jxl_distance,o.quality.codec_quality,o.quality.avif_depth,o.quality.downsize_gainmap);
    q.transparency_background_white=white_background(o);q.remove_alpha=remove_alpha(o);
    q.png_bits=o.quality.png_depth;q.png_auto_bits=o.quality.auto_bit_depth;
    q.gain_map_reconstruction=static_cast<int>(o.gain_map_reconstruction);
    const bool gainmap=!decoded&&cache.gainmap;
    const bool source_hdr=decoded?(cache.source_hdr||decoded->image.hdr()):cache.source_hdr;
    const bool full_gainmap=gainmap&&o.gain_map_reconstruction==GainMapReconstruction::full_image;
    if(full_gainmap)require(o.mode==Mode::process,"Full gain-map reconstruction requires Process pixels mode");
    const bool sdr_target=o.target==Target::jpeg||o.target==Target::jxl||o.target==Target::avif||o.target==Target::png||o.target==Target::webp;
    const bool base_only=gainmap&&!full_gainmap&&sdr_target;
    const bool mpcvr=o.sdr_tone_mapper==SdrToneMapper::mpcvr;
    const bool tonal_controls=!mpcvr&&(o.sdr_tone_mapper==SdrToneMapper::skiv_manual||!skiv_tone_mapper(o.sdr_tone_mapper));
    const std::array<float,3> zones=tonal_controls?std::array<float,3>{
        o.sdr_shadows,o.sdr_auto_tone_curve?0.f:o.sdr_midtones,o.sdr_highlights}:std::array<float,3>{};
    const bool tonal_edits=o.sdr_tone_map&&zones!=std::array<float,3>{};
    if(gainmap&&!full_gainmap) {
        const bool source_target=sdr_target||o.target==Target::jpeg_gainmap||o.target==Target::avif_gainmap;
        // Off/Auto can edit the stored SDR base, but rebuilding a gain map
        // or producing an HDR target requires the complete reconstruction.
        require(source_target&&(base_only||(!o.exposure&&!o.max_edge&&!o.srgb_clamp)),
            "Select Full gain-map reconstruction for HDR output or pixel processing that retains the gain map");
    }
    Result result;
    std::optional<RegularImage> raster_storage;
    auto* regular=decoded?&decoded->image:nullptr;
    const auto load_regular=[&] {
        if(regular)return;
        raster_storage.emplace(format==0?decode_jpeg(bytes.data(),bytes.size()):decode_regular(bytes.data(),bytes.size()));
        regular=&*raster_storage;
    };
    // Match each source to the shared canvas independently. HDR elsewhere in
    // an SDR animation does not require linearizing a compatible SDR raster.
    if(direct_frame&&!o.exposure&&!tonal_edits&&!source_hdr&&!gainmap&&(format==0||format==6)) {
        load_regular();
        const int orient=int(regular->orientation);
        const auto w=orient>=5?regular->height:regular->width;
        const auto h=orient>=5?regular->width:regular->height;
        if(orient>=1&&orient<=8&&w<=direct_frame->width&&h<=direct_frame->height&&
           (!o.max_edge||std::max(w,h)<=o.max_edge)&&
           prepare_animation_rgb8(*regular,direct_frame->primaries,direct_frame->video||remove_alpha(o))) {
            orient_image(regular->pixels,regular->width,regular->height,orient);
            frame->width=w;frame->height=h;
            frame->primaries=direct_frame->primaries;
            frame->rgba8=std::move(regular->pixels);
            return result;
        }
    }
    bool direct_sdr=false;
    if(!frame && o.mode==Mode::process && sdr_target && !o.exposure && !o.max_edge && !o.srgb_clamp && !tonal_edits) {
        if(format==6&&!source_hdr) {
            load_regular();
            direct_sdr=!regular->hdr()&&regular->floating_pixels.empty();
        } else direct_sdr=!decoded&&cache.integer_sdr;
    }
    if(direct_sdr && format==1 && o.target==Target::avif && cache.bits>q.avif_depth())direct_sdr=false;
    // Off/Auto export the stored base to SDR/source targets, retaining an
    // existing map in JPEG/AVIF. Full renders HDR before encoding any target.
    const bool direct_gainmap=gainmap && !full_gainmap && !o.exposure && !o.max_edge && !o.srgb_clamp &&
        !(base_only&&tonal_edits);
    const bool direct_jxl=format==3 && (linear_jxl(o.target) || (o.mode==Mode::preserve && o.target==Target::jxl)) &&
        !o.exposure && !o.max_edge && (!convert_gamut(o) || o.mode==Mode::preserve) && is_scrgb_jxl(bytes.data(),bytes.size());
    if(o.mode==Mode::preserve && linear_jxl(o.target))require(format==4||direct_jxl,"Preserved linear HDR JXL requires a scRGB JXR source");
    if(o.target==Target::webp && o.mode==Mode::preserve && source_hdr && !base_only)
        throw std::runtime_error("WebP is SDR; use Process pixels mode for HDR input");
    if(o.mode==Mode::preserve && format==4)require(o.target==Target::jxl||linear_jxl(o.target),"JXR preservation requires linear JXL output");
    bool source_done=false;
    if(!frame&&(direct_gainmap || direct_sdr || direct_jxl || (o.mode==Mode::preserve && format!=4&&!full_gainmap))) {
        require(format==0||format==1||format==3||format==6||format==7,"Source preservation supports JPEG, AVIF, HEIC, JXL and regular raster images; use --mode process for RAW/JXR");
        int info[5]{};const int target=o.target==Target::jpeg||o.target==Target::jpeg_gainmap?0:(o.target==Target::jxl||direct_jxl)?1:o.target==Target::png?3:o.target==Target::webp?kWebpSourceTarget:2;
        int rc=0;
        if(o.jpeg_transcoding && format==0&&!gainmap) {
            require(is_uhdr_image(const_cast<uint8_t*>(bytes.data()),static_cast<int>(bytes.size()))!=1,"JPEG coefficient transcoding cannot retain a gain map");
            const auto parser=decode_jpeg(bytes.data(),bytes.size(),true);
            require(!parser.cmyk,"CMYK JPEG coefficient transcoding is unsupported; disable Lossless JPEG transcoding to preserve CMYK samples in JXL");
            info[0]=parser.width;info[1]=parser.height;info[3]=13;
            if(parser.orientation>=5)std::swap(info[0],info[1]);
            rc=transcode_jpeg(bytes.data(),bytes.size(),path.c_str(),o.keep_exif);
            require(rc==0,"JPEG coefficient transcoding failed (codec "+std::to_string(rc)+")");
            rc=1;
        } else {
            if(format==6) {
                load_regular();
                if(o.target==Target::webp)require(!regular->hdr(),"WebP is SDR; use Process pixels mode for HDR input");
                require(regular->floating_pixels.empty(),"Floating-point HDR input requires Process pixels mode");
                if(o.keep_exif&&regular->format=="TIFF")regular->exif=portable_source_exif(bytes.data(),bytes.size(),regular->width,regular->height,regular->orientation);
                rc=convert_regular_source(*regular,target,path.c_str(),q,o.keep_exif,info);
            } else rc=convert_source(bytes.data(),bytes.size(),format,target,path.c_str(),q,o.keep_exif,info);
            // Try the integer/source path first: opaque images need no color
            // processing. Only actual transparency requires linear compositing.
            const bool flatten=rc==-97&&q.remove_alpha;
            if(flatten&&gainmap&&!base_only)throw std::runtime_error("Select Full gain-map reconstruction to remove alpha from a transparent gain-map source");
            const bool webp_render=o.target==Target::webp&&o.mode==Mode::process;
            if(rc==0&&o.target==Target::webp&&!webp_render)
                throw std::runtime_error("WebP cannot preserve this source color encoding; use Process pixels mode");
            require(rc>0 || (rc==0&&(direct_sdr||webp_render)) || flatten,"Source conversion unavailable or failed (codec "+std::to_string(rc)+"); no pixel-processing fallback was used");
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
        auto& linear=cache.linear;auto& sdr_base=cache.sdr_base;auto& exif=cache.exif;
        auto& w=cache.w;auto& h=cache.h;auto& gamut=cache.gamut;
        if(!cache.ready || processing_key(cache.previous,format)!=processing_key(o,format)) {
            cache.ready=false;cache.scrgb=false;linear.clear();sdr_base.clear();cache.mapped_sdr.clear();cache.mapped_shoulder=cache.mapped_range=-1.f;exif.clear();w=h=0;gamut=UHDR_CG_BT_709;
            bool hdr=false;int gain=0,transfer=0,rc=0;unsigned raster_orientation=1;
            if(base_only)rc=decode_gain_map_base_f32(bytes.data(),bytes.size(),format,linear,w,h,&gamut,o.keep_exif?&exif:nullptr);
            else if(format==0) {
                if(regular) {
                    w=regular->width;h=regular->height;
                    regular_to_linear(*regular,linear,gamut);
                } else jpeg_linear(bytes,linear,w,h,gamut,hdr,0,&sdr_base);
            }
            else if(format==1)rc=decode_avif_to_linear_f32(bytes.data(),bytes.size(),linear,w,h,&gain,&transfer,&gamut,o.keep_exif?&exif:nullptr,&sdr_base);
            else if(format==7)rc=decode_heif_to_linear_f32(bytes.data(),bytes.size(),linear,w,h,&gamut,o.keep_exif?&exif:nullptr,&sdr_base);
            else if(format==3)rc=decode_jxl_to_linear_f32(bytes.data(),bytes.size(),linear,w,h,&gamut,&hdr,&transfer,(is_scrgb_jxl(bytes.data(),bytes.size())?convert_gamut(o):(o.extended_bt2020&&!o.srgb_clamp)),o.keep_exif?&exif:nullptr);
            else if(format==4) {
                if((linear_jxl(o.target) || (o.mode==Mode::preserve && o.target==Target::jxl)) && !o.exposure && !o.max_edge && (!convert_gamut(o) || o.mode==Mode::preserve))
                    cache.scrgb=decode_jxr_scrgb(bytes.data(),bytes.size(),linear,w,h,cache.half_jxr);
                require(o.mode!=Mode::preserve || cache.scrgb,"JXR source is not preservable scRGB; use --mode process");
                if(!cache.scrgb)rc=decode_jxr_f32(bytes.data(),bytes.size(),linear,w,h,&gamut,convert_gamut(o));
            }
            else if(format==6) {
                load_regular();
                auto& raster=*regular;
                w=raster.width;h=raster.height;raster_orientation=raster.orientation;cache.bits=raster.bits;
                if(!decoded)cache.source_hdr=raster.hdr();
                if(o.keep_exif)exif=raster.format=="TIFF"?portable_source_exif(bytes.data(),bytes.size(),w,h,raster.orientation):raster.exif;
                if(decoded&&decoded->linear_gamut) {
                    linear=std::move(raster.floating_pixels);gamut=*decoded->linear_gamut;
                } else regular_to_linear(raster,linear,gamut);
            }
            else {
                int cg=0;const bool previous=paired_sdr;paired_sdr=gain_target(o.target);
                struct Reset{bool value;~Reset(){paired_sdr=value;}} reset{previous};
                rc=decode_raw_f32(bytes.data(),bytes.size(),o.raw,hdr_target(o.target),o.max_edge,linear,sdr_base,w,h,cg);
                gamut=cg==2?UHDR_CG_BT_2100:cg==1?UHDR_CG_DISPLAY_P3:UHDR_CG_BT_709;
            }
            require(rc==0&&w&&h&&fits_working_image(w,h)&&
                    linear.size()==size_t(w)*h*4,
                    "Image decode failed (codec "+std::to_string(rc)+")");
            if(o.keep_exif&&(format==4||format==5))exif=portable_source_exif(bytes.data(),bytes.size(),w,h);
            const int orient=format==7?1:format==6?int(raster_orientation):source_orientation(bytes.data(),bytes.size(),format,format==0&&o.keep_exif?&exif:nullptr);
            require(orient>=1&&orient<=8,"Unsupported image orientation/crop");
            if(orient!=1) {
                auto baseWidth=w,baseHeight=h;
                orient_image(linear,w,h,orient);
                orient_image(sdr_base,baseWidth,baseHeight,orient);
            }
            normalize_exif(exif);
            // RAW exposure belongs before its camera look, tone controls and
            // HDR boost; applying the global EV here would edit the render again.
            if(format!=5&&o.exposure) {
                const float multiplier=std::exp2(o.exposure);
                for(auto* values:{&linear,&sdr_base})for(size_t i=0;i<values->size();++i)if(i%4!=3)(*values)[i]*=multiplier;
            }
            if(format!=5&&o.max_edge) {
                unsigned sw=w,sh=h;if(!sdr_base.empty())scale_rgba_f32_max_edge(sdr_base,sw,sh,o.max_edge);
                scale_rgba_f32_max_edge(linear,w,h,o.max_edge);
            }
            if(o.srgb_clamp&&!cache.scrgb) {
                // Rematrix both HDR and paired SDR pixels before encoding.
                // Keep HDR headroom above 1; SDR encoders apply their normal
                // [0,1] range. Untouched scRGB retains its direct signed path.
                auto clampGamut=[&](std::vector<float>& pixels) {
                    for(size_t i=0;i<pixels.size();i+=4) {
                        auto& r=pixels[i];auto& g=pixels[i+1];auto& b=pixels[i+2];
                        if(gamut==UHDR_CG_BT_2100)linear_bt2020_to_bt709(r,g,b);
                        else if(gamut==UHDR_CG_DISPLAY_P3)linear_p3_to_bt709(r,g,b);
                        for(unsigned c=0;c<3;++c)if(!std::isfinite(pixels[i+c])||pixels[i+c]<0.f)pixels[i+c]=0.f;
                    }
                };
                clampGamut(linear);clampGamut(sdr_base);gamut=UHDR_CG_BT_709;
            }
            if(remove_alpha(o)) {
                // Preserved scRGB JXR uses 80 nits per unit; other working
                // buffers use 203. Background white always represents 203 nits.
                const float background=white_background(o)?(cache.scrgb?kSdrWhiteNits/80.f:1.f):0.f;
                auto flatten=[&](std::vector<float>& pixels) {
                    for(size_t i=0;i<pixels.size();i+=4) {
                        const float a=std::isfinite(pixels[i+3])?std::clamp(pixels[i+3],0.f,1.f):0.f;
                        for(unsigned c=0;c<3;++c)pixels[i+c]=(a>0.f?pixels[i+c]*a:0.f)+background*(1.f-a);
                        pixels[i+3]=1.f;
                    }
                };
                flatten(linear);flatten(sdr_base);
            }
            cache.analysis.bind(linear.data(),size_t(w)*h,gamut);
            cache.previous=o;cache.ready=true;
        }
        auto output_exif=exif;prepare_export_exif(output_exif,w,h);
        q.exif=output_exif.data();q.exif_size=output_exif.size();
        // A reconstructed base + gain map has F32 precision, regardless of
        // either encoded plane's depth. Quantize it at the selected export depth.
        q.source_bits=full_gainmap?32:cache.bits;
        q.analysis=&cache.analysis;
        const bool gain_output=gain_target(o.target);
        if(o.sdr_tone_map&&(sdr_target||gain_output)&&!cache.scrgb) {
            const float peak=cache.analysis.peak();
            if(mpcvr&&peak>1.f) {
                if(cache.mapped_sdr.empty()||cache.mapped_mapper!=o.sdr_tone_mapper) {
                    cache.mapped_sdr=tone_map_mpcvr_base(linear,gamut,double(peak)*kSdrWhiteNits);
                    cache.mapped_mapper=o.sdr_tone_mapper;
                }
            } else if(peak>1.f||tonal_edits) {
                // The manual range is a ceiling. Fit a dimmer source to its
                // actual peak; only values above the ceiling are clipped.
                const float range=o.sdr_auto_hdr_range?peak*kSdrWhiteNits:
                    std::min(peak*kSdrWhiteNits,float(o.sdr_hdr_range_nits));
                if(cache.mapped_sdr.empty()||cache.mapped_shoulder!=o.sdr_shoulder||cache.mapped_range!=range||cache.mapped_mapper!=o.sdr_tone_mapper||cache.mapped_color_mapping!=o.sdr_color_mapping||cache.mapped_zones!=zones||cache.mapped_auto_range!=o.sdr_auto_hdr_range||cache.mapped_cap!=o.sdr_hdr_range_nits||cache.mapped_auto_curve!=o.sdr_auto_tone_curve) {
                    const int cg=gamut==UHDR_CG_BT_2100?2:gamut==UHDR_CG_DISPLAY_P3?1:0;
                    cache.mapped_sdr=map_sdr_pixels(linear,cg,range,o);
                    cache.mapped_shoulder=o.sdr_shoulder;cache.mapped_range=range;cache.mapped_mapper=o.sdr_tone_mapper;cache.mapped_color_mapping=o.sdr_color_mapping;cache.mapped_zones=zones;cache.mapped_auto_range=o.sdr_auto_hdr_range;cache.mapped_cap=o.sdr_hdr_range_nits;cache.mapped_auto_curve=o.sdr_auto_tone_curve;
                }
            } else cache.mapped_sdr.clear();
        }
        if(frame) {
            frame->width=w;frame->height=h;
            frame->primaries=gamut==UHDR_CG_BT_2100?9:gamut==UHDR_CG_DISPLAY_P3?12:1;
            frame->rgba=std::move(sdr_target&&!cache.mapped_sdr.empty()?cache.mapped_sdr:linear);
            return result;
        }
        const float* export_pixels=sdr_target&&o.sdr_tone_map&&!cache.mapped_sdr.empty()?cache.mapped_sdr.data():linear.data();
        const float* export_base=gain_output&&o.sdr_tone_map&&!cache.mapped_sdr.empty()?cache.mapped_sdr.data():sdr_base.empty()?nullptr:sdr_base.data();
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
        } else if(o.target==Target::png||o.target==Target::png_hdr||o.target==Target::png_hlg) {
            const int transfer=o.target==Target::png_hlg?18:o.target==Target::png_hdr?16:13;
            rc=encode_png_f32(w,h,o.target==Target::png?export_pixels:linear.data(),gamut,transfer,q.png_depth(q.source_bits),path.c_str(),q);
        } else if(o.target==Target::webp) {
            rc=encode_webp_f32(w,h,export_pixels,gamut,path.c_str(),q);
        } else if(o.target==Target::jxl_gainmap) {
            rc=encode_jxl_gain_map(w,h,linear.data(),gamut,path.c_str(),q,export_base);
        } else if(o.target==Target::jxl_hlg) {
            rc=encode_jxl_f32(w,h,linear.data(),gamut,true,path.c_str(),q,false,true);
        } else if(o.target==Target::avif_hlg) {
            rc=encode_avif_f32(w,h,linear.data(),gamut,3,path.c_str(),q);
        } else rc=encode_still_export_f32(sdr_target?export_pixels:linear.data(),w,h,cg,static_cast<int>(o.target),path.c_str(),q,export_base);
        require(rc==0,"Image encode failed (codec "+std::to_string(rc)+")");
        result.width=w;result.height=h;result.gainmap=gain_output;
        result.transfer=hlg_target(o.target)?18:o.target==Target::avif_pq||o.target==Target::jxl_pq||o.target==Target::png_hdr?16:0;
        result.encoding=result.gainmap?"gainmap":result.transfer==18?"hlg":result.transfer==16?"pq":(linear_jxl(o.target)||cache.scrgb)?"linear":"sdr";
    }
    require(std::filesystem::file_size(stage->file)>0,"Encoder produced no output");stage->publish(output,o.overwrite);return result;
}
Preview preview(const std::filesystem::path& input,unsigned edge) {
    return preview(input,edge,{});
}
Preview preview(const std::filesystem::path& input,unsigned edge,const std::function<bool()>& cancelled) {
    require(edge>0&&edge<=1024,"Preview edge must be 1..1024");
    return preview_bytes(read(input,cancelled),edge,nullptr,cancelled);
}
Preview preview_bytes(const std::vector<uint8_t>& bytes,unsigned edge,ImageInfo* source_info,const std::function<bool()>& cancelled) {
    require(edge>0&&edge<=1024,"Preview edge must be 1..1024");
    require(!bytes.empty()&&bytes.size()<=512u*1024*1024,"Input must be 1 byte to 512 MiB");
    if(cancelled&&cancelled())throw std::runtime_error("Preview cancelled");
    edge=std::min(edge,768u);
    JxrcThreadBudget budget(1);
    const int format=kind(bytes);
    if(source_info) {
        *source_info={};
        // Regular rasters supply this from the actual decode below. Other
        // codecs have header inspectors that reuse the same in-memory bytes.
        if(format!=6)try{*source_info=inspect_bytes(bytes);}catch(const std::exception&){/* Metadata failure must not suppress a usable preview. */}
    }
    if(cancelled&&cancelled())throw std::runtime_error("Preview cancelled");
    std::vector<uint16_t> linear;std::vector<uint8_t> raw_pixels;
    unsigned w=0,h=0,raster_orientation=1;uhdr_color_gamut_t gamut=UHDR_CG_BT_709;bool hdr=false;int gain=0,transfer=0,rc=0;
    if(format==0)jpeg_linear(bytes,linear,w,h,gamut,hdr,edge);
    else if(format==7)rc=decode_heif_to_linear_f16(bytes.data(),bytes.size(),linear,w,h,&gamut,edge);
    else if(format==1)rc=decode_avif_to_linear_f16(bytes.data(),bytes.size(),linear,w,h,&gain,&transfer,&gamut,edge,nullptr);
    else if(format==3)rc=decode_jxl_to_linear_f16(bytes.data(),bytes.size(),linear,w,h,&gamut,&hdr,&transfer,false,edge,false,nullptr,nullptr);
    else if(format==4)rc=decode_jxr_memory(bytes.data(),bytes.size(),linear,w,h,&gamut,std::min(edge,768u));
    else if(format==6) {
        DecodedAnimationFrame first;
        const bool animated=decode_raster_animation(bytes.data(),bytes.size(),
            [&](DecodedAnimationFrame&& frame){first=std::move(frame);},true,cancelled);
        auto raster=animated?std::move(first.image):decode_regular(bytes.data(),bytes.size());
        if(source_info) {
            *source_info=inspect_regular(raster);
            if(animated) {
                if(raster.format=="GIF") {
                    try {source_info->animated=inspect_gif(bytes.data(),bytes.size()).animated;}
                    catch(const std::exception&) {/* Keep a usable first frame. */}
                } else source_info->animated=true;
            }
        }
        w=raster.width;h=raster.height;raster_orientation=raster.orientation;
        if(!raster.floating_pixels.empty()) {
            auto reduced=reduce_preview(raster.floating_pixels.data(),w,h,size_t(w)*4,4,edge);
            if(!reduced.empty())raster.floating_pixels=std::move(reduced);
        } else if(raster.bits>8) {
            const auto preview_pixels=reduce_preview(reinterpret_cast<const uint16_t*>(raster.pixels.data()),w,h,size_t(w)*4,4,edge);
            if(!preview_pixels.empty()){std::vector<uint8_t> reduced(preview_pixels.size()*2);std::memcpy(reduced.data(),preview_pixels.data(),reduced.size());raster.pixels=std::move(reduced);}
        } else {
            auto preview_pixels=reduce_preview(raster.pixels.data(),w,h,size_t(w)*4,4,edge);
            if(!preview_pixels.empty())raster.pixels=std::move(preview_pixels);
        }
        raster.width=w;raster.height=h;regular_to_linear(raster,linear,gamut);
    }
    else {
        std::vector<uint8_t> jpeg;bool embedded=false;
        if(raw_jpeg_thumbnail(bytes.data(),bytes.size(),jpeg)) {
            try {
                jpeg_linear(jpeg,linear,w,h,gamut,hdr,edge);
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
    if(cancelled&&cancelled())throw std::runtime_error("Preview cancelled");
    scale_rgba_f16_max_edge(linear,w,h,edge);
    const int orient=format==7?1:(format==6||format==5)?int(raster_orientation):source_orientation(bytes.data(),bytes.size(),format,nullptr);
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
