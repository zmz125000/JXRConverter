#include "jxrc/converter.hpp"
#include "codec_api.h"
#include "render_analysis.h"
#include "input_format.h"
#include "hdr_parallel.h"
#include "regular_decode.h"
#include "exif_metadata.h"
#include <exiv2/exiv2.hpp>
namespace ultrahdr { unsigned int GetCPUCoreCount(); }
void gain_map_f32_tests(const std::filesystem::path&);
void hdr_input_tests(const std::filesystem::path&);
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

#include "regular_fixtures.hpp"
#include "jpeg_fixtures.hpp"
#include "heic_fixtures.hpp"
#include "heif_decode.h"
#include "exif_jxr_fixture.hpp"

namespace fs=std::filesystem;
static void check(bool ok,const char* text) {if(!ok)throw std::runtime_error(text);}
static std::vector<uint8_t> bytes(const fs::path& path) {
    std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};
}
static void write(const fs::path& path,const std::vector<uint8_t>& data) {
    std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(data.data()),data.size());
}
static std::vector<uint16_t> fixture(float peak=6) {
    std::vector<uint16_t> p(64*48*4);
    for(size_t i=0;i<p.size()/4;++i) {
        p[4*i]=float_to_half(peak*(i%64)/63.f);
        p[4*i+1]=float_to_half(peak*(i/64)/47.f);
        p[4*i+2]=float_to_half(peak*0.2f);
        p[4*i+3]=float_to_half(1);
    }return p;
}
static void encode(const fs::path& path,int mode,bool hdr=true) {
    auto p=fixture(hdr?6:1);
    check(encode_still_export(reinterpret_cast<uint8_t*>(p.data()),p.size()*2,64,48,true,2,mode,false,path.string().c_str(),0,StillEncodeQuality(0),false)==0,"Fixture encode");
}
template<class F>static void fails(F f) {bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"Expected rejection");}
// Uncompressed, single-strip Bayer DNG with explicit sensor calibration.
static void dng(const fs::path& path,float scale=1.f) {
    struct Tag {uint16_t id,type;uint32_t count;std::vector<uint8_t> payload;};std::vector<Tag> tags;
    auto u16=[](uint16_t v){return std::vector<uint8_t>{uint8_t(v),uint8_t(v>>8)};};
    auto u32=[](uint32_t v){return std::vector<uint8_t>{uint8_t(v),uint8_t(v>>8),uint8_t(v>>16),uint8_t(v>>24)};};
    auto num=[&](int id,int type,uint32_t value){tags.push_back({uint16_t(id),uint16_t(type),1,type==3?u16(value):u32(value)});};
    auto str=[&](int id,std::string s){s.push_back(0);tags.push_back({uint16_t(id),2,uint32_t(s.size()),{s.begin(),s.end()}});};
    num(256,4,64);num(257,4,64);num(258,3,16);num(259,3,1);num(262,3,32803);
    str(271,"JXRC");str(272,"Test Camera");num(273,4,0);num(277,3,1);num(278,4,64);num(279,4,8192);num(284,3,1);
    tags.push_back({33421,3,2,{2,0,2,0}});tags.push_back({33422,1,4,{0,1,1,2}});
    tags.push_back({50706,1,4,{1,4,0,0}});tags.push_back({50707,1,4,{1,1,0,0}});str(50708,"JXRC Test Camera");
    tags.push_back({50710,1,3,{0,1,2}});num(50711,3,1);num(50714,4,64);num(50717,4,16383);
    std::vector<uint8_t> matrix,neutral;
    for(int i=0;i<9;++i){auto n=u32(i%4==0?1:0);auto d=u32(1);matrix.insert(matrix.end(),n.begin(),n.end());matrix.insert(matrix.end(),d.begin(),d.end());}
    for(int i=0;i<3;++i){auto n=u32(1);neutral.insert(neutral.end(),n.begin(),n.end());neutral.insert(neutral.end(),n.begin(),n.end());}
    tags.push_back({50721,10,9,matrix});tags.push_back({50728,5,3,neutral});num(50778,3,21);
    std::sort(tags.begin(),tags.end(),[](auto& a,auto& b){return a.id<b.id;});
    std::vector<uint8_t> out={'I','I',42,0,8,0,0,0};auto count=u16(tags.size());out.insert(out.end(),count.begin(),count.end());
    const size_t end=10+tags.size()*12+4;out.resize(end);std::vector<uint8_t> extra;
    for(size_t i=0;i<tags.size();++i) {
        auto& t=tags[i];size_t at=10+i*12;
        auto id=u16(t.id),type=u16(t.type),n=u32(t.count);std::copy(id.begin(),id.end(),out.begin()+at);std::copy(type.begin(),type.end(),out.begin()+at+2);std::copy(n.begin(),n.end(),out.begin()+at+4);
        if(t.payload.size()<=4)std::copy(t.payload.begin(),t.payload.end(),out.begin()+at+8);
        else {auto offset=u32(end+extra.size());std::copy(offset.begin(),offset.end(),out.begin()+at+8);extra.insert(extra.end(),t.payload.begin(),t.payload.end());if(extra.size()%2)extra.push_back(0);}
    }
    const auto offset=u32(end+extra.size());
    for(size_t i=0;i<tags.size();++i)if(tags[i].id==273)std::copy(offset.begin(),offset.end(),out.begin()+10+i*12+8);
    out.insert(out.end(),extra.begin(),extra.end());
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){auto v=u16(static_cast<uint16_t>(std::lround(std::clamp(64.f+(64.f+(x+y)*100)*scale,0.f,65535.f))));out.insert(out.end(),v.begin(),v.end());}
    write(path,out);
}
// Meter linear RAW before rendering, preserving manual compensation and cache isolation.
static void raw_auto_exposure(const fs::path& dir) {
    const auto source=dir/"auto-exposure.dng";dng(source);const auto data=bytes(source);
    jxrc::RawControls controls;controls.hdr_mode=jxrc::RawHdrMode::custom;controls.linear_hdr=true;controls.shoulder=0;
    unsigned w=0,h=0;int gamut=0;std::vector<float> unused;
    auto decode=[&](const std::vector<uint8_t>& input){std::vector<float> out;check(decode_raw_f32(input.data(),input.size(),controls,true,0,out,unused,w,h,gamut)==0,"Auto exposure RAW decode");return out;};
    auto meanLog=[](const std::vector<float>& pixels){std::vector<float> logs;for(size_t i=0;i<pixels.size();i+=4){const float y=.2627f*pixels[i]+.6780f*pixels[i+1]+.0593f*pixels[i+2];if(y>0.f)logs.push_back(std::log2(y));}std::sort(logs.begin(),logs.end());const size_t trim=logs.size()/100;double sum=0;for(size_t i=trim;i<logs.size()-trim;++i)sum+=logs[i];return static_cast<float>(sum/(logs.size()-2*trim));};
    check(!controls.auto_exposure,"RAW auto exposure must default off");
    const auto original=decode(data);controls.auto_exposure=true;const auto automatic=decode(data);
    check(std::abs(std::exp2(meanLog(automatic))-.18f)<.0001f,"RAW auto exposure did not meter scene-linear 18% gray");
    controls.exposure=1;const auto compensation=decode(data);
    for(size_t i=0;i<automatic.size();++i)check(std::abs(compensation[i]-automatic[i]*(i%4==3?1.f:2.f))<.0001f,"RAW auto exposure cancelled or doubled manual compensation");
    controls.exposure=0;const auto dim=dir/"auto-dim.dng";dng(dim,.25f);const auto dimAuto=decode(bytes(dim));
    check(std::abs(std::exp2(meanLog(dimAuto))-.18f)<.0001f,"RAW auto exposure failed for dim input");
    // A dark background must not drive an otherwise exposed bright subject into clipping.
    auto contrast=data;
    const size_t sensor=contrast.size()-8192;
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
        const uint16_t value=x>=40?10000:80;
        const size_t offset=sensor+(y*64+x)*2;
        contrast[offset]=uint8_t(value);contrast[offset+1]=uint8_t(value>>8);
    }
    controls.auto_exposure=false;const auto contrastOff=decode(contrast);
    controls.auto_exposure=true;const auto contrastAuto=decode(contrast);
    std::vector<float> peaks;
    for(size_t i=0;i<contrastOff.size();i+=4)peaks.push_back(std::max({contrastOff[i],contrastOff[i+1],contrastOff[i+2]}));
    std::sort(peaks.begin(),peaks.end());const float highlight=peaks[(peaks.size()-1)*99/100];
    const float lift=std::exp2(meanLog(contrastAuto)-meanLog(contrastOff));
    check(highlight>.5f,"RAW highlight fixture has no exposed subject");
    check(lift<=std::max(1.f,.95f/highlight)+.001f,"RAW auto exposure overexposed highlights against a dark background");
    controls.auto_exposure_protect_highlights=false;
    const auto unguarded=decode(contrast);
    check(meanLog(unguarded)>meanLog(contrastAuto)+.1f,"Disabling RAW AE highlight protection did not restore average-only metering");
    controls.auto_exposure_protect_highlights=true;
    // Equivalent manual EV must match when both run before the nonlinear look.
    const float meteredEV=std::clamp(std::log2(.18f)-meanLog(original),-6.f,6.f);
    controls.linear_hdr=false;const auto cameraAuto=decode(data);controls.auto_exposure=false;controls.exposure=meteredEV;
    const auto cameraManual=decode(data);
    for(size_t i=0;i<cameraAuto.size();++i)check(std::abs(cameraAuto[i]-cameraManual[i])<.0001f,"Auto exposure ran after the camera look");
    controls={};controls.auto_exposure=true;
    std::vector<uint8_t> half;check(decode_raw(data.data(),data.size(),controls,true,0,half,w,h,gamut)==0,"RAW auto exposure half path");
    jxrc::Options options;options.mode=jxrc::Mode::process;options.target=jxrc::Target::jxl_linear;options.quality.preset=jxrc::Preset::lossless;
    options.raw.linear_hdr=true;options.raw.shoulder=0;jxrc::ConversionSession session(source);
    const auto before=dir/"auto-off.jxl",reused=dir/"auto-reused.jxl",fresh=dir/"auto-fresh.jxl";
    session.convert(before,options);options.raw.auto_exposure=true;session.convert(reused,options);jxrc::convert(source,fresh,options);
    check(bytes(reused)==bytes(fresh)&&bytes(before)!=bytes(fresh),"RAW auto exposure did not invalidate cached pixels");
    // Global pixel EV must not re-expose any RAW render, including paired gain maps.
    for(auto target:{jxrc::Target::jpeg,jxrc::Target::jpeg_gainmap,jxrc::Target::jxl,jxrc::Target::jxl_linear,jxrc::Target::jxl_scrgb,jxrc::Target::jxl_pq,jxrc::Target::avif,jxrc::Target::avif_pq,jxrc::Target::avif_gainmap}) {
        options.target=target;options.exposure=0;
        const auto baseline=dir/("raw-ev-zero-"+std::to_string(int(target))),ignored=dir/("raw-ev-ignored-"+std::to_string(int(target))),freshEV=dir/("raw-ev-fresh-"+std::to_string(int(target)));
        session.convert(baseline,options);options.exposure=1;session.convert(ignored,options);jxrc::convert(source,freshEV,options);
        check(bytes(baseline)==bytes(ignored)&&bytes(baseline)==bytes(freshEV),"RAW output applied global pixel-processing EV");
    }
    options.target=jxrc::Target::jxl_linear;options.exposure=0;
    options.mode=jxrc::Mode::preserve;options.raw.linear_hdr=false;options.raw.shoulder=.5f;fails([&]{jxrc::validate(options);});
    const auto black=dir/"auto-black.dng";dng(black,0);controls={};controls.linear_hdr=true;controls.shoulder=0;
    const auto blackOff=decode(bytes(black));controls.auto_exposure=true;const auto blackAuto=decode(bytes(black));
    check(blackOff==blackAuto,"RAW auto exposure changed an unmeterable black image");
}

// Preserve raster samples through the public API, including low 16-bit bits and
// RGB hidden behind zero alpha. A half-float or 8-bit intermediate fails this.
static void regular_images(const fs::path& dir) {
    struct Raster { const char* name;const std::vector<uint8_t>& data;const std::vector<uint16_t>& expected;unsigned bits; };
    for(const auto& fixture:std::vector<Raster>{{"png16",raster_png16,expected_png16,16},
        {"png8",raster_png8,expected_png8,8},{"webp",raster_webp,expected_webp,8},
        {"tiff",raster_tiff,expected_tiff,8},{"bmp",raster_bmp,expected_bmp,8},
        {"gif",raster_gif,expected_gif,8},{"palette",raster_palette,expected_palette,8}}) {
        const auto input=dir/fixture.name,output=dir/(std::string(fixture.name)+".jxl");write(input,fixture.data);
        const auto source=decode_regular(fixture.data.data(),fixture.data.size());
        check(source.width==3 && source.height==2 && source.bits==fixture.bits,"Raster decoder dimensions/depth");
        const auto info=jxrc::inspect(input);check(!info.raw && info.bits==fixture.bits,"Raster inspection");
        jxrc::Options options;options.target=jxrc::Target::jxl;options.quality.preset=jxrc::Preset::lossless;
        jxrc::convert(input,output,options);
        const auto encoded=bytes(output);auto dec=JxlDecoderMake(nullptr);
        check(JxlDecoderSubscribeEvents(dec.get(),JXL_DEC_BASIC_INFO|JXL_DEC_FULL_IMAGE)==JXL_DEC_SUCCESS,"Raster JXL events");
        JxlDecoderSetInput(dec.get(),encoded.data(),encoded.size());JxlDecoderCloseInput(dec.get());
        std::vector<uint8_t> pixels(24*(fixture.bits/8));
        const JxlPixelFormat format{4,fixture.bits==16?JXL_TYPE_UINT16:JXL_TYPE_UINT8,JXL_NATIVE_ENDIAN,0};bool complete=false;
        for(;;) {
            const auto status=JxlDecoderProcessInput(dec.get());
            if(status==JXL_DEC_NEED_IMAGE_OUT_BUFFER)check(JxlDecoderSetImageOutBuffer(dec.get(),&format,pixels.data(),pixels.size())==JXL_DEC_SUCCESS,"Raster JXL buffer");
            else if(status==JXL_DEC_FULL_IMAGE){complete=true;break;}
            else if(status==JXL_DEC_SUCCESS)break;
            else check(status!=JXL_DEC_ERROR && status!=JXL_DEC_NEED_MORE_INPUT,"Raster JXL decode");
        }
        check(complete,"Missing raster JXL image");
        for(size_t i=0;i<24;++i){uint16_t value=pixels[i];if(fixture.bits==16)std::memcpy(&value,pixels.data()+2*i,2);check(value==fixture.expected[i],"Preserve changed a raster sample");}
        const auto copied=dir/(std::string(fixture.name)+"-integer-copy.jxl");
        options.mode=jxrc::Mode::process;jxrc::convert(output,copied,options);
        const auto copiedBytes=bytes(copied);
        check(is_integer_jxl(copiedBytes.data(),copiedBytes.size()),"Integer JXL source became floating point");
        check(jxrc::inspect(copied).bits==fixture.bits,"Integer JXL source depth changed");
        options.mode=jxrc::Mode::process;
        const auto direct=dir/(std::string(fixture.name)+"-direct.jxl");
        jxrc::convert(input,direct,options);
        check(bytes(direct)==encoded,"Untouched SDR raster did not retain integer source samples");
        options.target=jxrc::Target::avif;options.mode=jxrc::Mode::preserve;
        const auto nativeAvif=dir/(std::string(fixture.name)+"-native.avif");
        jxrc::convert(input,nativeAvif,options);options.mode=jxrc::Mode::process;
        const auto directAvif=dir/(std::string(fixture.name)+"-direct.avif");jxrc::convert(input,directAvif,options);
        check(bytes(directAvif)==bytes(nativeAvif),"Untouched SDR to AVIF changed integer samples/tags");
        check(jxrc::inspect(directAvif).bits==(fixture.bits==8?8:12),"SDR AVIF output depth");
        options.target=jxrc::Target::jxl;
        options.max_edge=2;options.exposure=1;
        const auto processed=jxrc::convert(input,dir/(std::string(fixture.name)+"-processed.jxl"),options);
        check(processed.width==2 && processed.height==1,"Raster processing resize");
        const auto processedInfo=jxrc::inspect(dir/(std::string(fixture.name)+"-processed.jxl"));
        check(processedInfo.bits==fixture.bits && processedInfo.transfer=="SDR","Processed SDR raster JXL depth/transfer");
        const auto processedBytes=bytes(dir/(std::string(fixture.name)+"-processed.jxl"));
        check(is_integer_jxl(processedBytes.data(),processedBytes.size()),"Processed SDR raster JXL is floating point");
        const auto thumb=jxrc::preview(input,2);check(thumb.width==2 && thumb.height==1,"Raster native thumbnail");
    }
}

