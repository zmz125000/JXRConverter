#include "jxrc/converter.hpp"
#include "codec_api.h"
#include "input_format.h"
#include "hdr_parallel.h"
#include "regular_decode.h"
#include "exif_metadata.h"
#include <exiv2/exiv2.hpp>
namespace ultrahdr { unsigned int GetCPUCoreCount(); }
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
static void heif_gainmaps(const fs::path& dir,const fs::path& jpeg_gain) {
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
    const size_t at=proxdr.size();proxdr.resize(at+144+12);
    auto putFloat=[&](size_t offset,float value){uint32_t code;std::memcpy(&code,&value,4);for(unsigned i=0;i<4;++i)proxdr[at+offset+i]=uint8_t(code>>(8*i));};
    putFloat(16,metadata->max_content_boost[0]);putFloat(132,metadata->hdr_capacity_max);for(const auto c:std::string("jxrslocal.hdr"))proxdr.push_back(uint8_t(c));write(oppo,proxdr);
    check(jxrc::inspect(oppo).gainmap&&jxrc::inspect(apple).gainmap,"HEIC gain-map header inspection");
    for(const auto& input:{sdr,apple,oppo})for(auto target:{jxrc::Target::jpeg,jxrc::Target::avif}){
        jxrc::Options options;options.target=target;options.quality.preset=jxrc::Preset::lossless;
        const auto preserved=dir/(input.stem().string()+"-preserve-"+std::to_string(int(target)));
        const auto processed=dir/(input.stem().string()+"-auto-"+std::to_string(int(target)));
        const auto result=jxrc::convert(input,preserved,options);check(result.gainmap==(input!=sdr),"HEIC source gain map lost");
        options.mode=jxrc::Mode::process;const auto automatic=jxrc::convert(input,processed,options);
        check(automatic.gainmap==result.gainmap&&bytes(processed)==bytes(preserved),"HEIC automatic source export changed the base/map");
    }
    jxrc::Options options;options.mode=jxrc::Mode::process;options.target=jxrc::Target::jxl_linear;options.exposure=1;
    jxrc::convert(apple,dir/"apple-rendered.jxl",options);check(jxrc::inspect(dir/"apple-rendered.jxl").hdr,"HEIC float32 pixel-processing path");
}

int main(int argc,char** argv) try {
    if(argc==3&&std::string(argv[1])=="--fixture") { encode(fs::u8path(argv[2]),0,false);return 0; }
    const auto dir=fs::temp_directory_path()/("jxrc-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directory(dir);
    struct Clean {fs::path p;~Clean(){std::error_code e;fs::remove_all(p,e);}} clean{dir};
    if(argc==2 && std::string(argv[1])=="--exif") {exif_retention(dir);std::cout<<"EXIF retention tests passed\n";return 0;}
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
    srgb_clamping(dir);
    shared_render(dir);
    signed_float_jxl(dir);
    pq_jxl_peak(dir);
    raw_auto_exposure(dir);
    regular_images(dir);jpeg_avif_paths(dir);
    jxrc::validate({});
    { JxrcThreadBudget budget(1);check(hdr_pixel_jobs(80000000,8)==1,"Pixel thread budget ignored");check(ultrahdr::GetCPUCoreCount()==1,"Ultra HDR thread budget ignored"); }

    for(int mode=0;mode<8;++mode) {
        const auto source=dir/("fixture"+std::to_string(mode));encode(source,mode,mode!=0&&mode!=2&&mode!=4);
        for(auto target:{jxrc::Target::jpeg,jxrc::Target::jpeg_gainmap,jxrc::Target::jxl,jxrc::Target::jxl_linear,jxrc::Target::jxl_pq,jxrc::Target::avif,jxrc::Target::avif_pq,jxrc::Target::avif_gainmap}) {
            jxrc::Options o;o.mode=jxrc::Mode::process;o.target=target;o.quality.preset=jxrc::Preset::lossless;
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
        automatic.exposure=1;check(!jxrc::convert(input,dir/(name+"-exposure"),automatic).gainmap,"Exposure bypassed pixel processing");
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
    o.target=jxrc::Target::jxl;fails([&]{jxrc::convert(gain,dir/"lost.jxl",o);});check(!fs::exists(dir/"lost.jxl"),"Failed output published");
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
