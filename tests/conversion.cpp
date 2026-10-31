#include "jxrc/converter.hpp"
#include "codec_api.h"
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
int main(int argc,char** argv) try {
    if(argc==3&&std::string(argv[1])=="--fixture") { encode(fs::u8path(argv[2]),0,false);return 0; }
    const auto dir=fs::temp_directory_path()/("jxrc-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directory(dir);
    struct Clean {fs::path p;~Clean(){std::error_code e;fs::remove_all(p,e);}} clean{dir};
    jxrc::validate({});
    for(int mode=0;mode<8;++mode) {
        const auto source=dir/("fixture"+std::to_string(mode));encode(source,mode,mode!=0&&mode!=2&&mode!=4);
        for(auto target:{jxrc::Target::jpeg,jxrc::Target::jpeg_gainmap,jxrc::Target::jxl,jxrc::Target::jxl_linear,jxrc::Target::jxl_pq,jxrc::Target::avif,jxrc::Target::avif_pq,jxrc::Target::avif_gainmap}) {
            jxrc::Options o;o.mode=jxrc::Mode::process;o.target=target;o.quality.preset=jxrc::Preset::lossless;
            const auto output=dir/("process-"+std::to_string(mode)+"-"+std::to_string(int(target)));
            jxrc::Result r;try{r=jxrc::convert(source,output,o);}catch(const std::exception& e){throw std::runtime_error("Input mode "+std::to_string(mode)+" target "+std::to_string(int(target))+": "+e.what());}check(r.width==64&&r.height==48,"Processed dimensions");check(fs::file_size(output)>10,"Encoded output");
        }
    }
    std::cout<<"64 process combinations passed\n";
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
    fails([&]{jxrc::convert(jpeg,dir/"resized.jxl",o);});o.overwrite=true;jxrc::convert(jpeg,dir/"resized.jxl",o);
    fails([&]{jxrc::convert(jpeg,jpeg,o);});
    o.mode=jxrc::Mode::preserve;fails([&]{jxrc::validate(o);});o={};o.raw.shoulder=NAN;fails([&]{jxrc::validate(o);});
    write(dir/"bad",{1,2,3,4});o={};fails([&]{jxrc::convert(dir/"bad",dir/"bad-output",o);});check(!fs::exists(dir/"bad-output"),"Bad input published");
    dng(dir/"sensor.dng");o={};o.mode=jxrc::Mode::process;o.target=jxrc::Target::jxl_linear;
    auto raw=jxrc::convert(dir/"sensor.dng",dir/"raw.jxl",o);check(raw.width>0&&raw.height>0,"RAW decode");
    o.raw.hdr_mode=jxrc::RawHdrMode::custom;o.raw.exposure=1;o.raw.white_balance=8;o.raw.kelvin=6500;o.raw.shadows=0.2f;o.raw.highlights=-0.1f;o.raw.highlight_stops=1;
    jxrc::convert(dir/"sensor.dng",dir/"raw-adjusted.jxl",o);check(bytes(dir/"raw.jxl")!=bytes(dir/"raw-adjusted.jxl"),"RAW controls ineffective");
    o.target=jxrc::Target::avif_gainmap;jxrc::convert(dir/"sensor.dng",dir/"raw-gain.avif",o);
    std::cout<<"RAW demosaic, controls, paired SDR base and I/O validation passed\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