// Signed float32 bit patterns used to overflow libjxl's weighted predictor.
static void signed_float_jxl(const fs::path& dir) {
    constexpr unsigned w=64,h=48;
    const float values[]={-1.f,0.125123f,-0.0000123f,2.f,0.f,1.000123f,-0.25f,6.f};
    std::vector<float> input(w*h*4);
    for(size_t i=0;i<w*h;++i) {
        for(unsigned c=0;c<3;++c)input[i*4+c]=values[(i+c)%8];
        input[i*4+3]=(i%3)*0.5f;
    }
    StillSourceColor color;color.bits=32;color.exponent_bits=8;color.transfer=8;color.intensity_target=80.f;
    const auto path=dir/"signed-float32.jxl";
    const int rc=encode_jxl_source(w,h,input.data(),true,color,path.string().c_str(),StillEncodeQuality(0));
    if(rc)throw std::runtime_error("Signed float32 JXL encode: "+std::to_string(rc));
    auto encoded=bytes(path);auto dec=JxlDecoderMake(nullptr);
    check(JxlDecoderSubscribeEvents(dec.get(),JXL_DEC_BASIC_INFO|JXL_DEC_FULL_IMAGE)==JXL_DEC_SUCCESS,"Float32 JXL events");
    JxlDecoderSetInput(dec.get(),encoded.data(),encoded.size());JxlDecoderCloseInput(dec.get());
    std::vector<float> output(input.size());const JxlPixelFormat format{4,JXL_TYPE_FLOAT,JXL_NATIVE_ENDIAN,0};bool complete=false;
    for(;;) {
        const auto status=JxlDecoderProcessInput(dec.get());
        if(status==JXL_DEC_BASIC_INFO) {
            JxlBasicInfo info{};check(JxlDecoderGetBasicInfo(dec.get(),&info)==JXL_DEC_SUCCESS,"Float32 JXL basic info");
            check(info.bits_per_sample==32 && info.exponent_bits_per_sample==8,"Float32 JXL precision reduced");
        } else if(status==JXL_DEC_NEED_IMAGE_OUT_BUFFER)check(JxlDecoderSetImageOutBuffer(dec.get(),&format,output.data(),output.size()*sizeof(float))==JXL_DEC_SUCCESS,"Float32 JXL output buffer");
        else if(status==JXL_DEC_FULL_IMAGE){complete=true;break;}
        else if(status==JXL_DEC_SUCCESS)break;
        else check(status!=JXL_DEC_ERROR && status!=JXL_DEC_NEED_MORE_INPUT,"Float32 JXL decode");
    }
    check(complete && std::memcmp(input.data(),output.data(),input.size()*sizeof(float))==0,"Signed float32 JXL samples changed");
    check(is_scrgb_jxl(encoded.data(),encoded.size()),"scRGB JXL detection");
    for(auto target:{jxrc::Target::jpeg,jxrc::Target::avif}) {
        jxrc::Options preserve;preserve.target=target;
        fails([&]{jxrc::convert(path,dir/("scrgb-preserve-"+std::to_string(int(target))),preserve);});
    }
    jxrc::Options options;options.mode=jxrc::Mode::process;options.target=jxrc::Target::jxl_scrgb;options.quality.preset=jxrc::Preset::lossless;
    const auto copied=dir/"scrgb-copy.jxl";jxrc::convert(path,copied,options);
    auto copy=bytes(copied);check(is_scrgb_jxl(copy.data(),copy.size()),"Direct scRGB JXL metadata lost");
    std::vector<float> working;unsigned width=0,height=0;uhdr_color_gamut_t gamut;bool hdr=false;int transfer=0;
    check(decode_jxl_to_linear_f32(copy.data(),copy.size(),working,width,height,&gamut,&hdr,&transfer,false,nullptr)==0 && hdr,"scRGB working decode");
    for(size_t i=0;i<input.size();++i)check(std::abs(working[i]-input[i]*(i%4==3?1.f:80.f/203.f))<1e-6f,"scRGB luminance scale");
    for(auto preset:{jxrc::Preset::lossless,jxrc::Preset::high,jxrc::Preset::medium,jxrc::Preset::low,jxrc::Preset::custom}) {
        options.quality.preset=preset;options.quality.jxl_distance=.73f;
        const auto output=dir/("scrgb-preset-"+std::to_string(int(preset))+".jxl");
        jxrc::convert(path,output,options);const auto data=bytes(output);
        check(is_scrgb_jxl(data.data(),data.size()),"Preset bypassed direct scRGB path");
        const auto info=jxrc::inspect(output);
        check(info.bits==32 && info.hdr && info.transfer=="Linear" && info.primaries=="BT.709","Preset changed scRGB encoding");
    }
    options.quality.preset=jxrc::Preset::lossless;
    options.srgb_clamp=true;options.target=jxrc::Target::jxl_linear;
    const auto clampedDirect=dir/"scrgb-clamp-direct.jxl";jxrc::convert(path,clampedDirect,options);
    check(bytes(clampedDirect)==copy,"sRGB clamp altered the direct scRGB path");
    options.srgb_clamp=false;
    options.convert_gamut=false;
    check(jxrc::Options{}.convert_gamut,"scRGB gamut conversion must default on");
    for(auto target:{jxrc::Target::avif_pq,jxrc::Target::jxl_pq,jxrc::Target::jxl_linear}) {
        options.target=target;
        const auto keep=dir/("keep-primaries-"+std::to_string(int(target)));
        jxrc::convert(path,keep,options);
        check(jxrc::inspect(keep).primaries=="BT.709","scRGB output changed source primaries");
        options.convert_gamut=true;
        const auto converted=dir/("convert-primaries-"+std::to_string(int(target)));
        jxrc::convert(path,converted,options);
        check(jxrc::inspect(converted).primaries=="BT.2020","Explicit BT.2020 conversion ignored");
        options.convert_gamut=false;
    }
    options.target=jxrc::Target::jxl_scrgb;options.convert_gamut=true;
    options.exposure=1;options.extended_bt2020=false;
    const auto processed=dir/"scrgb-exposed.jxl";jxrc::convert(path,processed,options);
    auto changed=bytes(processed);check(is_scrgb_jxl(changed.data(),changed.size()),"Edited scRGB output metadata");
    check(decode_jxl_to_linear_f32(changed.data(),changed.size(),working,width,height,&gamut,&hdr,&transfer,false,nullptr)==0,"Processed scRGB decode");
    for(size_t i=0;i<input.size();++i)check(std::abs(working[i]-input[i]*(i%4==3?1.f:160.f/203.f))<1e-5f,"scRGB exposure bypassed");

    // Non-scRGB sources are rematrixed and rebased, including negative channels.
    for(int primaries:{1,9,12}) {
        const float pixels[]={1.25f,.25f,.0625f,.5f};
        StillSourceColor source;source.bits=32;source.exponent_bits=8;
        source.transfer=8;source.primaries=primaries;source.intensity_target=1000.f;
        const auto inputPath=dir/("to-scrgb-source-"+std::to_string(primaries)+".jxl");
        check(encode_jxl_source(1,1,pixels,true,source,inputPath.string().c_str(),StillEncodeQuality(0))==0,"scRGB conversion fixture");
        options={};options.mode=jxrc::Mode::process;options.target=jxrc::Target::jxl_scrgb;
        options.extended_bt2020=false;options.quality.preset=jxrc::Preset::lossless;
        const auto outputPath=dir/("to-scrgb-"+std::to_string(primaries)+".jxl");
        jxrc::ConversionSession session(inputPath);session.convert(outputPath,options);
        auto data=bytes(outputPath);check(is_scrgb_jxl(data.data(),data.size()),"Converted output is not scRGB");
        check(decode_jxl_to_linear_f32(data.data(),data.size(),working,width,height,&gamut,&hdr,&transfer,false,nullptr)==0,"Converted scRGB decode");
        float r=pixels[0],g=pixels[1],b=pixels[2];
        if(primaries==9)linear_bt2020_to_bt709(r,g,b);
        else if(primaries==12)linear_p3_to_bt709(r,g,b);
        const float expected[]={r*1000.f/203.f,g*1000.f/203.f,b*1000.f/203.f,.5f};
        for(size_t i=0;i<4;++i)check(std::abs(working[i]-expected[i])<1e-5f,"scRGB conversion changed luminance, gamut or alpha");
        options.target=jxrc::Target::jxl_pq;
        const auto queued=dir/("after-scrgb-"+std::to_string(primaries)+".jxl"),single=dir/("single-pq-"+std::to_string(primaries)+".jxl");
        session.convert(queued,options);jxrc::convert(inputPath,single,options);
        check(bytes(queued)==bytes(single),"scRGB output mutated shared pixels");
        check(jxrc::inspect(queued).primaries==(primaries==9?"BT.2020":primaries==12?"Display P3":"BT.709"),"scRGB toggle affected PQ output");
    }

}

// PQ has an absolute 10000-nit coding range, independent of content-peak metadata.
static void pq_jxl_peak(const fs::path& dir) {
    for(bool half:{false,true})for(int preset:{0,2})for(float nits:{0.f,20.f,1000.f,20000.f}) {
        const float value=half?half_to_float(float_to_half(nits/kSdrWhiteNits)):nits/kSdrWhiteNits;
        std::vector<float> input(16*16*4,value);
        for(size_t i=3;i<input.size();i+=4)input[i]=1.f;
        const auto path=dir/("pq-peak-"+std::to_string(half)+"-"+std::to_string(preset)+"-"+std::to_string(int(nits))+".jxl");
        StillEncodeQuality quality(preset);
        int rc;
        if(half) {
            std::vector<uint16_t> packed(input.size());
            std::transform(input.begin(),input.end(),packed.begin(),float_to_half);
            rc=encode_jxl_f16(16,16,packed.data(),UHDR_CG_BT_2100,true,path.string().c_str(),quality,true);
        } else rc=encode_jxl_f32(16,16,input.data(),UHDR_CG_BT_2100,true,path.string().c_str(),quality,true);
        check(rc==0,"PQ content-peak encode");
        auto encoded=bytes(path);auto decoder=JxlDecoderMake(nullptr);
        check(JxlDecoderSubscribeEvents(decoder.get(),JXL_DEC_BASIC_INFO|JXL_DEC_FULL_IMAGE)==JXL_DEC_SUCCESS,"PQ peak events");
        JxlDecoderSetInput(decoder.get(),encoded.data(),encoded.size());JxlDecoderCloseInput(decoder.get());
        std::vector<uint16_t> codes(input.size());const JxlPixelFormat format{4,JXL_TYPE_UINT16,JXL_NATIVE_ENDIAN,0};bool complete=false;
        for(;;) {
            const auto status=JxlDecoderProcessInput(decoder.get());
            if(status==JXL_DEC_BASIC_INFO) {
                JxlBasicInfo info{};check(JxlDecoderGetBasicInfo(decoder.get(),&info)==JXL_DEC_SUCCESS,"PQ peak basic info");
                const float peak=std::clamp(value*kSdrWhiteNits,1.f,kMaxNits);
                check(std::abs(info.intensity_target-peak)<=std::max(.01f,peak*.001f),"PQ intensity target does not match content peak");
                check(info.bits_per_sample==16&&info.exponent_bits_per_sample==0,"PQ storage depth changed");
            } else if(status==JXL_DEC_NEED_IMAGE_OUT_BUFFER)check(JxlDecoderSetImageOutBuffer(decoder.get(),&format,codes.data(),codes.size()*2)==JXL_DEC_SUCCESS,"PQ peak buffer");
            else if(status==JXL_DEC_FULL_IMAGE){complete=true;break;}
            else check(status!=JXL_DEC_ERROR&&status!=JXL_DEC_NEED_MORE_INPUT&&status!=JXL_DEC_SUCCESS,"PQ peak decode");
        }
        check(complete,"PQ peak frame missing");
        if(preset==0)for(size_t i=0;i<codes.size();++i)
            check(codes[i]==(i%4==3?65535:std::lround(linear_to_pq_code(value)*65535.f)),"Content peak rescaled absolute PQ codes");
        unsigned w=0,h=0;uhdr_color_gamut_t gamut;bool hdr;int transfer;
        std::vector<float> output;
        check(decode_jxl_to_linear_f32(encoded.data(),encoded.size(),output,w,h,&gamut,&hdr,&transfer,false,nullptr)==0,"PQ peak float decode");
        const float expected=std::min(value,kMaxLinear);
        for(size_t i=0;i<output.size();++i)check(std::abs(output[i]-(i%4==3?1.f:expected))<std::max(.002f,expected*.02f),"PQ content-peak metadata changed absolute brightness");
        std::vector<uint16_t> outputHalf;
        check(decode_jxl_to_linear_f16(encoded.data(),encoded.size(),outputHalf,w,h,&gamut,&hdr,&transfer,false,0,false,nullptr,nullptr)==0,"PQ peak half decode");
        check(std::abs(half_to_float(outputHalf[0])-expected)<std::max(.002f,expected*.02f),"PQ half decode raised content below SDR white");
    }
}

static void srgb_clamping(const fs::path& dir) {
    jxrc::Options o;o.mode=jxrc::Mode::process;o.srgb_clamp=true;o.quality.preset=jxrc::Preset::lossless;
    std::vector<float> referencePixels(16*16*4,.5f);const auto referencePath=dir/"reference-srgb.jpg";
    check(encode_still_export_f32(referencePixels.data(),16,16,0,0,referencePath.string().c_str(),StillEncodeQuality(0))==0,"Reference sRGB JPEG");
    const auto referenceJpeg=bytes(referencePath),referenceIcc=source_jpeg_icc(referenceJpeg.data(),referenceJpeg.size());
    check(!referenceIcc.empty(),"Reference sRGB ICC missing");
    auto has709=[&](const fs::path& path){
        const auto info=jxrc::inspect(path);
        if(info.format=="JPEG"){const auto data=bytes(path);return source_jpeg_icc(data.data(),data.size())==referenceIcc;}
        if(info.format=="AVIF") {
            const auto data=bytes(path);std::unique_ptr<avifDecoder,decltype(&avifDecoderDestroy)> decoder(avifDecoderCreate(),avifDecoderDestroy);
            if(!decoder||avifDecoderSetIOMemory(decoder.get(),data.data(),data.size())!=AVIF_RESULT_OK||avifDecoderParse(decoder.get())!=AVIF_RESULT_OK)return false;
            const auto* image=decoder->image;
            return image->colorPrimaries==1&&(!image->icc.size||(image->icc.size==referenceIcc.size()&&!std::memcmp(image->icc.data,referenceIcc.data(),referenceIcc.size())));
        }
        return info.primaries=="BT.709";
    };
    auto invalid=o;invalid.mode=jxrc::Mode::preserve;fails([&]{jxrc::validate(invalid);});
    invalid=o;invalid.target=jxrc::Target::jxl;invalid.jpeg_transcoding=true;fails([&]{jxrc::validate(invalid);});
    for(int cp:{9,12}) {
        std::vector<float> pixels(16*16*4);
        for(size_t i=0;i<pixels.size();i+=4){pixels[i]=.05f;pixels[i+1]=1.5f;pixels[i+2]=.1f;pixels[i+3]=.5f;}
        StillSourceColor color;color.bits=32;color.exponent_bits=8;color.transfer=8;color.primaries=cp;color.intensity_target=1000.f;
        const auto input=dir/("wide-clamp-"+std::to_string(cp)+".jxl");
        check(encode_jxl_source(16,16,pixels.data(),true,color,input.string().c_str(),StillEncodeQuality(0))==0,"Clamp source fixture");
        jxrc::ConversionSession session(input);
        for(auto target:{jxrc::Target::jxl_linear,jxrc::Target::jxl_pq,jxrc::Target::avif_pq,jxrc::Target::jpeg_gainmap,jxrc::Target::avif_gainmap,jxrc::Target::jxl_scrgb}) {
            o.target=target;const auto output=dir/("clamped-"+std::to_string(cp)+"-"+std::to_string(int(target)));
            session.convert(output,o);const auto info=jxrc::inspect(output);check(has709(output),"sRGB clamp failed to tag output primaries");
            if(target==jxrc::Target::jxl_linear) {
                std::vector<float> expected,actual;unsigned w=0,h=0;uhdr_color_gamut_t gamut;bool hdr=false;int transfer=0;
                auto source=bytes(input),encoded=bytes(output);
                check(decode_jxl_to_linear_f32(source.data(),source.size(),expected,w,h,&gamut,&hdr,&transfer,false,nullptr)==0,"Clamp source decode");
                check(decode_jxl_to_linear_f32(encoded.data(),encoded.size(),actual,w,h,&gamut,&hdr,&transfer,false,nullptr)==0,"Clamped output decode");
                float r=expected[0],g=expected[1],b=expected[2];
                if(cp==9)linear_bt2020_to_bt709(r,g,b);else linear_p3_to_bt709(r,g,b);
                const float reference[]={std::max(0.f,r),std::max(0.f,g),std::max(0.f,b),expected[3]};
                // Linear JXL quantizes half samples and luminance metadata.
                for(unsigned c=0;c<4;++c)if(std::abs(actual[c]-reference[c])>=(c==3?.0001f:std::max(.002f,std::abs(reference[c])*.001f)))throw std::runtime_error("Clamp pixel mismatch: source "+std::to_string(cp)+", channel "+std::to_string(c)+", actual "+std::to_string(actual[c])+", expected "+std::to_string(reference[c]));
                check(actual[1]>1.f,"sRGB gamut clamp destroyed HDR headroom");
            }
        }
        // Changing the toggle must invalidate the shared prepared render.
        o.target=jxrc::Target::jxl_linear;o.srgb_clamp=false;
        const auto reused=dir/("unclamp-reused-"+std::to_string(cp)),fresh=dir/("unclamp-fresh-"+std::to_string(cp));
        session.convert(reused,o);jxrc::convert(input,fresh,o);check(bytes(reused)==bytes(fresh),"Clamped render leaked into another output");
        o.srgb_clamp=true;
    }
    // Integer SDR shortcuts must not bypass a requested gamut conversion.
    StillSourceColor color;color.bits=8;color.transfer=13;color.primaries=12;
    const uint8_t pixels[]={240,30,20,128};const auto input=dir/"p3-integer.jxl";
    check(encode_jxl_source(1,1,pixels,false,color,input.string().c_str(),StillEncodeQuality(0))==0,"P3 SDR fixture");
    for(auto target:{jxrc::Target::jpeg,jxrc::Target::jxl,jxrc::Target::avif}) {
        o.target=target;const auto output=dir/("sdr-clamp-"+std::to_string(int(target)));jxrc::convert(input,output,o);
        const auto info=jxrc::inspect(output);check(has709(output)&&info.bits==8,"SDR shortcut bypassed clamp or changed depth");
    }
}
// Shared metadata must produce the same files as independent codec calls,
// regardless of encoder order or precision, without rescanning for each target.
static void shared_output_analysis(const fs::path& dir) {
    std::vector<float> pixels(32*24*4);
    for(size_t i=0;i<pixels.size();i+=4) {
        pixels[i]=float(i%197)/11.f;pixels[i+1]=float(i%83)/23.f;pixels[i+2]=float(i%41)/37.f;pixels[i+3]=float(i%7)/6.f;
    }
    pixels[0]=NAN;pixels[1]=-1;pixels[2]=INFINITY;pixels[4]=100.f;
    auto encode=[&](int target,const fs::path& output,StillEncodeQuality quality,const float* input) {
        const auto path=output.string();int rc;
        if(target==0||target==3)rc=encode_png_f32(32,24,input,UHDR_CG_BT_2100,16,target==0?16:8,path.c_str(),quality);
        else if(target==1)rc=encode_jxl_f32(32,24,input,UHDR_CG_BT_2100,true,path.c_str(),quality,true);
        else if(target==4||target==6)rc=encode_png_f32(32,24,input,UHDR_CG_BT_2100,18,target==4?8:16,path.c_str(),quality);
        else if(target==5||target==7){quality.custom_avif_depth=target==5?8:10;rc=encode_avif_f32(32,24,input,UHDR_CG_BT_2100,3,path.c_str(),quality);}
        else {quality.custom_avif_depth=target==8?8:12;rc=encode_avif_f32(32,24,input,UHDR_CG_BT_2100,1,path.c_str(),quality);}
        check(rc==0,"Shared metadata encode failed");
    };
    for(int first:{0,1,2}) {
        RenderAnalysis analysis;analysis.bind(pixels.data(),32*24,UHDR_CG_BT_2100);
        StillEncodeQuality shared(4,0,100,12),independent=shared;shared.analysis=&analysis;
        std::vector<int> order={first};for(int target=0;target<=8;++target)if(target!=first)order.push_back(target);
        for(int target:order) {
            const auto shared_path=dir/("analysis-shared-"+std::to_string(first)+"-"+std::to_string(target));
            const auto single_path=dir/("analysis-single-"+std::to_string(first)+"-"+std::to_string(target));
            encode(target,shared_path,shared,pixels.data());encode(target,single_path,independent,pixels.data());
            check(bytes(shared_path)==bytes(single_path),"Shared metadata changed output pixels, profile or light levels");
            check(analysis.pq_builds()==1,"PQ metadata recomputed for another encoder/depth");
        }
        check(analysis.cached_light(18,8)&&analysis.cached_light(18,10)&&analysis.cached_light(18,16),"HLG metadata not cached by bit depth");
        check(!analysis.cached_light(18,12),"HLG metadata incorrectly shared between different precisions");
        // A copied/differently rendered buffer must never borrow stale metadata.
        auto changed=pixels;changed[4]=.5f;changed[8]=.8f;
        const auto shared_path=dir/"analysis-other-buffer",single_path=dir/"analysis-other-single";
        encode(0,shared_path,shared,changed.data());encode(0,single_path,independent,changed.data());
        check(bytes(shared_path)==bytes(single_path),"Metadata cache accepted another pixel buffer");
        analysis.bind(changed.data(),32*24,UHDR_CG_BT_2100);
        check(!analysis.pq_ready()&&!analysis.cached_light(18,8),"Rebinding a render retained stale metadata");
        encode(0,shared_path,shared,changed.data());check(bytes(shared_path)==bytes(single_path),"Changed render retained stale metadata");
        check(analysis.pq_builds()==1&&analysis.peak()<100.f,"Changed render did not rebuild its peak");
    }
    // Exercise ConversionSession invalidation, not only the native cache API.
    std::fill(pixels.begin(),pixels.end(),.5f);for(size_t i=3;i<pixels.size();i+=4)pixels[i]=1.f;pixels[0]=6.f;
    const auto source=dir/"analysis-session-source.jxl";
    check(encode_jxl_f32(32,24,pixels.data(),UHDR_CG_BT_2100,true,source.string().c_str(),StillEncodeQuality(0),false)==0,"Analysis session source");
    jxrc::ConversionSession session(source);
    for(float exposure:{0.f,1.f,-.5f})for(auto target:{jxrc::Target::jxl_pq,jxrc::Target::avif_pq,jxrc::Target::png_hdr}) {
        jxrc::Options options;options.mode=jxrc::Mode::process;options.target=target;options.exposure=exposure;options.quality.preset=jxrc::Preset::lossless;
        const auto shared_path=dir/"analysis-session-output",single_path=dir/"analysis-session-single";
        session.convert(shared_path,options);jxrc::convert(source,single_path,options);
        check(bytes(shared_path)==bytes(single_path),"Session metadata cache failed to invalidate changed processing controls");
    }
}

