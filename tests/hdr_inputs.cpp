#include "jxrc/converter.hpp"
#include "regular_decode.h"
#include "hdr_encode.h"
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfTiledOutputFile.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfStandardAttributes.h>
#include <Imath/half.h>
#include <tiffio.h>
#include <fstream>
#include <cstring>
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace {
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
std::vector<uint8_t> bytes(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void write(const std::filesystem::path& path,const std::vector<uint8_t>& b){std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(b.data()),std::streamsize(b.size()));}
void append(std::vector<uint8_t>& b,const std::string& s){b.insert(b.end(),s.begin(),s.end());}
template<class F>void fails(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"Invalid HDR input was accepted");}
void near(float a,float b,float tolerance=.0001f){if(std::abs(a-b)>tolerance*std::max(1.f,std::abs(b)))throw std::runtime_error("HDR sample changed: expected "+std::to_string(b)+", got "+std::to_string(a)+", tolerance "+std::to_string(tolerance));}
void exr_fixture(const std::filesystem::path& path,bool tiled,Imf::Compression compression,bool use_half,bool gray=false,bool layered=false) {
    Imf::Header header(16,16);header.compression()=compression;
    header.dataWindow()=Imath::Box2i(Imath::V2i(-7,11),Imath::V2i(8,26));header.displayWindow()=header.dataWindow();
    Imf::addChromaticities(header,Imf::Chromaticities(Imath::V2f(.708f,.292f),Imath::V2f(.170f,.797f),Imath::V2f(.131f,.046f),Imath::V2f(.3127f,.3290f)));
    std::vector<float> pixels(16*16*4);for(size_t i=0;i<pixels.size();i+=4){pixels[i]=2;pixels[i+1]=1;pixels[i+2]=.5;pixels[i+3]=.5;}
    std::vector<Imath::half> halves(pixels.begin(),pixels.end());
    Imf::FrameBuffer frame;
    for(unsigned c=0;c<4;++c) {
        if(gray&&c>0&&c<3)continue;
        const std::string name=std::string(layered?"beauty.":"")+(c==3?"A":gray?"Y":c==0?"R":c==1?"G":"B");
        header.channels().insert(name,Imf::Channel(use_half?Imf::HALF:Imf::FLOAT));
        if(use_half)frame.insert(name,Imf::Slice::Make(Imf::HALF,halves.data()+c,header.dataWindow(),4*sizeof(Imath::half),16*4*sizeof(Imath::half)));
        else frame.insert(name,Imf::Slice::Make(Imf::FLOAT,pixels.data()+c,header.dataWindow(),4*sizeof(float),16*4*sizeof(float)));
    }
    if(tiled){header.setTileDescription(Imf::TileDescription(8,8,Imf::ONE_LEVEL));Imf::TiledOutputFile out(path.string().c_str(),header);out.setFrameBuffer(frame);out.writeTiles(0,out.numXTiles()-1,0,out.numYTiles()-1);}
    else{Imf::OutputFile out(path.string().c_str(),header);out.setFrameBuffer(frame);out.writePixels(16);}
}
void tiff_fixture(const std::filesystem::path& path,unsigned bits,bool planar,bool tiled,bool associated=false,bool log=false) {
    auto* tif=TIFFOpen(path.string().c_str(),"w");check(tif,"Create TIFF fixture");
    TIFFSetField(tif,TIFFTAG_IMAGEWIDTH,16);TIFFSetField(tif,TIFFTAG_IMAGELENGTH,16);
    TIFFSetField(tif,TIFFTAG_BITSPERSAMPLE,bits);TIFFSetField(tif,TIFFTAG_SAMPLESPERPIXEL,log?3:4);
    TIFFSetField(tif,TIFFTAG_SAMPLEFORMAT,SAMPLEFORMAT_IEEEFP);TIFFSetField(tif,TIFFTAG_PLANARCONFIG,planar?PLANARCONFIG_SEPARATE:PLANARCONFIG_CONTIG);
    TIFFSetField(tif,TIFFTAG_PHOTOMETRIC,log?PHOTOMETRIC_LOGLUV:PHOTOMETRIC_RGB);
    TIFFSetField(tif,TIFFTAG_COMPRESSION,log?COMPRESSION_SGILOG:COMPRESSION_ADOBE_DEFLATE);
    if(log){TIFFSetField(tif,TIFFTAG_SGILOGDATAFMT,SGILOGDATAFMT_FLOAT);TIFFSetField(tif,TIFFTAG_STONITS,203.);}
    else {uint16_t extra=associated?EXTRASAMPLE_ASSOCALPHA:EXTRASAMPLE_UNASSALPHA;TIFFSetField(tif,TIFFTAG_EXTRASAMPLES,1,&extra);}
    if(tiled){TIFFSetField(tif,TIFFTAG_TILEWIDTH,16);TIFFSetField(tif,TIFFTAG_TILELENGTH,16);}else TIFFSetField(tif,TIFFTAG_ROWSPERSTRIP,4);
    const unsigned channels=planar?1:log?3:4,planes=planar?4:1;
    for(unsigned plane=0;plane<planes;++plane) {
        const size_t count=16*(tiled?16:1)*channels;std::vector<uint8_t> block(count*(bits/8));
        for(size_t i=0;i<count;++i) {
            unsigned c=planar?plane:unsigned(i%channels);float v=c==3?.5f:associated?2.f:4.f;
            if(log)v=c==0?3.80188f:c==1?4.f:4.35532f; // D65 neutral XYZ.
            if(bits==16){auto value=float_to_half(v);std::memcpy(block.data()+i*2,&value,2);}
            else if(bits==32)std::memcpy(block.data()+i*4,&v,4);else{double value=v;std::memcpy(block.data()+i*8,&value,8);}
        }
        if(tiled)check(TIFFWriteTile(tif,block.data(),0,0,0,uint16_t(plane))>0,"Write TIFF tile");
        else for(unsigned y=0;y<16;++y)check(TIFFWriteScanline(tif,block.data(),y,uint16_t(plane))==1,"Write TIFF row");
    }
    TIFFClose(tif);
}
}
void hdr_input_tests(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> sources;
    // PFM endian, scale, grayscale, bottom-up orientation and signed extended F32.
    for(bool little:{false,true})for(bool gray:{false,true}) {
        std::vector<uint8_t> b;append(b,gray?"Pf\n# test\n2 2\n":"PF\n2 2\n");append(b,little?"-2\n":"2\n");
        const unsigned channels=gray?1:3;
        for(float value:{2.f,-.125f,4.f,.25f})for(unsigned c=0;c<channels;++c) {
            uint32_t bits;std::memcpy(&bits,&value,4);for(unsigned j=0;j<4;++j)b.push_back(uint8_t(bits>>((little?j:3-j)*8)));
        }
        const auto image=decode_regular(b.data(),b.size());check(image.hdr(),"PFM not HDR");near(image.floating_pixels[0],8);near(image.floating_pixels[4],.5);near(image.floating_pixels[8],4);near(image.floating_pixels[12],-.25);
        const auto path=dir/(std::string("float-")+(little?"le":"be")+(gray?"-gray":"")+".pfm");write(path,b);sources.push_back(path);
        b.resize(b.size()-3);check(inspect_regular(b.data(),b.size()).hdr,"PFM probe decoded pixels");fails([&]{decode_regular(b.data(),b.size());});
    }
    // Flat, old RLE, modern planar RLE, axis reversal and XYZE.
    for(unsigned mode=0;mode<4;++mode) {
        std::vector<uint8_t> b;append(b,"#?RADIANCE\r\nFORMAT=32-bit_rle_");append(b,mode==3?"xyze\n\n":"rgbe\n\n");
        append(b,mode==1?"+X 8 +Y 2\n":"-Y 2 +X 8\n");
        if(mode==1)for(unsigned i=0;i<8;++i)b.insert(b.end(),{128,128,128,131,64,64,64,131});
        else for(unsigned y=0;y<2;++y) {
            if(mode==2)b.insert(b.end(),{128,128,128,131,1,1,1,7});
            else if(mode==0){b.insert(b.end(),{2,2,0,8});for(unsigned c=0;c<4;++c)b.insert(b.end(),{136,uint8_t(c==3?131:128)});}
            else for(unsigned x=0;x<8;++x)b.insert(b.end(),{122,128,139,131});
        }
        const auto image=decode_regular(b.data(),b.size());check(image.width==8&&image.height==2&&image.hdr(),"Radiance dimensions/range");
        if(mode!=3){near(image.floating_pixels[0],mode==1?2:4);near(image.floating_pixels[8*4],4);}
        const auto path=dir/("radiance-"+std::to_string(mode)+".hdr");write(path,b);sources.push_back(path);
        b.resize(b.size()-1);check(inspect_regular(b.data(),b.size()).hdr,"Radiance probe decoded pixels");fails([&]{decode_regular(b.data(),b.size());});
    }
    for(bool tiled:{false,true})for(bool gray:{false,true})for(bool use_half:{false,true}) {
        const auto path=dir/("exr-"+std::to_string(tiled)+std::to_string(gray)+std::to_string(use_half)+".exr");exr_fixture(path,tiled,Imf::ZIP_COMPRESSION,use_half,gray);sources.push_back(path);
        auto b=bytes(path);const auto image=decode_regular(b.data(),b.size());check(image.width==16&&image.height==16&&image.hdr(),"EXR inspection");
        near(image.floating_pixels[0],4);near(image.floating_pixels[1],gray?4:2);near(image.floating_pixels[2],gray?4:1);near(image.floating_pixels[3],.5);
        std::vector<float> linear;uhdr_color_gamut_t gamut;regular_to_linear(image,linear,gamut);near(linear[0],4,.001f);near(linear[1],gray?4:2,.001f);check(gamut==UHDR_CG_BT_2100,"EXR chromaticities ignored");
    }
    const auto layered=dir/"layered.exr";exr_fixture(layered,false,Imf::PIZ_COMPRESSION,true,false,true);sources.push_back(layered);
    auto layeredBytes=bytes(layered);auto layeredImage=decode_regular(layeredBytes.data(),layeredBytes.size());near(layeredImage.floating_pixels[0],4);near(layeredImage.floating_pixels[3],.5);
    for(unsigned bits:{16,32,64})for(bool planar:{false,true})for(bool tiled:{false,true}) {
        const auto path=dir/("tiff-"+std::to_string(bits)+std::to_string(planar)+std::to_string(tiled)+".tif");tiff_fixture(path,bits,planar,tiled);sources.push_back(path);
        const auto info=jxrc::inspect(path);check(info.hdr&&info.transfer=="Linear"&&info.bits==bits,"Float TIFF metadata");auto b=bytes(path);auto image=decode_regular(b.data(),b.size());near(image.floating_pixels[0],4);near(image.floating_pixels[3],.5);
    }
    const auto associated=dir/"associated.tif";tiff_fixture(associated,32,false,false,true);sources.push_back(associated);
    auto b=bytes(associated);auto image=decode_regular(b.data(),b.size());near(image.floating_pixels[0],4);near(image.floating_pixels[3],.5);
    const auto log=dir/"logluv.tif";tiff_fixture(log,32,false,false,false,true);sources.push_back(log);
    b=bytes(log);image=decode_regular(b.data(),b.size());// LogLuv quantizes chromaticity as well as logarithmic luminance.
    for(unsigned c=0;c<3;++c)near(image.floating_pixels[c],4,.025f);
    // All new input types reach the normal shared conversion/preview paths.
    for(const auto& source:sources) {
        const auto original=bytes(source);const auto originalDate=std::filesystem::last_write_time(source);
        const auto info=jxrc::inspect(source);check(info.hdr&&!info.raw&&info.transfer=="Linear","Linear HDR input classification");
        const auto preview=jxrc::preview(source,4);check(preview.width&&preview.height&&preview.width<=4&&preview.height<=4,"Linear HDR thumbnail");
        auto data=bytes(source);auto raster=decode_regular(data.data(),data.size());std::vector<float> reference;uhdr_color_gamut_t cg;regular_to_linear(raster,reference,cg);
        jxrc::Options options;options.mode=jxrc::Mode::process;options.target=jxrc::Target::jxl_linear;options.quality.preset=jxrc::Preset::lossless;
        const auto output=dir/(source.filename().string()+".jxl");jxrc::convert(source,output,options);
        data=bytes(output);std::vector<float> decoded;unsigned w=0,h=0;bool hdr=false;int transfer=0;
        check(decode_jxl_to_linear_f32(data.data(),data.size(),decoded,w,h,&cg,&hdr,&transfer,false,nullptr)==0,"Linear HDR output decode");
        check(decoded.size()==reference.size(),"Linear HDR output shape");for(size_t i=0;i<decoded.size();++i)near(decoded[i],reference[i],.002f);
        check(bytes(source)==original&&std::filesystem::last_write_time(source)==originalDate,"Conversion or preview modified the HDR source in place");
    }
    // Linear cICP PNG is identified by its transfer, even below SDR white.
    const auto linearPng=dir/"linear.png";
    const uint16_t codes[]={32768,16384,8192,65535};StillSourceColor color;color.bits=16;color.primaries=9;color.transfer=8;
    check(encode_png_source(1,1,codes,true,color,linearPng.string().c_str(),StillEncodeQuality(0))==0,"Linear PNG fixture");
    const auto pngInfo=jxrc::inspect(linearPng);check(pngInfo.hdr&&pngInfo.transfer=="Linear"&&pngInfo.primaries=="BT.2020","Linear PNG classification");
    auto pngData=bytes(linearPng);auto pngRaster=decode_regular(pngData.data(),pngData.size());std::vector<float> pngLinear;uhdr_color_gamut_t pngGamut;
    regular_to_linear(pngRaster,pngLinear,pngGamut);near(pngLinear[0],32768.f/65535);near(pngLinear[1],16384.f/65535);
    // One prepared render can serve all existing SDR/HDR output families.
    const auto source=dir/"all-targets.pfm";std::vector<uint8_t> allPixels;append(allPixels,"PF\n16 16\n-1\n");
    for(unsigned i=0;i<16*16;++i)for(float value:{4.f,2.f,1.f}) {uint32_t bits;std::memcpy(&bits,&value,4);for(unsigned c=0;c<4;++c)allPixels.push_back(uint8_t(bits>>(c*8)));}
    write(source,allPixels);const auto sourceDate=std::filesystem::last_write_time(source);jxrc::ConversionSession session(source);
    for(auto target:{jxrc::Target::jpeg,jxrc::Target::jpeg_gainmap,jxrc::Target::png,jxrc::Target::png_hdr,jxrc::Target::png_hlg,jxrc::Target::avif_pq,jxrc::Target::avif_hlg,jxrc::Target::avif_gainmap,jxrc::Target::jxl_pq,jxrc::Target::jxl_hlg,jxrc::Target::jxl_scrgb}) {
        jxrc::Options options;options.mode=jxrc::Mode::process;options.target=target;options.quality.preset=jxrc::Preset::lossless;
        const auto output=dir/("all-targets-"+std::to_string(int(target)));session.convert(output,options);check(jxrc::inspect(output).width==16,"HDR input output-family conversion");
    }
    jxrc::Options inplace;inplace.mode=jxrc::Mode::process;inplace.overwrite=true;
    fails([&]{session.convert(source,inplace);});
    const auto alias=dir/"source-alias.pfm";std::error_code linkError;std::filesystem::create_hard_link(source,alias,linkError);
    if(!linkError)fails([&]{session.convert(alias,inplace);});
    check(bytes(source)==allPixels&&std::filesystem::last_write_time(source)==sourceDate,"Multi-target conversion modified its source in place");
    std::cout<<"Linear HDR inputs and source preservation passed\n";
}
