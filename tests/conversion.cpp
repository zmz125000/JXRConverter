#include "jxrc/converter.hpp"
#include "codec_api.h"
#include "input_format.h"
#include "hdr_parallel.h"
#include "regular_decode.h"
namespace ultrahdr { unsigned int GetCPUCoreCount(); }
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "regular_fixtures.hpp"

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
static void dng(const fs::path& path) {
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
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){auto v=u16(128+(x+y)*100);out.insert(out.end(),v.begin(),v.end());}
    write(path,out);
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
        options.mode=jxrc::Mode::process;options.max_edge=2;options.exposure=1;
        const auto processed=jxrc::convert(input,dir/(std::string(fixture.name)+"-processed.jxl"),options);
        check(processed.width==2 && processed.height==1,"Raster processing resize");
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
    const float hdr[]={-0.125f,2.f,0.5f,0.5f};StillEncodeQuality quality(0);quality.jxl_half=true;
    const auto half=dir/"half-linear.jxl";
    check(encode_jxl_f32(1,1,hdr,UHDR_CG_BT_709,true,half.string().c_str(),quality,false)==0,"Half-float linear JXL encode");
    check(jxrc::inspect(half).bits==16,"Half-float JXR storage choice");
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
    for(auto tags:std::vector<std::vector<uint8_t>>{{9,16,0,0},{9,16,1,1},{2,16,0,1}}) {
        auto bad=png_chunk(raster_png16,"cICP",tags);fails([&]{decode_regular(bad.data(),bad.size());});
    }
    auto bad=png_chunk(raster_png16,"cICP",{9,16,0,1});bad[45]^=1;
    fails([&]{decode_regular(bad.data(),bad.size());}); // CRC, not just chunk contents.
}

int main(int argc,char** argv) try {
    if(argc==3&&std::string(argv[1])=="--fixture") { encode(fs::u8path(argv[2]),0,false);return 0; }
    const auto dir=fs::temp_directory_path()/("jxrc-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directory(dir);
    struct Clean {fs::path p;~Clean(){std::error_code e;fs::remove_all(p,e);}} clean{dir};
    if(argc==2 && std::string(argv[1])=="--hdr-png") {hdr_png(dir);std::cout<<"HDR PNG tests passed\n";return 0;}
    // BMFF is a container, not an AVIF signature. Canon CR3 must reach LibRaw.
    const std::vector<uint8_t> cr3_header={0,0,0,24,'f','t','y','p','c','r','x',' ',0,0,0,1,'c','r','x',' ','i','s','o','m'};
    check(!is_avif_input(cr3_header.data(),cr3_header.size()),"CR3 routed to AVIF decoder");
    auto avif_header=cr3_header;std::memcpy(avif_header.data()+8,"avif",4);std::memcpy(avif_header.data()+16,"avif",4);
    check(is_avif_input(avif_header.data(),avif_header.size()),"AVIF major brand rejected");
    std::memcpy(avif_header.data()+8,"mif1",4);
    check(is_avif_input(avif_header.data(),avif_header.size()),"AVIF compatible brand rejected");
    check(!is_avif_input(cr3_header.data(),8),"Truncated BMFF identified as AVIF");
    shared_render(dir);
    signed_float_jxl(dir);
    regular_images(dir);
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
    }
    o={};o.mode=jxrc::Mode::preserve;o.target=jxrc::Target::jxl;o.jpeg_transcoding=true;o.keep_exif=true;
    jxrc::convert(jpeg,dir/"transcoded.jxl",o);
    auto jxl=bytes(dir/"transcoded.jxl");auto dec=JxlDecoderMake(nullptr);
    check(JxlDecoderSubscribeEvents(dec.get(),JXL_DEC_JPEG_RECONSTRUCTION|JXL_DEC_FULL_IMAGE)==JXL_DEC_SUCCESS,"Subscribe reconstruction");
    JxlDecoderSetInput(dec.get(),jxl.data(),jxl.size());JxlDecoderCloseInput(dec.get());std::vector<uint8_t> reconstructed(65536);bool set=false;
    for(;;){auto status=JxlDecoderProcessInput(dec.get());if(status==JXL_DEC_JPEG_RECONSTRUCTION){check(JxlDecoderSetJPEGBuffer(dec.get(),reconstructed.data(),reconstructed.size())==JXL_DEC_SUCCESS,"Set reconstruction buffer");set=true;}else if(status==JXL_DEC_FULL_IMAGE||status==JXL_DEC_SUCCESS)break;else check(status!=JXL_DEC_ERROR&&status!=JXL_DEC_NEED_MORE_INPUT,"JPEG reconstruction failed");}
    check(set,"Missing JPEG reconstruction");reconstructed.resize(reconstructed.size()-JxlDecoderReleaseJPEGBuffer(dec.get()));check(reconstructed==bytes(jpeg),"JPEG byte reconstruction changed");
    std::cout<<"Gain-map, PQ, linear HDR and JPEG reconstruction passed\n";
    o={};o.mode=jxrc::Mode::process;o.target=jxrc::Target::jxl;o.max_edge=20;o.exposure=1;
    r=jxrc::convert(jpeg,dir/"resized.jxl",o);check(r.width==20&&r.height==15,"Resize dimensions");
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