static void shared_render(const fs::path& dir) {
    const auto source=dir/"session.png";write(source,raster_png8);
    jxrc::ConversionSession session(source);
    jxrc::Options o;o.mode=jxrc::Mode::process;o.target=jxrc::Target::jxl;o.quality.preset=jxrc::Preset::lossless;
    session.convert(dir/"session.jxl",o);
    auto sc=o;sc.target=jxrc::Target::jxl_scrgb;
    session.convert(dir/"sdr-to-scrgb.jxl",sc);
    auto converted=bytes(dir/"sdr-to-scrgb.jxl");check(is_scrgb_jxl(converted.data(),converted.size()),"SDR to scRGB metadata");
    const auto raster=decode_regular(raster_png8.data(),raster_png8.size());
    std::vector<float> expected,actual;uhdr_color_gamut_t gamut;unsigned width=0,height=0;bool decodedHdr=false;int transfer=0;
    regular_to_linear(raster,expected,gamut);
    for(size_t i=0;i<expected.size();i+=4) {
        if(gamut==UHDR_CG_BT_2100)linear_bt2020_to_bt709(expected[i],expected[i+1],expected[i+2]);
        else if(gamut==UHDR_CG_DISPLAY_P3)linear_p3_to_bt709(expected[i],expected[i+1],expected[i+2]);
    }
    check(decode_jxl_to_linear_f32(converted.data(),converted.size(),actual,width,height,&gamut,&decodedHdr,&transfer,false,nullptr)==0,"SDR to scRGB decode");
    check(actual.size()==expected.size(),"SDR to scRGB dimensions");
    for(size_t i=0;i<expected.size();++i)check(std::abs(actual[i]-expected[i])<1e-6f,"SDR to scRGB changed brightness");
    o.target=jxrc::Target::avif;
    session.convert(dir/"session.avif",o);
    jxrc::convert(source,dir/"single.avif",o);
    check(bytes(dir/"session.avif")==bytes(dir/"single.avif"),"Shared render changed AVIF pixels");
    check(jxrc::inspect(dir/"session.avif").bits==8,"AVIF maximum depth promoted an 8-bit source");
    o.target=jxrc::Target::jxl_pq;
    session.convert(dir/"session-pq.jxl",o);
    check(jxrc::inspect(dir/"session-pq.jxl").bits==16,"PQ JXL must store 16-bit integer codes");
    o.target=jxrc::Target::jxl;o.exposure=1;o.max_edge=2;
    session.convert(dir/"changed.jxl",o);jxrc::convert(source,dir/"single.jxl",o);
    check(bytes(dir/"changed.jxl")==bytes(dir/"single.jxl"),"Shared render failed to invalidate changed controls");
    o.target=jxrc::Target::avif;o.quality.preset=jxrc::Preset::custom;o.quality.avif_depth=10;
    write(source,raster_png16);jxrc::convert(source,dir/"max10.avif",o);
    check(jxrc::inspect(dir/"max10.avif").bits==10,"Custom AVIF max depth");
    jxrc::Options avifJxl;avifJxl.mode=jxrc::Mode::process;avifJxl.target=jxrc::Target::jxl;avifJxl.quality.preset=jxrc::Preset::lossless;
    jxrc::convert(dir/"max10.avif",dir/"integer-avif.jxl",avifJxl);
    auto integerAvif=bytes(dir/"integer-avif.jxl");
    check(jxrc::inspect(dir/"integer-avif.jxl").bits==10 && is_integer_jxl(integerAvif.data(),integerAvif.size()),"Untouched AVIF source depth lost");
    avifJxl.exposure=.25f;jxrc::convert(dir/"max10.avif",dir/"edited-avif.jxl",avifJxl);
    auto editedAvif=bytes(dir/"edited-avif.jxl");
    check(jxrc::inspect(dir/"edited-avif.jxl").bits==16 && is_integer_jxl(editedAvif.data(),editedAvif.size()),"Processed AVIF did not use 16-bit integer SDR JXL");
    const float hdr[]={-0.125f,2.f,0.5f,0.5f};StillEncodeQuality quality(0);quality.jxl_half=true;
    const auto half=dir/"half-linear.jxl";
    check(encode_jxl_f32(1,1,hdr,UHDR_CG_BT_709,true,half.string().c_str(),quality,false)==0,"Half-float linear JXL encode");
    check(jxrc::inspect(half).bits==16,"Half-float JXR storage choice");
}

// General JPEG decoding must not inherit Ultra HDR's RGB-only/8192 limit.
// A small synthetic CMYK -> D50 XYZ LUT, with identity input/output tables.
static std::vector<uint8_t> cmyk_profile() {
    std::vector<uint8_t> profile(144+48+1024+48+768,0);
    auto u32=[&](size_t at,uint32_t value){for(int i=3;i>=0;--i){profile[at+i]=uint8_t(value);value>>=8;}};
    u32(0,profile.size());u32(8,0x02100000);std::memcpy(profile.data()+12,"prtrCMYKXYZ ",12);
    std::memcpy(profile.data()+36,"acsp",4);u32(68,63190);u32(72,65536);u32(76,54061);
    u32(128,1);std::memcpy(profile.data()+132,"A2B0",4);u32(136,144);u32(140,profile.size()-144);
    std::memcpy(profile.data()+144,"mft1",4);profile[152]=4;profile[153]=3;profile[154]=2;
    for(unsigned c=0;c<3;++c)u32(156+(c*3+c)*4,65536);
    for(unsigned c=0;c<4;++c)for(unsigned v=0;v<256;++v)profile[192+c*256+v]=v;
    for(unsigned entry=0;entry<16;++entry) {
        const double r=(entry&8)?0:(entry&1)?0:1,g=(entry&4)?0:(entry&1)?0:1,b=(entry&2)?0:(entry&1)?0:1;
        const double xyz[]={.4360747*r+.3850649*g+.1430804*b,.2225045*r+.7168786*g+.0606169*b,.0139322*r+.0971045*g+.7141733*b};
        for(unsigned c=0;c<3;++c)profile[1216+entry*3+c]=uint8_t(std::lround(xyz[c]*128));
    }
    for(unsigned c=0;c<3;++c)for(unsigned v=0;v<256;++v)profile[1264+c*256+v]=v;
    profile.insert(profile.begin()+144,12,0);u32(128,2);u32(136,156);
    const size_t white=profile.size();std::memcpy(profile.data()+144,"wtpt",4);u32(148,white);u32(152,20);
    profile.resize(white+20,0);std::memcpy(profile.data()+white,"XYZ ",4);
    u32(white+8,63190);u32(white+12,65536);u32(white+16,54061);u32(0,profile.size());
    return profile;
}

static void jpeg_inputs(const fs::path& dir) {
    const auto profile=cmyk_profile();
    auto tagged=jpeg_cmyk;
    const unsigned length=profile.size()+16;
    std::vector<uint8_t> marker={255,226,uint8_t(length>>8),uint8_t(length),'I','C','C','_','P','R','O','F','I','L','E',0,1,1};
    marker.insert(marker.end(),profile.begin(),profile.end());tagged.insert(tagged.begin()+2,marker.begin(),marker.end());
    unsigned index=0;
    for(const auto* fixture:std::vector<const std::vector<uint8_t>*>{&jpeg_cmyk,&jpeg_cmyk_no_adobe,&jpeg_ycck,&tagged}) {
        const auto source=dir/("cmyk-"+std::to_string(index++)+".jpg");write(source,*fixture);
        const auto info=jxrc::inspect(source);check(info.width==32&&info.height==8&&!info.hdr,"CMYK JPEG inspection");
        const auto decoded=decode_jpeg(fixture->data(),fixture->size());
        for(unsigned patch=0;patch<4;++patch) {
            const auto* pixel=decoded.pixels.data()+(2*32+patch*8+4)*4;
            if(fixture!=&tagged)for(unsigned c=0;c<3;++c)check(std::abs(int(pixel[c])-(patch==c?0:255))<=3,"CMYK/YCCK JPEG colors");
            else check(decoded.icc.empty(),"Converted CMYK retained its CMYK ICC");
            check(pixel[3]==255,"CMYK K was treated as alpha");
        }
        check(!jxrc::preview(source,24).rgba.empty(),"CMYK JPEG preview");
        for(auto target:{jxrc::Target::jpeg,jxrc::Target::png,jxrc::Target::jxl,jxrc::Target::avif}) {
            jxrc::Options options;options.target=target;options.quality.preset=jxrc::Preset::lossless;
            const auto output=dir/("cmyk-out-"+std::to_string(index)+"-"+std::to_string(int(target)));
            jxrc::convert(source,output,options);check(jxrc::inspect(output).width==32,"CMYK preserved output");
            if(target==jxrc::Target::jpeg) {
                const auto out=bytes(output);const auto native=decode_jpeg(out.data(),out.size(),false,true);
                check(native.cmyk&&native.channels==4&&native.icc==(fixture==&tagged?profile:std::vector<uint8_t>{}),"JPEG CMYK/ICC was not preserved");
            }
            if(target==jxrc::Target::jxl&&fixture==&tagged) {
                const auto out=bytes(output);auto dec=JxlDecoderMake(nullptr);JxlBasicInfo basic{};
                JxlDecoderSubscribeEvents(dec.get(),JXL_DEC_BASIC_INFO|JXL_DEC_COLOR_ENCODING|JXL_DEC_FULL_IMAGE);JxlDecoderSetInput(dec.get(),out.data(),out.size());JxlDecoderCloseInput(dec.get());
                std::vector<uint8_t> cmy,black;JxlPixelFormat fmt{3,JXL_TYPE_UINT8,JXL_NATIVE_ENDIAN,0};
                for(;;) {
                    const auto status=JxlDecoderProcessInput(dec.get());
                    if(status==JXL_DEC_BASIC_INFO) {
                        JxlDecoderGetBasicInfo(dec.get(),&basic);JxlExtraChannelInfo extra{};JxlDecoderGetExtraChannelInfo(dec.get(),0,&extra);
                        check(basic.num_extra_channels==1&&!basic.alpha_bits&&extra.type==JXL_CHANNEL_BLACK,"JXL CMYK black channel");
                    } else if(status==JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                        size_t count=0;JxlDecoderImageOutBufferSize(dec.get(),&fmt,&count);cmy.resize(count);JxlDecoderSetImageOutBuffer(dec.get(),&fmt,cmy.data(),count);
                        JxlDecoderExtraChannelBufferSize(dec.get(),&fmt,&count,0);black.resize(count);JxlDecoderSetExtraChannelBuffer(dec.get(),&fmt,black.data(),count,0);
                    } else if(status==JXL_DEC_FULL_IMAGE)break;
                    else check(status!=JXL_DEC_ERROR&&status!=JXL_DEC_NEED_MORE_INPUT,"CMYK JXL decode");
                }
                const auto native=decode_jpeg(fixture->data(),fixture->size(),false,true);
                for(size_t i=0;i<black.size();++i) {
                    check(black[i]==native.pixels[i*4+3],"JXL changed K samples");
                    for(unsigned c=0;c<3;++c)check(cmy[i*3+c]==native.pixels[i*4+c],"JXL changed CMY samples");
                }
                check(!jxrc::preview(output,24).rgba.empty(),"CMYK JXL preview");
                jxrc::Options back;back.target=jxrc::Target::jpeg;
                const auto restored=dir/"cmyk-jxl-restored.jpg";jxrc::convert(output,restored,back);
                const auto restored_bytes=bytes(restored);check(decode_jpeg(restored_bytes.data(),restored_bytes.size(),false,true).cmyk,"JXL to JPEG lost CMYK");
                for(auto color_target:{jxrc::Target::png,jxrc::Target::avif}) {
                    back.target=color_target;jxrc::convert(output,dir/("cmyk-jxl-rgb-"+std::to_string(int(color_target))),back);
                }
                back.mode=jxrc::Mode::process;back.target=jxrc::Target::png;back.exposure=.5;
                jxrc::convert(output,dir/"cmyk-jxl-f32.png",back);
            }
        }
        jxrc::Options options;options.mode=jxrc::Mode::process;options.target=jxrc::Target::png;options.exposure=1;
        const auto output=dir/("cmyk-process-"+std::to_string(index)+".png");jxrc::convert(source,output,options);
        check(jxrc::inspect(output).width==32,"CMYK F32 output");
    }
    const auto gray=dir/"gray-source.png",gray_jpeg=dir/"gray.jpg";
    std::vector<uint8_t> gray_samples(32*8,125);StillSourceColor gray_color;gray_color.channels=1;
    check(encode_png_source(32,8,gray_samples.data(),false,gray_color,gray.string().c_str(),StillEncodeQuality(0))==0,"Grayscale fixture");
    jxrc::Options gray_options;gray_options.target=jxrc::Target::jpeg;jxrc::convert(gray,gray_jpeg,gray_options);
    const auto gray_bytes=bytes(gray_jpeg);check(decode_jpeg(gray_bytes.data(),gray_bytes.size(),false,true).channels==1,"Grayscale JPEG expanded to RGB");
    gray_options.target=jxrc::Target::png;const auto gray_png=dir/"gray-copy.png";jxrc::convert(gray_jpeg,gray_png,gray_options);
    check(bytes(gray_png)[25]==0,"Grayscale PNG expanded to RGB");
    gray_options.target=jxrc::Target::jxl;const auto gray_jxl=dir/"gray-copy.jxl";jxrc::convert(gray_jpeg,gray_jxl,gray_options);
    const auto gray_data=bytes(gray_jxl);auto gray_decoder=JxlDecoderMake(nullptr);JxlDecoderSubscribeEvents(gray_decoder.get(),JXL_DEC_BASIC_INFO);
    JxlDecoderSetInput(gray_decoder.get(),gray_data.data(),gray_data.size());check(JxlDecoderProcessInput(gray_decoder.get())==JXL_DEC_BASIC_INFO,"Grayscale JXL header");
    JxlBasicInfo gray_info{};JxlDecoderGetBasicInfo(gray_decoder.get(),&gray_info);check(gray_info.num_color_channels==1,"Grayscale JXL expanded to RGB");
    const auto png=dir/"wide-jpeg-source.png",jpeg=dir/"wide.jpg";
    std::vector<uint8_t> pixels(9000*8*4,255);StillSourceColor color;
    check(encode_png_source(9000,8,pixels.data(),false,color,png.string().c_str(),StillEncodeQuality(0))==0,"Wide JPEG fixture PNG");
    jxrc::Options options;options.target=jxrc::Target::jpeg;jxrc::convert(png,jpeg,options);
    check(jxrc::inspect(jpeg).width==9000,"Wide JPEG inspection");
    check(!jxrc::preview(jpeg,64).rgba.empty(),"Wide JPEG preview");
    options.target=jxrc::Target::png;jxrc::convert(jpeg,dir/"wide-direct.png",options);
    options.mode=jxrc::Mode::process;options.exposure=.5;jxrc::convert(jpeg,dir/"wide-f32.png",options);
    const auto encoded=bytes(jpeg);const auto decoded=decode_jpeg(encoded.data(),encoded.size());
    check(decoded.width==9000&&decoded.pixels[0]==255,"Wide JPEG pixels");
    // A tall or wide source remains safe under the pixel budget even when one
    // axis exceeds the old, unrelated 16,384-pixel threshold.
    const auto narrow=dir/"narrow.png";
    std::vector<uint8_t> narrow_pixels(20000*2*4,255);
    check(encode_png_source(20000,2,narrow_pixels.data(),false,color,narrow.string().c_str(),StillEncodeQuality(0))==0,"Narrow source fixture");
    for(auto target:{jxrc::Target::jxl,jxrc::Target::avif}) {
        jxrc::Options wide;wide.target=target;wide.quality.preset=jxrc::Preset::lossless;
        const auto source=dir/(target==jxrc::Target::jxl?"narrow.jxl":"narrow.avif");
        jxrc::convert(narrow,source,wide);
        check(jxrc::inspect(source).width==20000,"Long, narrow source inspection");
        try {check(!jxrc::preview(source,64).rgba.empty(),"Long, narrow source preview");}
        catch(const std::exception& error){throw std::runtime_error(source.filename().string()+": "+error.what());}
        wide.mode=jxrc::Mode::process;wide.target=jxrc::Target::png;wide.exposure=.1;
        const auto output=dir/(target==jxrc::Target::jxl?"narrow-jxl.png":"narrow-avif.png");
        jxrc::convert(source,output,wide);
        check(jxrc::inspect(output).width==20000,"Long, narrow F32 source conversion");
    }
    auto corrupt=jpeg_cmyk;corrupt.resize(24);fails([&]{decode_jpeg(corrupt.data(),corrupt.size());});
    std::cout<<"General JPEG inputs passed\n";
}

// Preserve native AVIF YUV planes and exercise JPEG quantization/compositing.
static void jpeg_avif_paths(const fs::path& dir) {
    auto image=std::unique_ptr<avifImage,decltype(&avifImageDestroy)>(avifImageCreate(8,8,8,AVIF_PIXEL_FORMAT_YUV420),avifImageDestroy);
    check(bool(image),"AVIF YUV fixture allocation");
    image->colorPrimaries=AVIF_COLOR_PRIMARIES_BT2020;image->transferCharacteristics=AVIF_TRANSFER_CHARACTERISTICS_SRGB;
    image->matrixCoefficients=AVIF_MATRIX_COEFFICIENTS_BT2020_NCL;image->yuvRange=AVIF_RANGE_LIMITED;
    check(avifImageAllocatePlanes(image.get(),AVIF_PLANES_YUV)==AVIF_RESULT_OK,"AVIF fixture planes");
    for(unsigned c=0;c<3;++c)for(unsigned y=0;y<(c?4u:8u);++y)for(unsigned x=0;x<(c?4u:8u);++x)
        image->yuvPlanes[c][y*image->yuvRowBytes[c]+x]=uint8_t(16+(x*31+y*53+c*47)%220);
    auto encoder=std::unique_ptr<avifEncoder,decltype(&avifEncoderDestroy)>(avifEncoderCreate(),avifEncoderDestroy);
    encoder->codecChoice=AVIF_CODEC_CHOICE_AOM;encoder->quality=100;encoder->speed=10;encoder->maxThreads=1;
    avifRWData compressed=AVIF_DATA_EMPTY;
    check(avifEncoderWrite(encoder.get(),image.get(),&compressed)==AVIF_RESULT_OK,"AVIF YUV fixture encode");
    const auto input=dir/"native-yuv.avif",output=dir/"native-yuv-copy.avif";
    write(input,std::vector<uint8_t>(compressed.data,compressed.data+compressed.size));avifRWDataFree(&compressed);
    jxrc::Options o;o.mode=jxrc::Mode::process;o.target=jxrc::Target::avif;o.quality.preset=jxrc::Preset::lossless;
    jxrc::convert(input,output,o);
    auto decoder=std::unique_ptr<avifDecoder,decltype(&avifDecoderDestroy)>(avifDecoderCreate(),avifDecoderDestroy);
    const auto data=bytes(output);check(avifDecoderSetIOMemory(decoder.get(),data.data(),data.size())==AVIF_RESULT_OK && avifDecoderParse(decoder.get())==AVIF_RESULT_OK && avifDecoderNextImage(decoder.get())==AVIF_RESULT_OK,"Copied AVIF decode");
    const auto* copied=decoder->image;
    check(copied->yuvFormat==image->yuvFormat && copied->yuvRange==image->yuvRange && copied->matrixCoefficients==image->matrixCoefficients && copied->colorPrimaries==image->colorPrimaries,"Untouched AVIF YUV layout or color tags changed");
    for(unsigned c=0;c<3;++c)for(unsigned y=0;y<(c?4u:8u);++y)
        check(!std::memcmp(copied->yuvPlanes[c]+y*copied->yuvRowBytes[c],image->yuvPlanes[c]+y*image->yuvRowBytes[c],c?4:8),"Untouched AVIF YUV samples changed");
    // Both adapter precisions must present tightly packed bytes to libavif
    // when the source-depth cap requests an 8-bit output.
    const float values[]={0.f,.25f,1.f,1.f,.5f,.75f,.125f,.5f,1.f,.5f,.25f,1.f,.125f,0.f,.75f,0.f};
    StillEncodeQuality quality(0);quality.source_bits=8;
    for(int mode=0;mode<3;++mode) {
        std::vector<uint16_t> half(16);std::transform(values,values+16,half.begin(),float_to_half);
        const auto f16=dir/("avif8-f16-"+std::to_string(mode)+".avif"),f32=dir/("avif8-f32-"+std::to_string(mode)+".avif");
        check(encode_avif_f16(2,2,half.data(),UHDR_CG_BT_709,mode,f16.string().c_str(),quality,nullptr)==0,"F16 8-bit AVIF encode");
        check(encode_avif_f32(2,2,values,UHDR_CG_BT_709,mode,f32.string().c_str(),quality,nullptr)==0,"F32 8-bit AVIF encode");
        check(bytes(f16)==bytes(f32),"F16/F32 8-bit AVIF packing mismatch");
    }
    // Both source depths describe the same final 8-bit JPEG codes. An AVIF
    // bridge would introduce an extra YUV conversion/depth quantization.
    std::vector<uint8_t> rgb8(16*16*4);std::vector<uint16_t> rgb16(rgb8.size());
    for(size_t i=0;i<rgb8.size();++i) {
        rgb8[i]=i%4==3?255:uint8_t((i*37+19)%256);
        rgb16[i]=uint16_t(rgb8[i]*257u);
        if(i%4!=3 && rgb8[i]>0 && rgb8[i]<255)rgb16[i]+=73;
    }
    StillSourceColor color;
    check(encode_jxl_source(16,16,rgb8.data(),false,color,(dir/"jpeg-source8.jxl").string().c_str(),StillEncodeQuality(0))==0,"JPEG source8 fixture");
    color.bits=16;
    check(encode_jxl_source(16,16,rgb16.data(),true,color,(dir/"jpeg-source16.jxl").string().c_str(),StillEncodeQuality(0))==0,"JPEG source16 fixture");
    o.target=jxrc::Target::jpeg;o.quality.preset=jxrc::Preset::low;
    jxrc::convert(dir/"jpeg-source8.jxl",dir/"jpeg8.jpg",o);jxrc::convert(dir/"jpeg-source16.jxl",dir/"jpeg16.jpg",o);
    check(bytes(dir/"jpeg8.jpg")==bytes(dir/"jpeg16.jpg"),"JPEG integer source took an extra color/depth conversion");
    write(dir/"jpeg-alpha.png",raster_png16);
    jxrc::convert(dir/"jpeg-alpha.png",dir/"alpha-direct.jpg",o);
    o.max_edge=64000;jxrc::convert(dir/"jpeg-alpha.png",dir/"alpha-rendered.jpg",o);
    check(bytes(dir/"alpha-direct.jpg")==bytes(dir/"alpha-rendered.jpg"),"JPEG alpha compositing changed");
}

// Insert a valid color chunk into synthetic fixtures; no external HDR assets.
static std::vector<uint8_t> png_chunk(std::vector<uint8_t> png,const char* type,std::vector<uint8_t> payload) {
    std::vector<uint8_t> chunk={0,0,0,uint8_t(payload.size())};
    chunk.insert(chunk.end(),type,type+4);chunk.insert(chunk.end(),payload.begin(),payload.end());
    uint32_t crc=~0u;
    for(size_t i=4;i<chunk.size();++i){crc^=chunk[i];for(int b=0;b<8;++b)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}
    crc=~crc;for(int i=3;i>=0;--i)chunk.push_back(uint8_t(crc>>(8*i)));
    png.insert(png.begin()+33,chunk.begin(),chunk.end());return png;
}
static void hdr_png(const fs::path& dir) {
    for(unsigned depth:{8u,16u})for(unsigned cp:{1u,9u,12u})for(unsigned tc:{16u,18u}) {
        const auto name=std::to_string(depth)+"-"+std::to_string(cp)+"-"+std::to_string(tc);
        auto png=png_chunk(depth==16?raster_png16:raster_png8,"cICP",{uint8_t(cp),uint8_t(tc),0,1});
        // A lower-priority gamma chunk must never change the HDR interpretation.
        png=png_chunk(png,"gAMA",{0,0,177,143});
        const auto input=dir/(name+".png");write(input,png);
        const auto source=decode_regular(png.data(),png.size());
        check(source.hdr() && source.transfer==tc && source.primaries==cp && source.icc.empty(),"HDR PNG cICP priority");
        const auto info=jxrc::inspect(input);
        check(info.hdr && info.bits==depth && info.transfer==(tc==16?"PQ":"HLG"),"HDR PNG inspection");
        // A broken IDAT must not prevent a metadata-only HDR routing decision.
        // Actual decoding still rejects it; this proves inspection skips pixels.
        auto broken=png;
        for(size_t at=8;at+12<=broken.size();) {
            const size_t n=size_t(broken[at])<<24|size_t(broken[at+1])<<16|size_t(broken[at+2])<<8|broken[at+3];
            if(!std::memcmp(broken.data()+at+4,"IDAT",4)){if(n)broken[at+8]^=0xff;break;}at+=n+12;
        }
        check(jxrc::inspect_bytes(broken).hdr,"PNG HDR routing decoded pixel data");
        fails([&]{decode_regular(broken.data(),broken.size());});
        std::vector<float> linear;uhdr_color_gamut_t gamut;regular_to_linear(source,linear,gamut);
        check(gamut==(cp==9?UHDR_CG_BT_2100:cp==12?UHDR_CG_DISPLAY_P3:UHDR_CG_BT_709),"HDR PNG primaries");
        const auto& codes=depth==16?expected_png16:expected_png8;const double max=depth==16?65535:255;
        for(size_t i=0;i<6;++i) {
            double rgb[3];
            for(unsigned c=0;c<3;++c) {
                const double v=codes[i*4+c]/max;
                if(tc==16){const double n=std::pow(v,32./2523.);rgb[c]=10000./203.*std::pow(std::max(n-3424./4096.,0.)/(2413./128.-2392./128.*n),16384./2610.);}
                else rgb[c]=v<=.5?v*v/3.:(std::exp((v-.55991073)/.17883277)+.28466892)/12.;
            }
            if(tc==18) {
                const double yr=cp==9?.2627:cp==12?.2289746:.2126,yb=cp==9?.0593:cp==12?.0792869:.0722;
                const double scale=std::pow(yr*rgb[0]+(1-yr-yb)*rgb[1]+yb*rgb[2],.2)*1000./203.;
                for(auto& c:rgb)c*=scale;
            }
            for(unsigned c=0;c<3;++c)check(std::abs(linear[i*4+c]-rgb[c])<std::max(1e-6,std::abs(rgb[c])*.0003),"HDR PNG transfer/OOTF");
            check(std::abs(linear[i*4+3]-codes[i*4+3]/max)<1e-7,"HDR PNG straight alpha");
        }
        const auto thumb=jxrc::preview(input,2);check(thumb.width==2 && thumb.height==1,"HDR PNG preview");
        jxrc::Options options;options.target=jxrc::Target::jxl;options.quality.preset=jxrc::Preset::lossless;
        const auto output=dir/(name+".jxl");const auto result=jxrc::convert(input,output,options);
        check(result.transfer==int(tc),"Preserved PNG transfer result");
        const auto encoded=bytes(output);auto dec=JxlDecoderMake(nullptr);
        JxlDecoderSubscribeEvents(dec.get(),JXL_DEC_BASIC_INFO|JXL_DEC_COLOR_ENCODING|JXL_DEC_FULL_IMAGE);
        JxlDecoderSetInput(dec.get(),encoded.data(),encoded.size());JxlDecoderCloseInput(dec.get());
        std::vector<uint16_t> samples(24);const JxlPixelFormat format{4,JXL_TYPE_UINT16,JXL_NATIVE_ENDIAN,0};bool complete=false;
        for(;;) {
            const auto status=JxlDecoderProcessInput(dec.get());
            if(status==JXL_DEC_COLOR_ENCODING){JxlColorEncoding color{};check(JxlDecoderGetColorAsEncodedProfile(dec.get(),JXL_COLOR_PROFILE_TARGET_ORIGINAL,&color)==JXL_DEC_SUCCESS,"PNG JXL color profile");check(int(color.transfer_function)==int(tc) && int(color.primaries)==int(cp==12?JXL_PRIMARIES_P3:cp),"PNG JXL cICP preservation");}
            else if(status==JXL_DEC_NEED_IMAGE_OUT_BUFFER)check(JxlDecoderSetImageOutBuffer(dec.get(),&format,samples.data(),samples.size()*2)==JXL_DEC_SUCCESS,"PNG JXL pixel buffer");
            else if(status==JXL_DEC_FULL_IMAGE){complete=true;break;}
            else if(status==JXL_DEC_SUCCESS)break;
            else check(status!=JXL_DEC_ERROR && status!=JXL_DEC_NEED_MORE_INPUT,"PNG JXL decode");
        }
        check(complete,"PNG JXL missing pixels");
        for(size_t i=0;i<24;++i)check(samples[i]==codes[i]*(depth==16?1:257),"HDR PNG source code changed");
        options.target=jxrc::Target::avif;
        const auto avif=dir/(name+".avif");check(jxrc::convert(input,avif,options).transfer==int(tc),"PNG AVIF transfer preservation");
        const auto avif_info=jxrc::inspect(avif);check(avif_info.hdr && avif_info.transfer==info.transfer && avif_info.primaries==info.primaries,"PNG AVIF color metadata");
        options.mode=jxrc::Mode::process;options.target=jxrc::Target::jxl_linear;
        check(jxrc::convert(input,dir/(name+"-linear.jxl"),options).width==3,"HDR PNG processing");
    }
    // An invalid ancillary ICC chunk must warn, while leaving usable pixels.
    auto warned=png_chunk(raster_png8,"iCCP",{'b','a','d',0,0});
    const auto recovered=decode_regular(warned.data(),warned.size());
    check(recovered.width==3&&recovered.height==2,"PNG metadata warning aborted decoding");
    const auto thumb=jxrc::preview_bytes(warned,2);
    check(thumb.width==2&&thumb.height==1,"PNG metadata warning aborted thumbnail");

    for(auto tags:std::vector<std::vector<uint8_t>>{{9,16,0,0},{9,16,1,1},{2,16,0,1}}) {
        auto bad=png_chunk(raster_png16,"cICP",tags);fails([&]{decode_regular(bad.data(),bad.size());});
    }
    auto bad=png_chunk(raster_png16,"cICP",{9,16,0,1});bad[45]^=1;
    fails([&]{decode_regular(bad.data(),bad.size());}); // CRC, not just chunk contents.
}

// Exercise the direct source path separately from the F32 processing boundary.
// Read the wire format independently of libpng's cLLI accessors.
static std::vector<uint32_t> png_light_chunk(const std::vector<uint8_t>& data) {
    const auto u32=[&](size_t at){return uint32_t(data[at])<<24|uint32_t(data[at+1])<<16|uint32_t(data[at+2])<<8|data[at+3];};
    std::vector<uint32_t> result;bool pixels=false;
    for(size_t at=8;at+12<=data.size();) {
        const auto size=u32(at);check(size<=data.size()-at-12,"cLLI PNG chunk bounds");
        if(!std::memcmp(data.data()+at+4,"IDAT",4))pixels=true;
        if(!std::memcmp(data.data()+at+4,"cLLI",4)) {
            check(!pixels&&size==8&&result.empty(),"cLLI size, order or duplicates");
            result={u32(at+8),u32(at+12)};
        }
        at+=size+12;
    }
    return result;
}

static void check_png_light_levels(const fs::path& path,unsigned cp,unsigned transfer) {
    const auto data=bytes(path);const auto levels=png_light_chunk(data);
    if(transfer==13){check(levels.empty(),"SDR PNG has HDR cLLI metadata");return;}
    check(levels.size()==2,"HDR PNG missing cLLI");
    const auto png=decode_regular(data.data(),data.size());
    const double yr=cp==9?.2627:cp==12?.2289746:.2126,yb=cp==9?.0593:cp==12?.0792869:.0722;
    double peak=0,sum=0;
    for(size_t i=0;i<size_t(png.width)*png.height;++i) {
        double rgb[3];
        for(unsigned c=0;c<3;++c) {
            uint16_t code=png.pixels[i*4+c];if(png.bits==16)std::memcpy(&code,png.pixels.data()+(i*4+c)*2,2);
            const double v=double(code)/(png.bits==8?255.:65535.);
            if(transfer==16) {
                const double n=std::pow(v,32./2523.);
                rgb[c]=10000.*std::pow(std::max(n-3424./4096.,0.)/(2413./128.-2392./128.*n),16384./2610.);
            } else rgb[c]=v<=.5?v*v/3.:(std::exp((v-.55991073)/.17883277)+.28466892)/12.;
        }
        if(transfer==18) {
            const double scale=1000.*std::pow(yr*rgb[0]+(1-yr-yb)*rgb[1]+yb*rgb[2],.2);
            for(auto& channel:rgb)channel*=scale;
        }
        const double light=std::min(std::max({rgb[0],rgb[1],rgb[2]}),transfer==16?10000.:1000.);
        peak=std::max(peak,light);sum+=light;
    }
    // Production's shared EOTF lookup is float; this reference uses double.
    const double average=sum/(double(png.width)*png.height);
    check(std::abs(double(levels[0])/10000.-peak)<std::max(.002,peak*.0001),"PNG MaxCLL differs from decoded output peak");
    check(std::abs(double(levels[1])/10000.-average)<std::max(.002,average*.0001),"PNG MaxFALL differs from decoded output frame average");
}

static void png_content_light(const fs::path& dir) {
    // Colored pixels distinguish maxRGB from weighted Y; straight alpha must
    // not change content metadata. Invalid and clipped input is measured only
    // after encoding, including HLG scene-channel clipping and PNG quantization.
    const float input[]={7350.f/203.f,1,.2f,0, .1f,2,.3f,.5f, .05f,.1f,.2f,1,
                         NAN,INFINITY,-1,1, 0,0,0,1, .3f,4,.2f,.25f};
    for(unsigned transfer:{13u,16u,18u})for(unsigned depth:{8u,16u})
        for(auto gamut:{UHDR_CG_BT_709,UHDR_CG_DISPLAY_P3,UHDR_CG_BT_2100}) {
            const unsigned cp=gamut==UHDR_CG_BT_2100?9:gamut==UHDR_CG_DISPLAY_P3?12:1;
            const auto stem="png-light-"+std::to_string(transfer)+"-"+std::to_string(depth)+"-"+std::to_string(cp);
            const auto path=dir/(stem+".png");StillEncodeQuality quality(0);
            check(encode_png_f32(3,2,input,gamut,transfer,depth,path.string().c_str(),quality)==0,"PNG light-level encode");
            check_png_light_levels(path,cp,transfer);
            const auto data=bytes(path);const auto png=decode_regular(data.data(),data.size());
            StillSourceColor color;color.bits=depth;color.channels=4;color.primaries=cp;color.transfer=transfer;color.orientation=6;
            quality.png_auto_bits=false;quality.png_bits=depth==8?16:8;
            const auto direct=dir/(stem+"-direct.png");
            check(encode_png_source(3,2,png.pixels.data(),depth==16,color,direct.string().c_str(),quality)==0,"Direct PNG light-level encode");
            check_png_light_levels(direct,cp,transfer);
        }
    for(unsigned transfer:{16u,18u}) {
        const float black[]={0,0,0,1};const auto path=dir/("png-light-black-"+std::to_string(transfer)+".png");
        check(encode_png_f32(1,1,black,UHDR_CG_BT_2100,transfer,16,path.string().c_str(),StillEncodeQuality(0))==0,"Black HDR PNG encode");
        check(png_light_chunk(bytes(path))==std::vector<uint32_t>({0,0}),"Black PNG invented a minimum light level");
        for(unsigned depth:{8u,16u}) {
            const uint8_t gray8[]={0,0,255,255};const uint16_t gray16[]={0,0,65535,65535};
            const auto grayPath=dir/("png-light-gray-"+std::to_string(transfer)+"-"+std::to_string(depth)+".png");
            StillSourceColor color;color.bits=depth;color.channels=2;color.primaries=9;color.transfer=transfer;
            check(encode_png_source(2,1,depth==8?static_cast<const void*>(gray8):gray16,depth==16,color,grayPath.string().c_str(),StillEncodeQuality(0))==0,"Gray HDR PNG encode");
            check_png_light_levels(grayPath,9,transfer);
        }
    }
}

static void png_outputs(const fs::path& dir) {
    png_content_light(dir);
    for(unsigned depth:{8u,16u})for(unsigned cp:{1u,9u,12u})for(unsigned tc:{13u,16u,18u}) {
        const auto stem="png-out-"+std::to_string(depth)+"-"+std::to_string(cp)+"-"+std::to_string(tc);
        const auto input=dir/(stem+".png");
        write(input,png_chunk(depth==16?raster_png16:raster_png8,"cICP",{uint8_t(cp),uint8_t(tc),0,1}));
        const auto inputData=bytes(input);const auto source=decode_regular(inputData.data(),inputData.size());
        for(auto preset:{jxrc::Preset::lossless,jxrc::Preset::low,jxrc::Preset::custom}) {
            jxrc::Options o;o.target=jxrc::Target::png;o.quality.preset=preset;
            const auto output=dir/(stem+"-"+std::to_string(int(preset))+".png");
            const auto result=jxrc::convert(input,output,o);const auto data=bytes(output);
            const auto copied=decode_regular(data.data(),data.size());
            check(copied.pixels==source.pixels&&copied.bits==depth,"Direct PNG codes or automatic depth changed with preset");
            check(copied.primaries==cp&&copied.transfer==tc&&result.transfer==int(tc),"Direct PNG color tags changed");
            if(tc==13) {
                o.mode=jxrc::Mode::process;
                const auto untouched=dir/(stem+"-process-"+std::to_string(int(preset))+".png");jxrc::convert(input,untouched,o);
                const auto encoded=bytes(untouched);const auto png=decode_regular(encoded.data(),encoded.size());
                check(png.pixels==source.pixels&&png.primaries==cp,"Untouched Process PNG did not retain source codes/primaries");
            }
        }
        if(tc!=13)continue;
        for(unsigned output_depth:{8u,16u})for(bool process:{false,true}) {
            jxrc::Options o;o.target=jxrc::Target::png;o.quality.auto_bit_depth=false;o.quality.png_depth=output_depth;
            o.mode=process?jxrc::Mode::process:jxrc::Mode::preserve;o.max_edge=process?65535:0;
            const auto output=dir/(stem+"-depth-"+std::to_string(output_depth)+(process?"-f32.png":"-direct.png"));
            jxrc::convert(input,output,o);const auto data=bytes(output);const auto converted=decode_regular(data.data(),data.size());
            check(converted.bits==output_depth,"PNG manual depth ignored");
            if(process) {
                std::vector<float> expected,actual;uhdr_color_gamut_t ec,ac;
                regular_to_linear(source,expected,ec);regular_to_linear(converted,actual,ac);
                check(ec==ac,"SDR PNG working primaries changed");
                for(size_t i=0;i<expected.size();++i)check(std::abs(actual[i]-expected[i])<(output_depth==8?.012f:.0001f),"F32 SDR PNG color or alpha changed");
            } else {
                const auto& values=depth==16?expected_png16:expected_png8;
                for(size_t i=0;i<values.size();++i) {
                    const unsigned full=values[i]*(depth==16?1:257);
                    uint16_t value=converted.pixels[i];if(output_depth==16)std::memcpy(&value,converted.pixels.data()+i*2,2);
                    check(value==(output_depth==16?full:(full+128)/257),"Direct PNG sample depth conversion");
                }
            }
        }
    }
    // Fixed expected order covers every EXIF transform independently of the writer.
    const unsigned orders[8][6]={{0,1,2,3,4,5},{2,1,0,5,4,3},{5,4,3,2,1,0},{3,4,5,0,1,2},
        {0,3,1,4,2,5},{3,0,4,1,5,2},{5,2,4,1,3,0},{2,5,1,4,0,3}};
    for(unsigned orientation=1;orientation<=8;++orientation)for(bool keep:{false,true}) {
        std::vector<uint8_t> rotated={'I','I',42,0,8,0,0,0,1,0,0x12,1,3,0,1,0,0,0,uint8_t(orientation),0,0,0,0,0,0,0};
        const auto oriented=dir/"png-rotated-input.png";write(oriented,png_chunk(raster_png16,"eXIf",rotated));
        jxrc::Options o;o.target=jxrc::Target::png;o.keep_exif=keep;
        const auto output=dir/("png-rotated-"+std::to_string(orientation)+(keep?"-exif.png":".png"));const auto result=jxrc::convert(oriented,output,o);
        const auto data=bytes(output);const auto png=decode_regular(data.data(),data.size());
        const unsigned w=orientation>=5?2:3,h=orientation>=5?3:2;
        check(result.width==w&&result.height==h&&png.width==w&&png.height==h&&png.orientation==1,"Direct PNG orientation or dimensions incorrect");
        if(!keep)check(png.exif.empty(),"PNG retained EXIF when disabled");
        for(unsigned i=0;i<6;++i)for(unsigned c=0;c<4;++c) {
            uint16_t value;std::memcpy(&value,png.pixels.data()+(i*4+c)*2,2);
            check(value==expected_png16[orders[orientation-1][i]*4+c],"Direct PNG orientation changed sample codes");
        }
    }
    // PQ anchors at black, 203, 1000 and 10000 nits, including invalid samples.
    const float pqPixels[]={0.f,1.f,1000.f/203.f,.25f,10000.f/203.f,-1.f,INFINITY,NAN};
    const double pqCodes[]={0.,.580688881,.751827096,.25,1.,0.,0.,0.};
    for(unsigned depth:{8u,16u}) {
        const auto output=dir/("png-f32-pq-"+std::to_string(depth)+".png");
        check(encode_png_f32(2,1,pqPixels,UHDR_CG_BT_2100,16,depth,output.string().c_str(),StillEncodeQuality(0))==0,"F32 PQ PNG encode");
        const auto data=bytes(output);const auto png=decode_regular(data.data(),data.size());
        check(png.bits==depth&&png.transfer==16&&png.primaries==9,"F32 PQ PNG metadata");
        for(size_t i=0;i<8;++i){uint16_t value=png.pixels[i];if(depth==16)std::memcpy(&value,png.pixels.data()+i*2,2);check(std::abs(int(value)-int(std::lround(pqCodes[i]*(depth==8?255:65535))))<=1,"F32 PQ PNG reference codes or alpha");}
    }
    // The cICP-aware path and ICC-only readers must see the same SDR primaries.
    // Compare the fallback profile with AVIF's actual SDR output profile.
    const float sdrPixels[]={.1f,.4f,.9f,.25f,.8f,.3f,.2f,1.f};
    const auto profiledPng=dir/"png-bt2020-icc.png",profiledAvif=dir/"png-reference-profile.avif";
    check(encode_png_f32(2,1,sdrPixels,UHDR_CG_BT_2100,13,16,profiledPng.string().c_str(),StillEncodeQuality(0))==0,"SDR PNG ICC export");
    check(encode_avif_f32(2,1,sdrPixels,UHDR_CG_BT_2100,0,profiledAvif.string().c_str(),StillEncodeQuality(0))==0,"SDR AVIF ICC reference");
    auto fallback=bytes(profiledPng);const auto tagged=decode_regular(fallback.data(),fallback.size());bool cicpFound=false,iccFound=false;
    for(size_t at=8;at+12<=fallback.size();) {
        const uint32_t n=uint32_t(fallback[at])<<24|uint32_t(fallback[at+1])<<16|uint32_t(fallback[at+2])<<8|fallback[at+3];
        check(n<=fallback.size()-at-12,"PNG output chunk bounds");
        if(!std::memcmp(fallback.data()+at+4,"iCCP",4))iccFound=true;
        if(!std::memcmp(fallback.data()+at+4,"cICP",4)){fallback.erase(fallback.begin()+at,fallback.begin()+at+n+12);cicpFound=true;}else at+=n+12;
    }
    const auto iccOnly=decode_regular(fallback.data(),fallback.size());
    auto referenceDecoder=std::unique_ptr<avifDecoder,decltype(&avifDecoderDestroy)>(avifDecoderCreate(),avifDecoderDestroy);
    const auto avifData=bytes(profiledAvif);
    check(avifDecoderSetIOMemory(referenceDecoder.get(),avifData.data(),avifData.size())==AVIF_RESULT_OK&&avifDecoderParse(referenceDecoder.get())==AVIF_RESULT_OK,"PNG ICC AVIF reference parse");
    const auto& referenceIcc=referenceDecoder->image->icc;
    check(cicpFound&&iccFound&&tagged.primaries==9&&tagged.transfer==13,"SDR PNG lost BT.2020 cICP or ICC");
    check(!iccOnly.icc.empty()&&iccOnly.icc.size()==referenceIcc.size&&!std::memcmp(iccOnly.icc.data(),referenceIcc.data,referenceIcc.size),"SDR PNG ICC differs from AVIF output");
    check(iccOnly.pixels==tagged.pixels,"PNG ICC fallback changed sample codes");
    std::vector<float> taggedLinear,iccLinear;uhdr_color_gamut_t taggedCg,iccCg;
    regular_to_linear(tagged,taggedLinear,taggedCg);regular_to_linear(iccOnly,iccLinear,iccCg);
    check(taggedCg==iccCg,"PNG ICC fallback primaries differ from cICP");
    // Allow the ICC's fixed-point matrix/TRC and CMS approximation error.
    for(size_t i=0;i<taggedLinear.size();++i)check(std::abs(taggedLinear[i]-iccLinear[i])<.0003f,"PNG ICC fallback color or alpha differs from cICP");
    // Direct exports retain profiles and work across the other source decoders.
    const auto jpeg=dir/"png-source.jpg";encode(jpeg,0,false);const auto jpg=bytes(jpeg);
    const auto icc=source_jpeg_icc(jpg.data(),jpg.size());check(!icc.empty(),"JPEG fixture ICC missing");
    jxrc::Options o;o.target=jxrc::Target::png;
    const auto direct=dir/"png-jpeg-direct.png";jxrc::convert(jpeg,direct,o);
    const auto directData=bytes(direct);const auto directPng=decode_regular(directData.data(),directData.size());
    check(directPng.icc==icc,"Direct JPEG to PNG lost ICC");
    for(auto target:{jxrc::Target::jxl,jxrc::Target::avif}) {
        o.target=target;o.quality.preset=jxrc::Preset::lossless;
        const auto intermediate=dir/(target==jxrc::Target::jxl?"png-source.jxl":"png-source.avif");jxrc::convert(jpeg,intermediate,o);
        o.target=jxrc::Target::png;const auto output=dir/(target==jxrc::Target::jxl?"png-from-jxl.png":"png-from-avif.png");jxrc::convert(intermediate,output,o);
        const auto data=bytes(output);const auto png=decode_regular(data.data(),data.size());
        check(png.icc==icc&&png.pixels==directPng.pixels,"Direct JXL/AVIF to PNG changed source ICC or codes");
    }
    const auto heic=dir/"png-source.heic";write(heic,heic_sdr_fixture);jxrc::convert(heic,dir/"png-from-heic.png",o);
    const auto gain=dir/"png-source-gain.jpg";encode(gain,1);jxrc::convert(gain,dir/"png-source-base.png",o);check(!jxrc::inspect(dir/"png-source-base.png").hdr,"Auto source PNG must export the stored SDR base");
    const auto linear=dir/"png-source-linear.jxl";encode(linear,3);fails([&]{jxrc::convert(linear,dir/"png-clipped-linear.png",o);});
    o.mode=jxrc::Mode::process;o.max_edge=32;jxrc::convert(linear,dir/"png-rendered-linear.png",o);
}

static void hlg_outputs(const fs::path& dir) {
    // Independent BT.2100 forward reference: HLG codes -> scene light ->
    // 1000-nit display light. Encoders must recover these codes from F32 RGB.
    const std::vector<double> codes={0,0,0,0, .5,.5,.5,.25, .75,.75,.75,.5, 1,1,1,1, .2,.4,.7,.75, .9,.6,.3,0};
    for(auto gamut:{UHDR_CG_BT_709,UHDR_CG_DISPLAY_P3,UHDR_CG_BT_2100}) {
        const unsigned cp=gamut==UHDR_CG_BT_2100?9:gamut==UHDR_CG_DISPLAY_P3?12:1;
        const double yr=cp==9?.2627:cp==12?.2289746:.2126,yb=cp==9?.0593:cp==12?.0792869:.0722;
        std::vector<float> linear(codes.size());
        for(size_t i=0;i<codes.size();i+=4) {
            double scene[3];
            for(unsigned c=0;c<3;++c){const double v=codes[i+c];scene[c]=v<=.5?v*v/3.:(std::exp((v-.55991073)/.17883277)+.28466892)/12.;}
            const double scale=std::pow(yr*scene[0]+(1-yr-yb)*scene[1]+yb*scene[2],.2)*1000./203.;
            for(unsigned c=0;c<3;++c)linear[i+c]=float(scene[c]*scale);
            linear[i+3]=float(codes[i+3]);
        }
        const auto original=linear;StillEncodeQuality quality(0);
        const auto stem="hlg-"+std::to_string(cp);
        const auto pngPath=dir/(stem+".png");check(encode_png_f32(3,2,linear.data(),gamut,18,16,pngPath.string().c_str(),quality)==0,"F32 HLG PNG encode");
        const auto pngData=bytes(pngPath);const auto png=decode_regular(pngData.data(),pngData.size());
        check(png.transfer==18&&png.primaries==cp&&png.bits==16,"HLG PNG color tags");
        for(size_t i=0;i<codes.size();++i){uint16_t v;std::memcpy(&v,png.pixels.data()+i*2,2);check(std::abs(int(v)-int(std::lround(codes[i]*65535)))<=1,"HLG PNG inverse OOTF/OETF or alpha");}
        std::vector<float> decoded;uhdr_color_gamut_t decodedGamut;regular_to_linear(png,decoded,decodedGamut);
        for(size_t i=0;i<linear.size();++i)check(std::abs(decoded[i]-linear[i])<.0004f,"HLG PNG F32 round trip brightness");
        const auto jxlPath=dir/(stem+".jxl");check(encode_jxl_f32(3,2,linear.data(),gamut,true,jxlPath.string().c_str(),quality,false,true)==0,"F32 HLG JXL encode");
        const auto jxlData=bytes(jxlPath);auto dec=JxlDecoderMake(nullptr);
        JxlDecoderSubscribeEvents(dec.get(),JXL_DEC_BASIC_INFO|JXL_DEC_COLOR_ENCODING|JXL_DEC_FULL_IMAGE);JxlDecoderSetInput(dec.get(),jxlData.data(),jxlData.size());JxlDecoderCloseInput(dec.get());
        std::vector<uint16_t> samples(codes.size());const JxlPixelFormat format{4,JXL_TYPE_UINT16,JXL_NATIVE_ENDIAN,0};bool complete=false;
        for(;;) {
            const auto status=JxlDecoderProcessInput(dec.get());
            if(status==JXL_DEC_BASIC_INFO){JxlBasicInfo info{};check(JxlDecoderGetBasicInfo(dec.get(),&info)==JXL_DEC_SUCCESS&&info.intensity_target==1000.f&&info.bits_per_sample==16,"HLG JXL reference intensity/depth");}
            else if(status==JXL_DEC_COLOR_ENCODING){JxlColorEncoding color{};check(JxlDecoderGetColorAsEncodedProfile(dec.get(),JXL_COLOR_PROFILE_TARGET_ORIGINAL,&color)==JXL_DEC_SUCCESS&&color.transfer_function==JXL_TRANSFER_FUNCTION_HLG,"HLG JXL transfer");}
            else if(status==JXL_DEC_NEED_IMAGE_OUT_BUFFER)check(JxlDecoderSetImageOutBuffer(dec.get(),&format,samples.data(),samples.size()*2)==JXL_DEC_SUCCESS,"HLG JXL decode buffer");
            else if(status==JXL_DEC_FULL_IMAGE){complete=true;break;}
            else if(status==JXL_DEC_SUCCESS)break;
            else check(status!=JXL_DEC_ERROR&&status!=JXL_DEC_NEED_MORE_INPUT,"HLG JXL decode");
        }
        check(complete,"HLG JXL incomplete");for(size_t i=0;i<codes.size();++i)check(std::abs(int(samples[i])-int(std::lround(codes[i]*65535)))<=1,"HLG JXL encoded codes changed");
        if(cp==9) {
            unsigned w=0,h=0;uhdr_color_gamut_t cg;bool hdr=false;int transfer=0;std::vector<float> decodedJxl;
            check(decode_jxl_to_linear_f32(jxlData.data(),jxlData.size(),decodedJxl,w,h,&cg,&hdr,&transfer,false,nullptr)==0&&transfer==18,"HLG JXL F32 decode");
            for(size_t i=0;i<linear.size();++i)check(std::abs(decodedJxl[i]-linear[i])<.001f,"HLG JXL round trip brightness");
        }
        for(unsigned depth:{8u,10u,12u}) {
            StillEncodeQuality avifQuality(4,0,100,depth);
            const auto path=dir/(stem+"-"+std::to_string(depth)+".avif");
            check(encode_avif_f32(3,2,linear.data(),gamut,3,path.string().c_str(),avifQuality)==0,"F32 HLG AVIF encode");
            auto decoder=std::unique_ptr<avifDecoder,decltype(&avifDecoderDestroy)>(avifDecoderCreate(),avifDecoderDestroy);const auto data=bytes(path);
            check(avifDecoderSetIOMemory(decoder.get(),data.data(),data.size())==AVIF_RESULT_OK&&avifDecoderParse(decoder.get())==AVIF_RESULT_OK&&avifDecoderNextImage(decoder.get())==AVIF_RESULT_OK,"HLG AVIF decode");
            const auto* image=decoder->image;check(image->depth==depth&&image->transferCharacteristics==18&&image->colorPrimaries==cp&&!image->icc.size,"HLG AVIF metadata or selected depth");
            std::vector<uint8_t> packed(codes.size()*(depth==8?1:2));avifRGBImage rgb;avifRGBImageSetDefaults(&rgb,image);rgb.format=AVIF_RGB_FORMAT_RGBA;rgb.depth=depth;rgb.pixels=packed.data();rgb.rowBytes=3*4*(depth==8?1:2);
            check(avifImageYUVToRGB(image,&rgb)==AVIF_RESULT_OK,"HLG AVIF RGB decode");
            for(size_t i=0;i<codes.size();++i){uint16_t value=packed[i];if(depth>8)std::memcpy(&value,packed.data()+i*2,2);check(std::abs(int(value)-int(std::lround(codes[i]*((1u<<depth)-1))))<=1,"HLG AVIF inverse OOTF/OETF or alpha");}
        }
        check(linear==original,"HLG encoder mutated shared F32 input");
        // Public conversion targets must reach the F32 path and share its render.
        const auto source=dir/(stem+"-linear.jxl");check(encode_jxl_f32(3,2,linear.data(),gamut,true,source.string().c_str(),quality,false)==0,"HLG linear source fixture");
        jxrc::ConversionSession session(source);
        for(auto target:{jxrc::Target::png_hlg,jxrc::Target::jxl_hlg,jxrc::Target::avif_hlg}) {
            jxrc::Options options;options.mode=jxrc::Mode::process;options.target=target;options.quality.preset=jxrc::Preset::lossless;options.exposure=-.25f;
            const auto output=dir/(stem+"-target-"+std::to_string(int(target)));const auto result=session.convert(output,options);const auto info=jxrc::inspect(output);
            check(result.transfer==18&&result.encoding=="hlg"&&info.hdr&&info.transfer=="HLG","HLG target dispatch or inspection");
            options.mode=jxrc::Mode::preserve;options.exposure=0;fails([&]{jxrc::validate(options);});
        }
    }
}

static std::vector<uint8_t> png_fallback_profile(std::vector<uint8_t> data) {
    for(size_t at=8;at+12<=data.size();) {
        const uint32_t n=uint32_t(data[at])<<24|uint32_t(data[at+1])<<16|uint32_t(data[at+2])<<8|data[at+3];
        check(n<=data.size()-at-12,"ICC PNG chunk bounds");
        if(!std::memcmp(data.data()+at+4,"cICP",4))data.erase(data.begin()+at,data.begin()+at+n+12);
        else at+=n+12;
    }
    return decode_regular(data.data(),data.size()).icc;
}

static void check_jxl_color(const std::vector<uint8_t>& data,unsigned cp,unsigned tc) {
    auto decoder=JxlDecoderMake(nullptr);JxlDecoderSubscribeEvents(decoder.get(),JXL_DEC_BASIC_INFO|JXL_DEC_COLOR_ENCODING);
    JxlDecoderSetInput(decoder.get(),data.data(),data.size());JxlDecoderCloseInput(decoder.get());
    JxlBasicInfo info{};
    for(;;) {
        const auto status=JxlDecoderProcessInput(decoder.get());
        if(status==JXL_DEC_BASIC_INFO)check(JxlDecoderGetBasicInfo(decoder.get(),&info)==JXL_DEC_SUCCESS,"Color audit JXL basic info");
        else if(status==JXL_DEC_COLOR_ENCODING) {
            JxlColorEncoding color{};
            check(JxlDecoderGetColorAsEncodedProfile(decoder.get(),JXL_COLOR_PROFILE_TARGET_ORIGINAL,&color)==JXL_DEC_SUCCESS,"JXL native color encoding missing");
            check(color.primaries==(cp==12?JXL_PRIMARIES_P3:static_cast<JxlPrimaries>(cp))&&unsigned(color.transfer_function)==tc&&color.white_point==JXL_WHITE_POINT_D65,"JXL source primaries or transfer incorrect");
            size_t icc_size=0;check(JxlDecoderGetICCProfileSize(decoder.get(),JXL_COLOR_PROFILE_TARGET_ORIGINAL,&icc_size)==JXL_DEC_SUCCESS&&icc_size>128,"JXL cannot expose an ICC profile for its native color encoding");
            check(info.intensity_target>0,"JXL intensity metadata missing");
            if(tc==18)check(info.intensity_target==1000.f,"JXL HLG reference display changed");
            return;
        } else check(status!=JXL_DEC_ERROR&&status!=JXL_DEC_NEED_MORE_INPUT&&status!=JXL_DEC_SUCCESS,"Color audit JXL decode");
    }
}

static void output_color_metadata(const fs::path& dir) {
    std::vector<float> pixels(16*16*4);
    for(size_t i=0;i<pixels.size();i+=4){pixels[i]=4;pixels[i+1]=.25f;pixels[i+2]=.1f;pixels[i+3]=1;}
    for(auto gamut:{UHDR_CG_BT_709,UHDR_CG_DISPLAY_P3,UHDR_CG_BT_2100}) {
        const unsigned cp=gamut==UHDR_CG_BT_2100?9:gamut==UHDR_CG_DISPLAY_P3?12:1;
        const auto reference=dir/("color-reference-"+std::to_string(cp)+".jpg");
        check(encode_still_export_f32(pixels.data(),16,16,cp==9?2:cp==12?1:0,0,reference.string().c_str(),StillEncodeQuality(0))==0,"Color audit ICC reference JPEG");
        const auto reference_data=bytes(reference),expected=source_jpeg_icc(reference_data.data(),reference_data.size());
        check(expected.size()>128&&!std::memcmp(expected.data()+36,"acsp",4),"Color audit reference ICC invalid");
        const auto source=dir/("color-audit-"+std::to_string(cp)+".jxl");
        check(encode_jxl_f32(16,16,pixels.data(),gamut,true,source.string().c_str(),StillEncodeQuality(0),false)==0,"Color audit source");
        jxrc::ConversionSession session(source);
        for(auto preset:{jxrc::Preset::lossless,jxrc::Preset::medium})for(int id=0;id<=13;++id) {
            jxrc::Options options;options.mode=jxrc::Mode::process;options.target=static_cast<jxrc::Target>(id);options.quality.preset=preset;options.extended_bt2020=false;options.convert_gamut=false;
            const auto output=dir/("color-audit-"+std::to_string(cp)+"-"+std::to_string(int(preset))+"-"+std::to_string(id));
            try{session.convert(output,options);}catch(const std::exception& error){throw std::runtime_error(output.filename().string()+": "+error.what());}const auto data=bytes(output);
            if(id==0||id==1) {
                check(source_jpeg_icc(data.data(),data.size())==expected,"JPEG SDR/base ICC does not describe output pixels");
                if(id==1) {
                    const std::string markers(data.begin(),data.end());
                    check(markers.find("hdrgm:Version")!=std::string::npos&&markers.find("urn:iso:std:iso:ts:21496:-1")!=std::string::npos,"Ultra HDR missing XMP or ISO gain-map metadata");
                }
            } else if(id==9||id==10||id==11) {
                const auto png=decode_regular(data.data(),data.size());const unsigned tc=id==9?13:id==10?16:18;
                check(png.primaries==cp&&png.transfer==tc,"PNG CICP incorrect");
                if(id==9)check(png_fallback_profile(data)==expected,"SDR PNG ICC fallback incorrect");
                else check(png_light_chunk(data).size()==2,"HDR PNG missing light levels");
            } else if(id==4||id==5||id==6||id==13) {
                auto decoder=std::unique_ptr<avifDecoder,decltype(&avifDecoderDestroy)>(avifDecoderCreate(),avifDecoderDestroy);
                check(avifDecoderSetIOMemory(decoder.get(),data.data(),data.size())==AVIF_RESULT_OK&&avifDecoderParse(decoder.get())==AVIF_RESULT_OK&&avifDecoderNextImage(decoder.get())==AVIF_RESULT_OK,"Color audit AVIF decode");
                const auto* image=decoder->image;const unsigned tc=id==5?16:id==13?18:13;
                check(image->colorPrimaries==cp&&image->transferCharacteristics==tc&&image->yuvRange==AVIF_RANGE_FULL,"AVIF CICP or range incorrect");
                if(id==4)check(image->icc.size==expected.size()&&!std::memcmp(image->icc.data,expected.data(),expected.size()),"SDR AVIF ICC incorrect");
                else check(image->icc.size==0,"HDR AVIF has a conflicting ICC");
                if(id==6)check(image->gainMap&&image->gainMap->altColorPrimaries==cp&&image->gainMap->altTransferCharacteristics==16&&image->gainMap->altCLLI.maxPALL>0,"ISO gain-map alternate color metadata missing");
                else if(tc==16||tc==18)check(image->clli.maxCLL>0&&image->clli.maxPALL>0,"HDR AVIF missing clli");
            } else check_jxl_color(data,id==8?1:cp,id==2?13:id==7?16:id==12?18:8);
        }
        // CICP-only source transcodes must also offer an SDR ICC fallback.
        const auto raster=dir/("color-cicp-only-"+std::to_string(cp)+".png");
        const uint8_t opaque[]={40,80,120,255,160,60,20,255};StillSourceColor source_color;source_color.primaries=cp;
        check(encode_png_source(2,1,opaque,false,source_color,raster.string().c_str(),StillEncodeQuality(0))==0,"CICP-only opaque source fixture");
        auto unprofiled=bytes(raster);
        for(size_t at=8;at+12<=unprofiled.size();) {
            const uint32_t n=uint32_t(unprofiled[at])<<24|uint32_t(unprofiled[at+1])<<16|uint32_t(unprofiled[at+2])<<8|unprofiled[at+3];
            if(!std::memcmp(unprofiled.data()+at+4,"iCCP",4))unprofiled.erase(unprofiled.begin()+at,unprofiled.begin()+at+n+12);
            else at+=n+12;
        }
        write(raster,unprofiled);
        for(auto target:{jxrc::Target::jpeg,jxrc::Target::png,jxrc::Target::jxl,jxrc::Target::avif}) {
            jxrc::Options options;options.target=target;options.quality.preset=jxrc::Preset::lossless;
            const auto output=dir/("color-direct-"+std::to_string(cp)+"-"+std::to_string(int(target)));try{jxrc::convert(raster,output,options);}catch(const std::exception& error){throw std::runtime_error(output.filename().string()+": "+error.what());}const auto data=bytes(output);
            if(target==jxrc::Target::jpeg)check(source_jpeg_icc(data.data(),data.size())==expected,"Direct JPEG ICC fallback missing");
            else if(target==jxrc::Target::png)check(png_fallback_profile(data)==expected,"Direct PNG ICC fallback missing");
            else if(target==jxrc::Target::jxl)check_jxl_color(data,cp,13);
            else {
                auto decoder=std::unique_ptr<avifDecoder,decltype(&avifDecoderDestroy)>(avifDecoderCreate(),avifDecoderDestroy);
                check(avifDecoderSetIOMemory(decoder.get(),data.data(),data.size())==AVIF_RESULT_OK&&avifDecoderParse(decoder.get())==AVIF_RESULT_OK,"Direct AVIF color decode");
                check(decoder->image->icc.size==expected.size()&&!std::memcmp(decoder->image->icc.data,expected.data(),expected.size()),"Direct AVIF ICC fallback missing");
            }
        }
        for(unsigned tc:{16u,18u})for(unsigned depth:{8u,10u,12u}) {
            const auto output=dir/("color-avif-levels-"+std::to_string(cp)+"-"+std::to_string(tc)+"-"+std::to_string(depth)+".avif");
            StillEncodeQuality quality(4,0,100,depth);
            check(encode_avif_f32(16,16,pixels.data(),gamut,tc==16?1:3,output.string().c_str(),quality)==0,"AVIF quantized HDR metadata fixture");
            const auto data=bytes(output);auto decoder=std::unique_ptr<avifDecoder,decltype(&avifDecoderDestroy)>(avifDecoderCreate(),avifDecoderDestroy);
            check(avifDecoderSetIOMemory(decoder.get(),data.data(),data.size())==AVIF_RESULT_OK&&avifDecoderParse(decoder.get())==AVIF_RESULT_OK&&avifDecoderNextImage(decoder.get())==AVIF_RESULT_OK,"AVIF light-level decode");
            auto* image=decoder->image;std::vector<uint16_t> packed(pixels.size());
            avifRGBImage rgb;avifRGBImageSetDefaults(&rgb,image);rgb.depth=16;rgb.format=AVIF_RGB_FORMAT_RGBA;rgb.alphaPremultiplied=AVIF_FALSE;
            rgb.pixels=reinterpret_cast<uint8_t*>(packed.data());rgb.rowBytes=16*8;
            check(avifImageYUVToRGB(image,&rgb)==AVIF_RESULT_OK,"AVIF light-level RGB decode");
            std::vector<float> linear;uhdr_color_gamut_t decoded_gamut;
            cicp_rgb_to_linear(rgb.pixels,16*16,16,cp,tc,linear,decoded_gamut);
            double peak=0,sum=0;
            for(size_t i=0;i<linear.size();i+=4){const double light=std::min(double(std::max({linear[i],linear[i+1],linear[i+2]}))*203.,tc==16?10000.:1000.);peak=std::max(peak,light);sum+=light;}
            check(std::abs(double(image->clli.maxCLL)-peak)<1.1&&std::abs(double(image->clli.maxPALL)-sum/256.)<1.1,"AVIF clli does not describe its quantized HDR codes");
            // A still AVIF transcode retains its valid source content-light box.
            jxrc::Options options;options.target=jxrc::Target::avif;options.quality.preset=jxrc::Preset::lossless;
            const auto copy=dir/(output.filename().string()+"-copy.avif");try{jxrc::convert(output,copy,options);}catch(const std::exception& error){throw std::runtime_error(copy.filename().string()+": "+error.what());}
            const auto copied=bytes(copy);auto decoded_copy=std::unique_ptr<avifDecoder,decltype(&avifDecoderDestroy)>(avifDecoderCreate(),avifDecoderDestroy);
            check(avifDecoderSetIOMemory(decoded_copy.get(),copied.data(),copied.size())==AVIF_RESULT_OK&&avifDecoderParse(decoded_copy.get())==AVIF_RESULT_OK,"AVIF light-level copy decode");
            check(decoded_copy->image->clli.maxCLL==image->clli.maxCLL&&decoded_copy->image->clli.maxPALL==image->clli.maxPALL,"Source AVIF clli not preserved");
            if(depth==12) {
                const auto png_source=dir/(output.filename().string()+"-source.png"),from_png=dir/(output.filename().string()+"-from-png.avif");
                check(encode_png_f32(16,16,pixels.data(),gamut,tc,16,png_source.string().c_str(),quality)==0,"HDR PNG direct-source fixture");
                jxrc::convert(png_source,from_png,options);const auto direct_data=bytes(from_png);
                auto direct_decoder=std::unique_ptr<avifDecoder,decltype(&avifDecoderDestroy)>(avifDecoderCreate(),avifDecoderDestroy);
                check(avifDecoderSetIOMemory(direct_decoder.get(),direct_data.data(),direct_data.size())==AVIF_RESULT_OK&&avifDecoderParse(direct_decoder.get())==AVIF_RESULT_OK,"Direct HDR PNG to AVIF metadata parse");
                check(direct_decoder->image->transferCharacteristics==tc&&!direct_decoder->image->icc.size&&direct_decoder->image->clli.maxCLL>0&&direct_decoder->image->clli.maxPALL>0,"Direct HDR AVIF missing CICP or clli");
            }
        }
        // The alternate HDR light-level average must use maxRGB, not (R+G+B)/3.
        for(bool half:{false,true}) {
            const auto output=dir/("color-gain-levels-"+std::to_string(cp)+"-"+std::to_string(half)+".avif");
            auto values=std::vector<uint16_t>(pixels.size());std::transform(pixels.begin(),pixels.end(),values.begin(),float_to_half);
            check((half?encode_avif_f16(16,16,values.data(),gamut,2,output.string().c_str(),StillEncodeQuality(0)):
                encode_avif_f32(16,16,pixels.data(),gamut,2,output.string().c_str(),StillEncodeQuality(0)))==0,"Gain-map metadata fixture");
            const auto data=bytes(output);auto decoder=std::unique_ptr<avifDecoder,decltype(&avifDecoderDestroy)>(avifDecoderCreate(),avifDecoderDestroy);
            check(avifDecoderSetIOMemory(decoder.get(),data.data(),data.size())==AVIF_RESULT_OK&&avifDecoderParse(decoder.get())==AVIF_RESULT_OK&&decoder->image->gainMap,"Gain-map metadata decode");
            const auto light=decoder->image->gainMap->altCLLI;
            check(light.maxCLL==812&&light.maxPALL==812,"ISO alternate MaxFALL is not the frame average of maxRGB");
        }
    }
}

static Exiv2::ExifData photographic_fixture() {
    Exiv2::ExifData exif;
    exif["Exif.Image.Make"]="JXRC";exif["Exif.Image.Model"]="Test Camera";
    exif["Exif.Image.Artist"]="Photographer";exif["Exif.Image.Orientation"]=uint16_t(6);
    exif["Exif.Photo.DateTimeOriginal"]="2025:06:12 14:30:00";
    exif["Exif.Photo.ExposureTime"]="1/125";exif["Exif.Photo.FNumber"]="28/10";
    exif["Exif.Photo.ISOSpeedRatings"]=uint16_t(400);exif["Exif.Photo.LensModel"]="Prime 50";
    exif["Exif.GPSInfo.GPSLatitudeRef"]="N";exif["Exif.GPSInfo.GPSLatitude"]="25/1 2/1 0/1";
    exif["Exif.GPSInfo.GPSLongitudeRef"]="E";exif["Exif.GPSInfo.GPSLongitude"]="121/1 30/1 0/1";
    return exif;
}
static void exif_retention(const fs::path& dir) {
    std::vector<fs::path> sources;
    for(const auto& item:std::vector<std::pair<const char*,std::vector<uint8_t>>>{{"tiff",raster_tiff},{"png",raster_png8},{"webp",raster_webp}}) {
        const auto path=dir/(std::string("photo.")+item.first);write(path,item.second);sources.push_back(path);
    }
    sources.push_back(dir/"photo.jpg");encode(sources.back(),0,false);
    sources.push_back(dir/"photo.dng");dng(sources.back());
    for(const auto& path:sources) {
        auto image=Exiv2::ImageFactory::open(path.string());image->readMetadata();
        auto metadata=image->exifData();for(const auto& tag:photographic_fixture())metadata.add(tag);
        image->setExifData(metadata);image->writeMetadata();
    }
    for(auto target:{jxrc::Target::avif,jxrc::Target::jxl}) {
        jxrc::Options options;options.target=target;options.keep_exif=true;
        const auto path=dir/(target==jxrc::Target::avif?"photo.avif":"photo.jxl");jxrc::convert(dir/"photo.jpg",path,options);sources.push_back(path);
    }
    auto exif=photographic_fixture();Exiv2::Blob tiff;Exiv2::ExifParser::encode(tiff,Exiv2::littleEndian,exif);
    // Exercise XR's different TIFF magic and offset-based EXIF/GPS IFDs.
    auto xr=tiff;xr[2]=0xbc;xr[3]=1;
    auto extracted=portable_source_exif(xr.data(),xr.size(),64,48);
    Exiv2::ExifData xrExif;Exiv2::ExifParser::decode(xrExif,extracted.data(),extracted.size());
    check(xrExif["Exif.Photo.DateTimeOriginal"].toString()=="2025:06:12 14:30:00","JXR photographic directory lost");
    check(xrExif.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSLatitude"))!=xrExif.end(),"JXR GPS directory lost");
    const auto xrPath=dir/"photo.jxr";write_jxr_exif_fixture(xrPath,tiff);sources.push_back(xrPath);
    // A real HDR source catches the Ultra HDR encoder's missing EXIF wiring.
    StillEncodeQuality quality(1);quality.exif=tiff.data();quality.exif_size=tiff.size();
    auto pixels=fixture();const auto hdr=dir/"photo-hdr.jxl";
    check(encode_still_export(reinterpret_cast<const uint8_t*>(pixels.data()),pixels.size()*2,64,48,true,2,3,false,hdr.string().c_str(),0,quality,false)==0,"HDR EXIF fixture encode");sources.push_back(hdr);
    for(const auto& input:sources) {
        jxrc::ConversionSession session(input);
        for(int target=0;target<=8;++target) {
            // Ultra HDR gain maps need larger fixtures; the JPEG/RAW/HDR JXL
            // sources below exercise both gain-map output encoders.
            if((target==1||target==6)&&jxrc::inspect(input).width<8)continue;
            jxrc::Options options;options.mode=jxrc::Mode::process;options.target=jxrc::Target(target);options.keep_exif=true;options.max_edge=32;
            std::cout<<"EXIF "<<input.filename()<<" target "<<target<<std::endl;
            const auto path=dir/(input.filename().string()+"-"+std::to_string(target));const auto result=session.convert(path,options);
            if(input==hdr&&target==1)check(jxrc::inspect(path).gainmap,"HDR EXIF test used SDR JPEG fallback");
            const auto data=bytes(path);auto output=Exiv2::ImageFactory::open(data.data(),data.size());output->readMetadata();const auto& tags=output->exifData();
            auto value=[&](const char* key){const auto it=tags.findKey(Exiv2::ExifKey(key));check(it!=tags.end(),key);return it->toString();};
            check(value("Exif.Image.Make")=="JXRC","EXIF camera make lost");
            check(value("Exif.Photo.DateTimeOriginal")=="2025:06:12 14:30:00","EXIF date lost");
            check(value("Exif.Photo.LensModel")=="Prime 50","EXIF lens lost");
            check(value("Exif.Image.Orientation")=="1","Processed EXIF orientation stale");
            check(value("Exif.Photo.PixelXDimension")==std::to_string(result.width),"EXIF output width stale");
            check(value("Exif.Photo.PixelYDimension")==std::to_string(result.height),"EXIF output height stale");
            value("Exif.GPSInfo.GPSLatitude");value("Exif.Photo.ExposureTime");
            check(tags.findKey(Exiv2::ExifKey("Exif.Image.StripOffsets"))==tags.end(),"Raster offsets copied into EXIF");
            options.keep_exif=false;session.convert(dir/(path.filename().string()+"-stripped"),options);
            const auto stripped=bytes(dir/(path.filename().string()+"-stripped"));
            if(stripped.size()>=2&&stripped[0]==255&&stripped[1]==10)continue; // Raw JXL codestream has no metadata boxes.
            auto noExif=Exiv2::ImageFactory::open(stripped.data(),stripped.size());noExif->readMetadata();
            check(noExif->exifData().findKey(Exiv2::ExifKey("Exif.Image.Make"))==noExif->exifData().end(),"Keep EXIF off retained photography");
        }
    }
    // This TIFF fixture has transparency, which preservation cannot put in JPEG.
    for(auto target:{jxrc::Target::avif,jxrc::Target::jxl}) {
        jxrc::Options options;options.target=target;options.keep_exif=true;
        const auto path=dir/("preserved-tiff-"+std::to_string(int(target)));jxrc::convert(sources.front(),path,options);
        const auto data=bytes(path);auto image=Exiv2::ImageFactory::open(data.data(),data.size());image->readMetadata();
        check(image->exifData()["Exif.Image.Make"].toString()=="JXRC","Preserved TIFF EXIF lost");
    }
}
static void heif_source_color(const fs::path& dir) {
    // Keep the tiny coded picture, varying only the container's color tags.
    // Matrix 0 exercises libheif's GBR packing shortcut, which can omit NCLX
    // on the returned RGB image; matrix 6 exercises ordinary YCbCr conversion.
    const uint8_t signature[]={'n','c','l','x'};
    const auto found=std::search(heic_sdr_fixture.begin(),heic_sdr_fixture.end(),signature,signature+4);
    check(found!=heic_sdr_fixture.end(),"HEIC fixture NCLX missing");
    const size_t at=size_t(found-heic_sdr_fixture.begin())+4;
    for(unsigned matrix:{0u,6u}) {
        auto fixture=heic_sdr_fixture;fixture[at+5]=uint8_t(matrix);
        const auto original=decode_heif(fixture.data(),fixture.size()).base;
        for(unsigned cp:{1u,9u,12u})for(unsigned tc:{8u,13u,16u,18u}) {
            fixture[at+1]=uint8_t(cp);fixture[at+3]=uint8_t(tc);
            const auto source=decode_heif(fixture.data(),fixture.size()).base;
            check(source.primaries==cp&&source.transfer==tc,"HEIC decode lost source color tags");
            check(source.pixels==original.pixels,"HEIC decode changed transfer-encoded source codes");
            const auto small=decode_heif(fixture.data(),fixture.size(),1).base;
            check(small.primaries==cp&&small.transfer==tc,"HEIC thumbnail lost source color tags");
            std::vector<float> actual,expected;unsigned w=0,h=0;uhdr_color_gamut_t gamut,expectedGamut;
            check(decode_heif_to_linear_f32(fixture.data(),fixture.size(),actual,w,h,&gamut,nullptr)==0,"HEIC linear decode");
            cicp_rgb_to_linear(original.pixels.data(),size_t(original.width)*original.height,original.bits,cp,tc,expected,expectedGamut);
            // Ordinary SDR BT.709 can use the ICC pipeline's BT.2020 working
            // gamut. HDR and wide-gamut source tags must reach the CICP path.
            if(tc==16||tc==18||cp!=1) {
                check(gamut==expectedGamut&&actual.size()==expected.size(),"HEIC linear source gamut");
                for(size_t i=0;i<actual.size();++i)check(std::abs(actual[i]-expected[i])<.0001f,"HEIC source brightness changed");
            }
            if(tc==16||tc==18) {
                const auto input=dir/"color.heic",output=dir/"color.png";write(input,fixture);
                jxrc::Options options;options.mode=jxrc::Mode::preserve;options.target=jxrc::Target::png;options.overwrite=true;
                jxrc::convert(input,output,options);
                const auto encoded=bytes(output);const auto png=decode_regular(encoded.data(),encoded.size());
                check(png.primaries==cp&&png.transfer==tc&&png.pixels==original.pixels,"HEIC preservation lost HDR color or codes");
            }
        }
    }
}
static void heif_gainmaps(const fs::path& dir,const fs::path& jpeg_gain) {
    heif_source_color(dir);
    const auto sdr=dir/"sdr.heic",apple=dir/"apple.heic",oppo=dir/"oppo.heic";write(sdr,heic_sdr_fixture);write(apple,heic_apple_fixture);
    check(is_heif_input(heic_sdr_fixture.data(),heic_sdr_fixture.size()),"HEIC brand detection");
    auto info=jxrc::inspect(sdr);check(info.width==2&&info.height==2&&!info.hdr&&info.bits==8,"HEIC source inspection");
    const auto source=decode_heif(heic_apple_fixture.data(),heic_apple_fixture.size());
    check(!source.gain.pixels.empty()&&std::abs(source.max_boost-std::exp2(1.5909f))<.001f,"Apple mixed-endian headroom/gain map");
    auto preview=jxrc::preview(apple,1);check(preview.width==1&&preview.height==1&&preview.rgba.size()==4,"HEIC gain-map preview");
    auto jpeg=bytes(jpeg_gain);auto* decoder=uhdr_create_decoder();check(decoder,"Cannot allocate gain fixture decoder");
    std::unique_ptr<uhdr_codec_private_t,decltype(&uhdr_release_decoder)> owner(decoder,uhdr_release_decoder);
    uhdr_compressed_image_t compressed{jpeg.data(),jpeg.size(),jpeg.size(),UHDR_CG_UNSPECIFIED,UHDR_CT_UNSPECIFIED,UHDR_CR_FULL_RANGE};
    check(uhdr_dec_set_image(decoder,&compressed).error_code==UHDR_CODEC_OK&&uhdr_dec_probe(decoder).error_code==UHDR_CODEC_OK,"Gain fixture probe");
    const auto* map=uhdr_dec_get_gainmap_image(decoder);const auto* metadata=uhdr_dec_get_gainmap_metadata(decoder);check(map&&metadata,"Gain fixture metadata");
    auto proxdr=heic_sdr_fixture;proxdr.resize(proxdr.size()+84);const auto* mapBytes=static_cast<const uint8_t*>(map->data);proxdr.insert(proxdr.end(),mapBytes,mapBytes+map->data_sz);proxdr.resize(proxdr.size()+16);
    const size_t at=proxdr.size();proxdr.resize(at+144);
    auto putFloat=[&](size_t offset,float value){uint32_t code;std::memcpy(&code,&value,4);for(unsigned i=0;i<4;++i)proxdr[at+offset+i]=uint8_t(code>>(8*i));};
    putFloat(16,metadata->max_content_boost[0]);putFloat(132,metadata->hdr_capacity_max);for(const auto c:std::string("[{\"name\":\"local.hdr.linear.mask\"}]"))proxdr.push_back(uint8_t(c));
    proxdr.push_back(0);for(const auto c:std::string("jxrs"))proxdr.push_back(uint8_t(c));proxdr.resize(proxdr.size()+4);write(oppo,proxdr);
    check(jxrc::inspect(oppo).gainmap&&jxrc::inspect(apple).gainmap,"HEIC gain-map header inspection");
    for(const auto& input:{sdr,apple,oppo})for(auto target:{jxrc::Target::jpeg,jxrc::Target::avif}){
        jxrc::Options options;options.target=target;options.quality.preset=jxrc::Preset::lossless;
        const auto preserved=dir/(input.stem().string()+"-preserve-"+std::to_string(int(target)));
        const auto processed=dir/(input.stem().string()+"-auto-"+std::to_string(int(target)));
        const auto result=jxrc::convert(input,preserved,options);check(result.gainmap==(input!=sdr),"HEIC source gain map lost");
        options.mode=jxrc::Mode::process;const auto automatic=jxrc::convert(input,processed,options);
        check(automatic.gainmap==result.gainmap&&bytes(processed)==bytes(preserved),"HEIC automatic source export changed the base/map");
    }
    // Real camera files can carry a neutral maximum gain of exactly 1.
    putFloat(16,1.f);write(oppo,proxdr);
    const auto neutral=decode_heif(proxdr.data(),proxdr.size());
    check(neutral.max_boost==1.f&&!neutral.gain.pixels.empty(),"Neutral ProXDR map rejected or discarded");
    check(jxrc::inspect(oppo).gainmap,"Neutral ProXDR inspection");
    for(auto target:{jxrc::Target::jpeg,jxrc::Target::avif,jxrc::Target::png,jxrc::Target::jxl_linear}) {
        jxrc::Options options;options.target=target;options.mode=jxrc::Mode::process;
        options.gain_map_reconstruction=jxrc::GainMapReconstruction::full_image;
        jxrc::convert(oppo,dir/("neutral-"+std::to_string(int(target))),options);
    }
    for(float invalid:{0.f,.5f,65.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        putFloat(16,invalid);fails([&]{inspect_heif(proxdr.data(),proxdr.size());});
    }
    putFloat(16,1.f);
    auto truncated=proxdr;truncated.erase(truncated.begin()+at+140,truncated.begin()+at+144);
    fails([&]{inspect_heif(truncated.data(),truncated.size());});
    jxrc::Options options;options.mode=jxrc::Mode::process;options.gain_map_reconstruction=jxrc::GainMapReconstruction::full_image;options.target=jxrc::Target::jxl_linear;options.exposure=1;
    jxrc::convert(apple,dir/"apple-rendered.jxl",options);check(jxrc::inspect(dir/"apple-rendered.jxl").hdr,"HEIC float32 pixel-processing path");
}

static void transparency_background(const fs::path& dir) {
    // Opaque inputs must retain their source codes/profile and direct routes,
    // even with Remove alpha selected. In particular, do not round-trip their
    // 16-bit colors through the F32 working gamut or disable JPEG transcoding.
    std::vector<uint16_t> opaque(16*12*4);
    for(size_t i=0;i<opaque.size();++i)opaque[i]=i%4==3?65535:uint16_t((i*7919)%65536);
    StillSourceColor opaqueColor;opaqueColor.bits=16;
    const auto opaquePng=dir/"background-opaque.png";
    check(encode_png_source(16,12,opaque.data(),true,opaqueColor,opaquePng.string().c_str(),StillEncodeQuality(0))==0,"Opaque source fixture");
    jxrc::Options sourceOptions;sourceOptions.mode=jxrc::Mode::process;sourceOptions.target=jxrc::Target::jxl;sourceOptions.quality.preset=jxrc::Preset::lossless;
    const auto opaqueJxl=dir/"background-opaque.jxl";jxrc::convert(opaquePng,opaqueJxl,sourceOptions);
    const auto referenceBytes=bytes(opaquePng);const auto reference=decode_regular(referenceBytes.data(),referenceBytes.size());
    for(const auto& source:{opaquePng,opaqueJxl})for(auto mode:{jxrc::Mode::preserve,jxrc::Mode::process}) {
        auto o=sourceOptions;o.mode=mode;o.target=jxrc::Target::png;o.transparency_background=jxrc::TransparencyBackground::white_remove_alpha;
        auto baselineOptions=o;baselineOptions.transparency_background=jxrc::TransparencyBackground::black;
        const auto baseline=dir/(source.filename().string()+"-baseline-"+std::to_string(int(mode))+".png");jxrc::convert(source,baseline,baselineOptions);
        const auto baselineBytes=bytes(baseline);const auto baselineImage=decode_regular(baselineBytes.data(),baselineBytes.size());
        const auto output=dir/(source.filename().string()+"-opaque-"+std::to_string(int(mode))+".png");jxrc::convert(source,output,o);
        const auto encoded=bytes(output);const auto decoded=decode_regular(encoded.data(),encoded.size());
        check(encoded[25]==2&&decoded.bits==16,"Opaque remove-alpha PNG layout/depth");
        check(decoded.pixels==reference.pixels,"Remove alpha processed opaque source color codes");
        check(decoded.icc==baselineImage.icc,"Remove alpha processed opaque source ICC");
    }
    sourceOptions.target=jxrc::Target::jpeg;const auto opaqueJpeg=dir/"background-opaque.jpg";jxrc::convert(opaquePng,opaqueJpeg,sourceOptions);
    sourceOptions.target=jxrc::Target::jxl;sourceOptions.jpeg_transcoding=true;
    const auto coefficients=dir/"background-coefficients.jxl",removedCoefficients=dir/"background-coefficients-removed.jxl";
    jxrc::convert(opaqueJpeg,coefficients,sourceOptions);sourceOptions.transparency_background=jxrc::TransparencyBackground::white_remove_alpha;
    jxrc::convert(opaqueJpeg,removedCoefficients,sourceOptions);
    check(bytes(coefficients)==bytes(removedCoefficients),"Remove alpha disabled opaque JPEG coefficient transcoding");
    // Wide uniform bands keep JPEG boundary ringing away from sampled pixels.
    const unsigned w=96,h=32;std::vector<float> pixels(size_t(w)*h*4);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
        auto* p=pixels.data()+(size_t(y)*w+x)*4;
        p[0]=p[1]=p[2]=.25f;p[3]=float(x/32)*.5f;
    }
    const auto input=dir/"background-input.png";
    check(encode_png_f32(w,h,pixels.data(),UHDR_CG_BT_709,13,16,input.string().c_str(),StillEncodeQuality(0))==0,"Background PNG fixture");
    jxrc::ConversionSession session(input);
    for(auto target:{jxrc::Target::jpeg,jxrc::Target::jpeg_gainmap})for(bool white:{false,true}) {
        jxrc::Options o;o.mode=jxrc::Mode::process;o.target=target;o.quality.preset=jxrc::Preset::lossless;
        o.transparency_background=white?jxrc::TransparencyBackground::white:jxrc::TransparencyBackground::black;
        const auto name=std::to_string(int(target))+(white?"-white":"-black");
        const auto jpeg=dir/(name+".jpg"),png=dir/(name+".png");session.convert(jpeg,o);
        jxrc::Options unpack;unpack.target=jxrc::Target::png;unpack.gain_map_reconstruction=jxrc::GainMapReconstruction::off;
        jxrc::convert(jpeg,png,unpack);
        const auto data=bytes(png);const auto decoded=decode_regular(data.data(),data.size());
        check(decoded.bits==8,"Background JPEG base depth");
        for(unsigned band=0;band<3;++band) {
            const float alpha=band*.5f,linear=.25f*alpha+(white?1.f:0.f)*(1.f-alpha);
            const int expected=linear_to_srgb_u8(linear);
            const size_t at=(size_t(16)*w+band*32+16)*4;
            for(unsigned c=0;c<3;++c)check(std::abs(int(decoded.pixels[at+c])-expected)<=3,"JPEG transparency background or linear compositing changed");
        }
        const auto fresh=dir/(name+"-fresh.jpg");jxrc::convert(input,fresh,o);
        check(bytes(fresh)==bytes(jpeg),"Background selection leaked between session outputs");
    }
    for(auto target:{jxrc::Target::png,jxrc::Target::png_hdr,jxrc::Target::png_hlg,
                     jxrc::Target::jxl,jxrc::Target::jxl_linear,jxrc::Target::jxl_scrgb,jxrc::Target::jxl_pq,jxrc::Target::jxl_hlg,
                     jxrc::Target::avif,jxrc::Target::avif_pq,jxrc::Target::avif_hlg,jxrc::Target::avif_gainmap}) {
        for(auto background:{jxrc::TransparencyBackground::white,jxrc::TransparencyBackground::black,
                             jxrc::TransparencyBackground::white_remove_alpha,jxrc::TransparencyBackground::black_remove_alpha}) {
            const bool remove=background==jxrc::TransparencyBackground::white_remove_alpha||background==jxrc::TransparencyBackground::black_remove_alpha;
            const bool white=background==jxrc::TransparencyBackground::white||background==jxrc::TransparencyBackground::white_remove_alpha;
            jxrc::Options o;o.mode=jxrc::Mode::process;o.target=target;o.transparency_background=background;o.quality.preset=jxrc::Preset::lossless;
            const auto output=dir/("alpha-"+std::to_string(int(target))+"-"+std::to_string(int(background)));
            session.convert(output,o);const auto encoded=bytes(output);
            if(background==jxrc::TransparencyBackground::black) {
                const auto whiteOutput=dir/("alpha-"+std::to_string(int(target))+"-"+std::to_string(int(jxrc::TransparencyBackground::white)));
                check(encoded==bytes(whiteOutput),"White/Black changed a non-JPEG conversion");
            }
            if(remove&&(target==jxrc::Target::png||target==jxrc::Target::jxl||target==jxrc::Target::avif)) {
                auto preserve=o;preserve.mode=jxrc::Mode::preserve;
                const auto preserved=dir/(output.filename().string()+"-preserve");jxrc::convert(input,preserved,preserve);
                check(bytes(preserved)==encoded,"Remove alpha was bypassed in preserve mode");
            }
            std::vector<float> decoded;unsigned dw=0,dh=0;uhdr_color_gamut_t gamut;int gain=0,transfer=0;bool hdr=false;
            if(target==jxrc::Target::png||target==jxrc::Target::png_hdr||target==jxrc::Target::png_hlg) {
                const auto raster=decode_regular(encoded.data(),encoded.size());regular_to_linear(raster,decoded,gamut);
                if(remove)check(encoded[25]==2,"PNG remove alpha retained an alpha channel");
            } else if(target==jxrc::Target::avif||target==jxrc::Target::avif_pq||target==jxrc::Target::avif_hlg||target==jxrc::Target::avif_gainmap) {
                check(decode_avif_to_linear_f32(encoded.data(),encoded.size(),decoded,dw,dh,&gain,&transfer,&gamut,nullptr)==0,"Background AVIF decode");
                auto* decoder=avifDecoderCreate();check(decoder!=nullptr,"Background AVIF decoder");
                check(avifDecoderSetIOMemory(decoder,encoded.data(),encoded.size())==AVIF_RESULT_OK&&avifDecoderParse(decoder)==AVIF_RESULT_OK&&avifDecoderNextImage(decoder)==AVIF_RESULT_OK,"Background AVIF planes");
                if(remove)check(!decoder->image->alphaPlane,"AVIF remove alpha retained an alpha plane");
                avifDecoderDestroy(decoder);
            } else {
                check(decode_jxl_to_linear_f32(encoded.data(),encoded.size(),decoded,dw,dh,&gamut,&hdr,&transfer,false,nullptr)==0,"Background JXL decode");
                auto decoder=JxlDecoderMake(nullptr);JxlBasicInfo info{};
                check(JxlDecoderSubscribeEvents(decoder.get(),JXL_DEC_BASIC_INFO)==JXL_DEC_SUCCESS,"Background JXL events");
                JxlDecoderSetInput(decoder.get(),encoded.data(),encoded.size());JxlDecoderCloseInput(decoder.get());
                check(JxlDecoderProcessInput(decoder.get())==JXL_DEC_BASIC_INFO&&JxlDecoderGetBasicInfo(decoder.get(),&info)==JXL_DEC_SUCCESS,"Background JXL alpha info");
                if(remove)check(!info.alpha_bits&&!info.num_extra_channels,"JXL remove alpha retained an alpha channel");
            }
            for(unsigned band=0;band<3;++band) {
                const size_t at=(size_t(16)*w+band*32+16)*4;const float a=band*.5f;
                const float expected=remove?.25f*a+(white?1.f:0.f)*(1.f-a):.25f;
                for(unsigned c=0;c<3;++c)check(std::abs(decoded[at+c]-expected)<.025f,"Transparency background changed color");
                check(std::abs(decoded[at+3]-(remove?1.f:a))<.002f,"Transparency background changed alpha");
            }
        }
    }
    // HDR white is SDR reference white (203 nits), independent of the PQ
    // 10000-nit encoding ceiling and the source's brighter foreground.
    auto hdrPixels=pixels;for(size_t i=0;i<hdrPixels.size();++i)if(i%4!=3)hdrPixels[i]=4.f;
    const auto hdrInput=dir/"background-hdr.png";
    check(encode_png_f32(w,h,hdrPixels.data(),UHDR_CG_BT_709,16,16,hdrInput.string().c_str(),StillEncodeQuality(0))==0,"HDR background fixture");
    for(auto target:{jxrc::Target::png_hdr,jxrc::Target::png_hlg,jxrc::Target::avif_pq,jxrc::Target::avif_hlg,jxrc::Target::jxl_linear,jxrc::Target::jxl_scrgb}) {
        jxrc::Options o;o.mode=jxrc::Mode::process;o.target=target;o.quality.preset=jxrc::Preset::lossless;o.transparency_background=jxrc::TransparencyBackground::white_remove_alpha;
        const auto output=dir/("alpha-hdr-white-"+std::to_string(int(target)));jxrc::convert(hdrInput,output,o);
        const auto encoded=bytes(output);std::vector<float> decoded;unsigned dw=0,dh=0;uhdr_color_gamut_t gamut;int gain=0,transfer=0;bool hdr=false;
        if(target==jxrc::Target::png_hdr||target==jxrc::Target::png_hlg)regular_to_linear(decode_regular(encoded.data(),encoded.size()),decoded,gamut);
        else if(target==jxrc::Target::avif_pq||target==jxrc::Target::avif_hlg)check(decode_avif_to_linear_f32(encoded.data(),encoded.size(),decoded,dw,dh,&gain,&transfer,&gamut,nullptr)==0,"HDR background AVIF decode");
        else check(decode_jxl_to_linear_f32(encoded.data(),encoded.size(),decoded,dw,dh,&gamut,&hdr,&transfer,false,nullptr)==0,"HDR background JXL decode");
        for(unsigned band=0;band<3;++band)for(unsigned c=0;c<3;++c) {
            const float alpha=band*.5f,expected=4.f*alpha+(1.f-alpha);
            check(std::abs(decoded[(size_t(16)*w+band*32+16)*4+c]-expected)<.025f,"HDR alpha white is not 203 nits");
        }
    }
    // Preserved scRGB JXR uses 80 nits per unit, unlike the 203-nit working
    // buffer. Its opaque background still needs to decode to 203 nits.
    const auto xr=dir/"background-scrgb.jxr";
    {
        auto require=[](ERR rc){check(!Failed(rc),"Alpha scRGB JXR fixture encode");};
        std::vector<uint8_t> memory(1024*1024);WMPStream* stream=nullptr;PKImageEncode* encoder=nullptr;
        require(CreateWS_Memory(&stream,memory.data(),memory.size()));require(PKImageEncode_Create_WMP(&encoder));
        struct Release {PKImageEncode*& e;~Release(){if(e)e->Release(&e);}} release{encoder};
        CWMIStrCodecParam parameters{};parameters.cfColorFormat=YUV_444;parameters.bdBitDepth=BD_LONG;parameters.bfBitstreamFormat=SPATIAL;parameters.sbSubband=SB_ALL;
        parameters.uiDefaultQPIndex=1;parameters.uiDefaultQPIndexAlpha=1;parameters.uAlphaMode=2;
        require(encoder->Initialize(encoder,stream,&parameters,sizeof(parameters)));
        require(encoder->SetPixelFormat(encoder,GUID_PKPixelFormat64bppRGBAHalf));require(encoder->SetSize(encoder,w,h));require(encoder->SetResolution(encoder,96,96));
        std::vector<uint16_t> half(pixels.size());std::transform(pixels.begin(),pixels.end(),half.begin(),float_to_half);
        require(encoder->WritePixels(encoder,h,reinterpret_cast<U8*>(half.data()),w*8));
        const auto length=std::max(size_t(encoder->WMP.nOffImage)+size_t(encoder->WMP.nCbImage),size_t(encoder->WMP.nOffAlpha)+size_t(encoder->WMP.nCbAlpha));
        memory.resize(length);write(xr,memory);
    }
    const auto xrBytes=bytes(xr);std::vector<float> xrPixels;unsigned xw=0,xh=0;bool halfSource=false;
    check(decode_jxr_scrgb(xrBytes.data(),xrBytes.size(),xrPixels,xw,xh,halfSource)&&halfSource,"Alpha scRGB fixture decode");
    for(auto mode:{jxrc::Mode::preserve,jxrc::Mode::process}) {
        jxrc::Options o;o.mode=mode;o.target=jxrc::Target::jxl_linear;o.convert_gamut=false;o.quality.preset=jxrc::Preset::lossless;o.transparency_background=jxrc::TransparencyBackground::white_remove_alpha;
        const auto output=dir/("alpha-scrgb-white-"+std::to_string(int(mode))+".jxl");jxrc::convert(xr,output,o);
        const auto encoded=bytes(output);std::vector<float> decoded;unsigned dw=0,dh=0;uhdr_color_gamut_t gamut;bool hdr=false;int transfer=0;
        check(decode_jxl_to_linear_f32(encoded.data(),encoded.size(),decoded,dw,dh,&gamut,&hdr,&transfer,false,nullptr)==0,"Alpha scRGB result decode");
        for(size_t i=0;i<decoded.size();i+=4)for(unsigned c=0;c<3;++c) {
            const float alpha=xrPixels[i+3],expected=xrPixels[i+c]*80.f/203.f*alpha+1.f-alpha;
            check(std::abs(decoded[i+c]-expected)<.001f,"scRGB alpha white is not 203 nits");
        }
    }
    // Removing alpha from an existing gain map needs both renditions rebuilt.
    const auto gainSource=dir/("alpha-"+std::to_string(int(jxrc::Target::avif_gainmap))+"-"+std::to_string(int(jxrc::TransparencyBackground::white)));
    jxrc::Options rebuild;rebuild.mode=jxrc::Mode::process;rebuild.target=jxrc::Target::avif_gainmap;
    rebuild.transparency_background=jxrc::TransparencyBackground::white_remove_alpha;
    fails([&]{jxrc::convert(gainSource,dir/"remove-gain-auto.avif",rebuild);});
    rebuild.gain_map_reconstruction=jxrc::GainMapReconstruction::full_image;
    jxrc::convert(gainSource,dir/"remove-gain-full.avif",rebuild);
    const auto rebuilt=bytes(dir/"remove-gain-full.avif");std::vector<float> full;unsigned fw=0,fh=0;int gain=0,transfer=0;uhdr_color_gamut_t gamut;
    check(decode_avif_to_linear_f32(rebuilt.data(),rebuilt.size(),full,fw,fh,&gain,&transfer,&gamut,nullptr)==0&&gain,"Removed gain-map alpha decode");
    for(size_t i=3;i<full.size();i+=4)check(full[i]==1.f,"Reconstructed gain-map alpha was retained");
    jxrc::Options invalid;invalid.transparency_background=static_cast<jxrc::TransparencyBackground>(4);
    fails([&]{jxrc::validate(invalid);});
}

int main(int argc,char** argv) try {
    if(argc==3&&std::string(argv[1])=="--fixture") { encode(fs::u8path(argv[2]),0,false);return 0; }
    const auto dir=fs::temp_directory_path()/("jxrc-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directory(dir);
    struct Clean {fs::path p;~Clean(){std::error_code e;fs::remove_all(p,e);}} clean{dir};
    if(argc==2 && std::string(argv[1])=="--heif") {const auto gain=dir/"gain.jpg";encode(gain,1,true);heif_gainmaps(dir,gain);std::cout<<"HEIC tests passed\n";return 0;}
    if(argc==2 && std::string(argv[1])=="--jpeg-inputs") {jpeg_inputs(dir);return 0;}
    if(argc==2 && std::string(argv[1])=="--hdr-inputs") {hdr_input_tests(dir);return 0;}
    if(argc==2 && std::string(argv[1])=="--gainmap-f32") {gain_map_f32_tests(dir);std::cout<<"Gain-map F32 tests passed\n";return 0;}
    if(argc==2 && std::string(argv[1])=="--color-metadata") {output_color_metadata(dir);shared_output_analysis(dir);std::cout<<"Output color metadata tests passed\n";return 0;}
    if(argc==2 && std::string(argv[1])=="--exif") {exif_retention(dir);std::cout<<"EXIF retention tests passed\n";return 0;}
    if(argc==2 && std::string(argv[1])=="--png-hlg") {png_outputs(dir);hlg_outputs(dir);std::cout<<"PNG output and HLG tests passed\n";return 0;}
    if(argc==2 && std::string(argv[1])=="--transparency") {transparency_background(dir);return 0;}
    if(argc==2 && std::string(argv[1])=="--hdr-png") {hdr_png(dir);std::cout<<"HDR PNG tests passed\n";return 0;}
    if(argc==2 && std::string(argv[1])=="--raw-auto-exposure") {raw_auto_exposure(dir);std::cout<<"RAW auto exposure tests passed\n";return 0;}
    if(argc==2 && std::string(argv[1])=="--pq-jxl-peak") {pq_jxl_peak(dir);std::cout<<"PQ JXL content-peak tests passed\n";return 0;}
    if(argc==2 && std::string(argv[1])=="--srgb-clamp") {srgb_clamping(dir);signed_float_jxl(dir);std::cout<<"sRGB clamp tests passed\n";return 0;}
    // BMFF is a container, not an AVIF signature. Canon CR3 must reach LibRaw.
    const std::vector<uint8_t> cr3_header={0,0,0,24,'f','t','y','p','c','r','x',' ',0,0,0,1,'c','r','x',' ','i','s','o','m'};
    check(!is_avif_input(cr3_header.data(),cr3_header.size()),"CR3 routed to AVIF decoder");
    auto avif_header=cr3_header;std::memcpy(avif_header.data()+8,"avif",4);std::memcpy(avif_header.data()+16,"avif",4);
    check(is_avif_input(avif_header.data(),avif_header.size()),"AVIF major brand rejected");
    std::memcpy(avif_header.data()+8,"mif1",4);
    check(is_avif_input(avif_header.data(),avif_header.size()),"AVIF compatible brand rejected");
    check(!is_avif_input(cr3_header.data(),8),"Truncated BMFF identified as AVIF");
    transparency_background(dir);
    srgb_clamping(dir);
    shared_render(dir);shared_output_analysis(dir);
    signed_float_jxl(dir);
    pq_jxl_peak(dir);
    raw_auto_exposure(dir);
    regular_images(dir);jpeg_inputs(dir);jpeg_avif_paths(dir);
    jxrc::validate({});
    { JxrcThreadBudget budget(1);check(hdr_pixel_jobs(80000000,8)==1,"Pixel thread budget ignored");check(ultrahdr::GetCPUCoreCount()==1,"Ultra HDR thread budget ignored"); }

    for(int mode=0;mode<8;++mode) {
        const auto source=dir/("fixture"+std::to_string(mode));encode(source,mode,mode!=0&&mode!=2&&mode!=4);
        for(auto target:{jxrc::Target::jpeg,jxrc::Target::jpeg_gainmap,jxrc::Target::jxl,jxrc::Target::jxl_linear,jxrc::Target::jxl_pq,jxrc::Target::avif,jxrc::Target::avif_pq,jxrc::Target::avif_gainmap}) {
            jxrc::Options o;o.mode=jxrc::Mode::process;o.gain_map_reconstruction=jxrc::GainMapReconstruction::full_image;o.target=target;o.quality.preset=jxrc::Preset::lossless;
            const auto output=dir/("process-"+std::to_string(mode)+"-"+std::to_string(int(target)));
            jxrc::Result r;try{r=jxrc::convert(source,output,o);}catch(const std::exception& e){throw std::runtime_error("Input mode "+std::to_string(mode)+" target "+std::to_string(int(target))+": "+e.what());}check(r.width==64&&r.height==48,"Processed dimensions");check(fs::file_size(output)>10,"Encoded output");
        }
    }
    std::cout<<"64 process combinations passed\n";
    for(int mode=0;mode<8;++mode) {
        const auto file=dir/("fixture"+std::to_string(mode));
        const auto info=jxrc::inspect(file);check(info.width==64&&info.height==48,"Header inspection dimensions");
        auto preview=jxrc::preview(file,20);check(std::max(preview.width,preview.height)<=20&&preview.rgba.size()==size_t(preview.width)*preview.height*4,"Native preview dimensions");
    }

    const auto jpeg=dir/"fixture0",gain=dir/"fixture1",linear=dir/"fixture3",pq=dir/"fixture7";
    jxrc::Options o;o.target=jxrc::Target::avif_gainmap;o.quality.preset=jxrc::Preset::lossless;
    auto r=jxrc::convert(gain,dir/"preserved.avif",o);check(r.gainmap,"JPEG to AVIF gain map lost");
    o.target=jxrc::Target::jpeg_gainmap;r=jxrc::convert(dir/"preserved.avif",dir/"preserved.jpg",o);check(r.gainmap,"AVIF to JPEG gain map lost");
    for(const auto& input:{gain,dir/"preserved.avif",dir/"fixture6"})for(auto target:{jxrc::Target::jpeg,jxrc::Target::avif}){
        jxrc::Options automatic;automatic.target=target;automatic.quality.preset=jxrc::Preset::lossless;
        const auto name=input.filename().string()+"-"+std::to_string(int(target));
        const auto preserved=dir/(name+"-preserve"),processed=dir/(name+"-auto");
        check(jxrc::convert(input,preserved,automatic).gainmap,"Plain target lost source gain map in Preserve mode");
        automatic.mode=jxrc::Mode::process;check(jxrc::convert(input,processed,automatic).gainmap,"Plain target flattened source gain map");
        check(bytes(preserved)==bytes(processed),"Automatic source base/gain map was processed");
        automatic.gain_map_reconstruction=jxrc::GainMapReconstruction::full_image;automatic.exposure=1;check(!jxrc::convert(input,dir/(name+"-exposure"),automatic).gainmap,"Exposure bypassed pixel processing");
    }
    // Per-channel ISO metadata must survive AVIF -> JPEG; Ultra HDR's XMP
    // writer cannot represent it, so this output must use its ISO packet.
    {
        auto avif=bytes(dir/"fixture6");std::unique_ptr<avifDecoder,decltype(&avifDecoderDestroy)> decoder(avifDecoderCreate(),avifDecoderDestroy);
        check(decoder&&avifDecoderSetIOMemory(decoder.get(),avif.data(),avif.size())==AVIF_RESULT_OK&&avifDecoderParse(decoder.get())==AVIF_RESULT_OK,"Multichannel source parse");
        const auto* map=decoder->image->gainMap;check(map,"Missing multichannel source map");
        auto jpeg=bytes(dir/"fixture6-0-auto");std::unique_ptr<uhdr_codec_private_t,decltype(&uhdr_release_decoder)> output(uhdr_create_decoder(),uhdr_release_decoder);
        uhdr_compressed_image_t compressed{jpeg.data(),jpeg.size(),jpeg.size(),UHDR_CG_UNSPECIFIED,UHDR_CT_UNSPECIFIED,UHDR_CR_FULL_RANGE};
        check(output&&uhdr_dec_set_image(output.get(),&compressed).error_code==UHDR_CODEC_OK&&uhdr_dec_probe(output.get()).error_code==UHDR_CODEC_OK,"Multichannel JPEG map parse");
        const auto* metadata=uhdr_dec_get_gainmap_metadata(output.get());check(metadata,"Multichannel JPEG metadata lost");
        for(unsigned c=0;c<3;++c)check(std::abs(std::log2(metadata->max_content_boost[c])-double(map->gainMapMax[c].n)/map->gainMapMax[c].d)<.0001,"Per-channel gain-map range changed");
    }
    heif_gainmaps(dir,gain);
    gain_map_f32_tests(dir);
    o.target=jxrc::Target::jxl;jxrc::convert(gain,dir/"source-base.jxl",o);check(!jxrc::inspect(dir/"source-base.jxl").hdr,"Auto source JXL must export the stored SDR base");
    o.target=jxrc::Target::avif;r=jxrc::convert(pq,dir/"pq.avif",o);check(r.transfer==16,"Preserved PQ transfer");
    // Linear JXL stores samples normalized to its declared intensity; process round trip restores absolute values.
    o.mode=jxrc::Mode::process;o.target=jxrc::Target::jxl_linear;o.extended_bt2020=false;
    jxrc::convert(linear,dir/"linear.jxl",o);
    auto compressed=bytes(dir/"linear.jxl");std::vector<uint16_t> decoded;unsigned w=0,h=0;uhdr_color_gamut_t cg;bool hdr=false;int transfer=0;
    check(decode_jxl_to_linear_f16(compressed.data(),compressed.size(),decoded,w,h,&cg,&hdr,&transfer,false,0,false,nullptr,nullptr)==0,"Linear round trip decode");
    auto expected=fixture();float error=0;
    for(size_t i=0;i<decoded.size();++i)error=std::max(error,std::abs(half_to_float(decoded[i])-half_to_float(expected[i])));
    check(hdr&&cg==UHDR_CG_BT_2100&&error<0.025f,"Linear HDR values/gamut changed");
    // Gain-map repack retains HDR headroom rather than exporting only an SDR base.
    auto mapped=bytes(dir/"preserved.avif");std::vector<uint16_t> mapped_pixels;int has_map=0;
    check(decode_avif_to_linear_f16(mapped.data(),mapped.size(),mapped_pixels,w,h,&has_map,&transfer,&cg,0,nullptr)==0&&has_map,"Preserved gain-map reconstruction");
    float map_peak=0;for(size_t i=0;i<mapped_pixels.size();++i)if(i%4!=3)map_peak=std::max(map_peak,half_to_float(mapped_pixels[i]));
    check(map_peak>4&&map_peak<8,"Preserved gain-map HDR headroom changed");
    // Straight alpha survives both linear JXL and PQ AVIF processing.
    auto alpha=fixture();for(size_t i=0;i<alpha.size()/4;++i)alpha[i*4+3]=float_to_half((i%3)*0.5f);
    check(encode_still_export(reinterpret_cast<uint8_t*>(alpha.data()),alpha.size()*2,64,48,true,2,3,false,(dir/"alpha.jxl").string().c_str(),0,StillEncodeQuality(0),false)==0,"Alpha fixture");
    for(auto target:{jxrc::Target::jxl_linear,jxrc::Target::avif_pq}) {
        o={};o.mode=jxrc::Mode::process;o.target=target;o.quality.preset=jxrc::Preset::lossless;
        const auto path=dir/("alpha-"+std::to_string(int(target)));jxrc::convert(dir/"alpha.jxl",path,o);
        auto encoded=bytes(path);std::vector<uint16_t> output;
        int rc=target==jxrc::Target::jxl_linear?decode_jxl_to_linear_f16(encoded.data(),encoded.size(),output,w,h,&cg,&hdr,&transfer,false,0,false,nullptr,nullptr):decode_avif_to_linear_f16(encoded.data(),encoded.size(),output,w,h,&has_map,&transfer,&cg,0,nullptr);
        check(rc==0,"Alpha output decode");for(size_t i=3;i<output.size();i+=4)check(std::abs(half_to_float(output[i])-half_to_float(alpha[i]))<0.002f,"Straight alpha changed");
    }
    // Structural orientation survives preservation and is applied once in processing.
    const std::vector<uint8_t> rotated_exif={'I','I',42,0,8,0,0,0,1,0,0x12,1,3,0,1,0,0,0,6,0,0,0,0,0,0,0};
    auto samples=fixture(1);StillEncodeQuality oriented_quality(0);oriented_quality.exif=rotated_exif.data();oriented_quality.exif_size=rotated_exif.size();
    check(encode_still_export(reinterpret_cast<uint8_t*>(samples.data()),samples.size()*2,64,48,true,2,0,false,(dir/"rotated.jpg").string().c_str(),0,oriented_quality,false)==0,"Oriented JPEG fixture");
    for(auto target:{jxrc::Target::avif,jxrc::Target::jxl}) {
        o={};o.target=target;o.keep_exif=true;auto preserved=dir/("oriented-"+std::to_string(int(target)));jxrc::convert(dir/"rotated.jpg",preserved,o);
        o.mode=jxrc::Mode::process;o.target=jxrc::Target::jxl;
        auto oriented=jxrc::convert(preserved,dir/("oriented-process-"+std::to_string(int(target))),o);
        check(oriented.width==48&&oriented.height==64,"Orientation applied incorrectly");
        const auto orientedInfo=jxrc::inspect(dir/("oriented-process-"+std::to_string(int(target))));
        check(orientedInfo.width==48&&orientedInfo.height==64,"Direct integer output orientation lost");
    }
    o={};o.mode=jxrc::Mode::preserve;o.target=jxrc::Target::jxl;o.jpeg_transcoding=true;o.keep_exif=true;
    jxrc::convert(jpeg,dir/"transcoded.jxl",o);
    auto jxl=bytes(dir/"transcoded.jxl");auto dec=JxlDecoderMake(nullptr);
    check(JxlDecoderSubscribeEvents(dec.get(),JXL_DEC_JPEG_RECONSTRUCTION|JXL_DEC_FULL_IMAGE)==JXL_DEC_SUCCESS,"Subscribe reconstruction");
    JxlDecoderSetInput(dec.get(),jxl.data(),jxl.size());JxlDecoderCloseInput(dec.get());std::vector<uint8_t> reconstructed(65536);bool set=false;
    for(;;){auto status=JxlDecoderProcessInput(dec.get());if(status==JXL_DEC_JPEG_RECONSTRUCTION){check(JxlDecoderSetJPEGBuffer(dec.get(),reconstructed.data(),reconstructed.size())==JXL_DEC_SUCCESS,"Set reconstruction buffer");set=true;}else if(status==JXL_DEC_FULL_IMAGE||status==JXL_DEC_SUCCESS)break;else check(status!=JXL_DEC_ERROR&&status!=JXL_DEC_NEED_MORE_INPUT,"JPEG reconstruction failed");}
    check(set,"Missing JPEG reconstruction");reconstructed.resize(reconstructed.size()-JxlDecoderReleaseJPEGBuffer(dec.get()));check(reconstructed==bytes(jpeg),"JPEG byte reconstruction changed");
    o.mode=jxrc::Mode::process;o.quality.preset=jxrc::Preset::low;
    jxrc::convert(jpeg,dir/"transcoded-process.jxl",o);
    check(bytes(dir/"transcoded-process.jxl")==jxl,"Process-mode JPEG transcoding decoded/recompressed pixels or applied quality preset");
    o.exposure=1;fails([&]{jxrc::validate(o);});o.exposure=0;
    o.max_edge=32;fails([&]{jxrc::validate(o);});o.max_edge=0;
    write(dir/"transcode-mixed.png",raster_png16);
    jxrc::convert(dir/"transcode-mixed.png",dir/"transcode-mixed.jxl",o);
    o.jpeg_transcoding=false;jxrc::convert(dir/"transcode-mixed.png",dir/"transcode-mixed-reference.jxl",o);
    check(bytes(dir/"transcode-mixed.jxl")==bytes(dir/"transcode-mixed-reference.jxl"),"JPEG transcoding toggle affected a regular raster");
    std::cout<<"Gain-map, PQ, linear HDR and JPEG reconstruction passed\n";
    o={};o.target=jxrc::Target::jxl;o.quality.preset=jxrc::Preset::lossless;
    jxrc::convert(jpeg,dir/"jpeg-preserved.jxl",o);
    o.mode=jxrc::Mode::process;jxrc::convert(jpeg,dir/"jpeg-direct.jxl",o);
    check(bytes(dir/"jpeg-direct.jxl")==bytes(dir/"jpeg-preserved.jxl"),"JPEG SDR direct pixels changed");
    // Original-profile exports must retain the actual ICC bytes. Merely asking
    // for an ICC size also succeeds when libjxl synthesizes a replacement.
    const auto jpegBytes=bytes(jpeg),sourceIcc=source_jpeg_icc(jpegBytes.data(),jpegBytes.size());
    check(!sourceIcc.empty(),"SDR JPEG fixture missing ICC");
    for(auto preset:{jxrc::Preset::lossless,jxrc::Preset::high,jxrc::Preset::medium,jxrc::Preset::low,jxrc::Preset::custom}) {
        o.quality.preset=preset;
        const auto output=dir/("jpeg-icc-"+std::to_string(int(preset))+".jxl");jxrc::convert(jpeg,output,o);
        const auto encoded=bytes(output);auto dec=JxlDecoderMake(nullptr);
        check(JxlDecoderSubscribeEvents(dec.get(),JXL_DEC_BASIC_INFO|JXL_DEC_COLOR_ENCODING|JXL_DEC_FULL_IMAGE)==JXL_DEC_SUCCESS,"ICC export events");
        JxlDecoderSetInput(dec.get(),encoded.data(),encoded.size());JxlDecoderCloseInput(dec.get());
        std::vector<uint8_t> pixels(64*48*4);const JxlPixelFormat format{4,JXL_TYPE_UINT8,JXL_NATIVE_ENDIAN,0};
        bool saw=false,complete=false;
        for(;;){
            const auto status=JxlDecoderProcessInput(dec.get());
            if(status==JXL_DEC_BASIC_INFO){
                JxlBasicInfo info{};check(JxlDecoderGetBasicInfo(dec.get(),&info)==JXL_DEC_SUCCESS&&info.uses_original_profile,"ICC export did not retain original profile");
            } else if(status==JXL_DEC_COLOR_ENCODING){
                size_t size=0;check(JxlDecoderGetICCProfileSize(dec.get(),JXL_COLOR_PROFILE_TARGET_ORIGINAL,&size)==JXL_DEC_SUCCESS,"ICC export profile size");
                std::vector<uint8_t> actual(size);
                check(JxlDecoderGetColorAsICCProfile(dec.get(),JXL_COLOR_PROFILE_TARGET_ORIGINAL,actual.data(),actual.size())==JXL_DEC_SUCCESS&&actual==sourceIcc,"JXL did not preserve source ICC bytes");
                saw=true;
            } else if(status==JXL_DEC_NEED_IMAGE_OUT_BUFFER)check(JxlDecoderSetImageOutBuffer(dec.get(),&format,pixels.data(),pixels.size())==JXL_DEC_SUCCESS,"ICC export output buffer");
            else if(status==JXL_DEC_FULL_IMAGE){complete=true;break;}
            else if(status==JXL_DEC_SUCCESS)break;
            else check(status!=JXL_DEC_ERROR&&status!=JXL_DEC_NEED_MORE_INPUT,"ICC export pixel decode");
        }
        check(saw&&complete,"ICC export missing profile or image");
    }
    o={};o.mode=jxrc::Mode::process;o.target=jxrc::Target::jxl;o.max_edge=20;o.exposure=1;
    r=jxrc::convert(jpeg,dir/"resized.jxl",o);check(r.width==20&&r.height==15,"Resize dimensions");
    const auto resizedBytes=bytes(dir/"resized.jxl");
    check(jxrc::inspect(dir/"resized.jxl").bits==8 && is_integer_jxl(resizedBytes.data(),resizedBytes.size()),"Processed JPEG SDR JXL is not 8-bit integer");
    const auto previous=bytes(dir/"resized.jxl");
    check(jxrc::convert(jpeg,dir/"resized.jxl",o).skipped,"Existing output was not skipped");
    write(dir/"invalid-source",{1,2,3});
    check(jxrc::convert(dir/"invalid-source",dir/"resized.jxl",o).skipped,"Conflict check decoded the source");
    check(bytes(dir/"resized.jxl")==previous,"Skip modified existing output");
    o.overwrite=true;jxrc::convert(jpeg,dir/"resized.jxl",o);
    fails([&]{jxrc::convert(jpeg,jpeg,o);});
    o.mode=jxrc::Mode::preserve;fails([&]{jxrc::validate(o);});o={};o.raw.shoulder=NAN;fails([&]{jxrc::validate(o);});
    write(dir/"bad",{1,2,3,4});o={};fails([&]{jxrc::convert(dir/"bad",dir/"bad-output",o);});check(!fs::exists(dir/"bad-output"),"Bad input published");
    dng(dir/"sensor.dng");o={};o.mode=jxrc::Mode::process;o.target=jxrc::Target::jxl_linear;
    auto raw=jxrc::convert(dir/"sensor.dng",dir/"raw.jxl",o);check(raw.width>0&&raw.height>0,"RAW decode");
    o.target=jxrc::Target::jxl_scrgb;jxrc::convert(dir/"sensor.dng",dir/"raw-scrgb.jxl",o);
    const auto rawScrgb=bytes(dir/"raw-scrgb.jxl");check(is_scrgb_jxl(rawScrgb.data(),rawScrgb.size()),"RAW to scRGB target");
    o.target=jxrc::Target::jxl_linear;
    o.raw.hdr_mode=jxrc::RawHdrMode::custom;o.raw.exposure=1;o.raw.white_balance=8;o.raw.kelvin=6500;o.raw.shadows=0.2f;o.raw.highlights=-0.1f;o.raw.highlight_stops=1;
    jxrc::convert(dir/"sensor.dng",dir/"raw-adjusted.jxl",o);check(bytes(dir/"raw.jxl")!=bytes(dir/"raw-adjusted.jxl"),"RAW controls ineffective");
    o.target=jxrc::Target::avif_gainmap;jxrc::convert(dir/"sensor.dng",dir/"raw-gain.avif",o);
    std::cout<<"RAW demosaic, controls, paired SDR base and I/O validation passed\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
