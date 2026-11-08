#include "regular_decode.h"
#include <OpenEXR/ImfIO.h>
#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfInputPart.h>
#include <OpenEXR/ImfTiledInputPart.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfPartType.h>
#include <OpenEXR/ImfStandardAttributes.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void dimensions(RegularImage& image,uint64_t w,uint64_t h) {
    check(w&&h&&w<=80000000&&h<=80000000&&w*h<=80000000,"Invalid or oversized linear image dimensions");
    image.width=unsigned(w);image.height=unsigned(h);image.transfer=8;image.bits=32;
}
struct MemoryStream final:Imf::IStream {
    const uint8_t* data;size_t length,at=0;
    MemoryStream(const uint8_t* p,size_t n):Imf::IStream("memory"),data(p),length(n){}
    bool read(char* out,int n) override {check(n>=0&&size_t(n)<=length-at,"Truncated EXR");std::memcpy(out,data+at,size_t(n));at+=size_t(n);return at<length;}
    uint64_t tellg() override{return at;}
    void seekg(uint64_t p) override{check(p<=length,"Invalid EXR seek");at=size_t(p);}
    int64_t size() override{return int64_t(length);}
};
RegularImage exr(const uint8_t* data,size_t size,bool metadata) {
    MemoryStream stream(data,size);Imf::MultiPartInputFile file(stream,1);
    // Multipart files have no universal composite. Use the first ordinary image
    // part with RGB or luminance, skipping deep/utility-only parts.
    for(int part=0;part<file.parts();++part) {
        const auto& header=file.header(part);const auto& channels=header.channels();
        if(Imf::isDeepData(header.type()))continue;
        std::string prefix;
        bool rgb=channels.findChannel("R")&&channels.findChannel("G")&&channels.findChannel("B");
        const bool gray=channels.findChannel("Y")&&!channels.findChannel("RY")&&!channels.findChannel("BY");
        if(!rgb&&!gray)for(auto channel=channels.begin();channel!=channels.end();++channel) {
            const std::string name=channel.name();
            if(name.size()>2&&name.compare(name.size()-2,2,".R")==0) {
                const auto layer=name.substr(0,name.size()-1);
                if(channels.findChannel(layer+"G")&&channels.findChannel(layer+"B")){prefix=layer;rgb=true;break;}
            }
        }
        if(!rgb&&!gray)continue;
        const std::array<std::string,4> names={rgb?prefix+"R":"Y",prefix+"G",prefix+"B",prefix+"A"};
        RegularImage image;image.format="EXR";const auto window=header.dataWindow();
        const int64_t w=int64_t(window.max.x)-window.min.x+1,h=int64_t(window.max.y)-window.min.y+1;
        check(w>0&&h>0,"Invalid EXR data window");dimensions(image,uint64_t(w),uint64_t(h));
        if(Imf::hasChromaticities(header)) {const auto& c=Imf::chromaticities(header);image.chromaticities={c.red.x,c.red.y,c.green.x,c.green.y,c.blue.x,c.blue.y,c.white.x,c.white.y};}
        image.bits=16;
        for(unsigned c=0;c<4;++c) {
            if(!rgb&&c>0&&c<3)continue;
            const auto* channel=channels.findChannel(names[c]);if(!channel)continue;
            check(channel->xSampling==1&&channel->ySampling==1,"Subsampled EXR color channels are unsupported");
            if(channel->type!=Imf::HALF)image.bits=32;
        }
        if(metadata)return image;
        image.floating_pixels.resize(size_t(w)*size_t(h)*4);
        Imf::FrameBuffer frame;
        for(unsigned c=0;c<4;++c) {
            if(!rgb&&c>0&&c<3)continue;
            frame.insert(names[c],Imf::Slice::Make(Imf::FLOAT,image.floating_pixels.data()+c,window,4*sizeof(float),size_t(w)*4*sizeof(float),1,1,c==3?1.:0.));
        }
        if(Imf::isTiled(header.type())) {Imf::TiledInputPart input(file,part);input.setFrameBuffer(frame);input.readTiles(0,input.numXTiles(0)-1,0,input.numYTiles(0)-1,0);}
        else {Imf::InputPart input(file,part);input.setFrameBuffer(frame);input.readPixels(window.min.y,window.max.y);}
        for(size_t i=0;i<image.floating_pixels.size();i+=4) {
            auto* p=image.floating_pixels.data()+i;if(!rgb)p[1]=p[2]=p[0];
            // OpenEXR RGB is associated with alpha; the converter uses straight alpha.
            if(p[3]>0)for(unsigned c=0;c<3;++c)p[c]/=p[3];
            else p[0]=p[1]=p[2]=0;
        }
        return image;
    }
    throw std::runtime_error("EXR has no supported RGB/luminance image part (deep and subsampled/YCA images are unsupported)");
}
struct TextInput {
    const uint8_t* data;size_t size,at=0;
    std::string line() {
        const size_t start=at;while(at<size&&data[at]!='\n')++at;
        check(at<size&&at-start<65536,"Truncated or oversized linear image header");
        std::string text(reinterpret_cast<const char*>(data+start),at++-start);if(!text.empty()&&text.back()=='\r')text.pop_back();return text;
    }
    uint8_t byte(){check(at<size,"Truncated HDR pixels");return data[at++];}
};
RegularImage radiance(const uint8_t* data,size_t size,bool metadata) {
    TextInput in{data,size};const auto magic=in.line();check(magic=="#?RADIANCE"||magic=="#?RGBE","Invalid Radiance signature");
    RegularImage image;image.format="Radiance";image.chromaticities={.64f,.33f,.29f,.60f,.15f,.06f,.3333f,.3333f};bool xyz=false,format=false;
    for(;;) {const auto line=in.line();if(line.empty())break;
        if(line.rfind("FORMAT=",0)==0) {check(line=="FORMAT=32-bit_rle_rgbe"||line=="FORMAT=32-bit_rle_xyze","Unsupported Radiance encoding");xyz=line=="FORMAT=32-bit_rle_xyze";format=true;}
        if(line.rfind("PRIMARIES=",0)==0) {std::istringstream values(line.substr(10));for(auto& c:image.chromaticities)check(bool(values>>c)&&std::isfinite(c),"Invalid Radiance primaries");}
    }
    check(format,"Missing Radiance format");
    std::istringstream resolution(in.line());std::string axis1,axis2;uint64_t n1=0,n2=0;
    check(bool(resolution>>axis1>>n1>>axis2>>n2)&&axis1.size()==2&&axis2.size()==2&&
          (axis1[0]=='+'||axis1[0]=='-')&&(axis2[0]=='+'||axis2[0]=='-')&&
          ((axis1[1]=='Y'&&axis2[1]=='X')||(axis1[1]=='X'&&axis2[1]=='Y')),"Invalid Radiance resolution");
    dimensions(image,axis1[1]=='X'?n1:n2,axis1[1]=='Y'?n1:n2);if(metadata)return image;
    image.floating_pixels.resize(size_t(image.width)*image.height*4);std::vector<uint8_t> row(size_t(n2)*4);
    for(unsigned major=0;major<n1;++major) {
        uint8_t first[4];for(auto& v:first)v=in.byte();
        if(n2>=8&&n2<=32767&&first[0]==2&&first[1]==2&&!(first[2]&128)) {
            check((unsigned(first[2])<<8|first[3])==n2,"Invalid Radiance RLE width");
            for(unsigned c=0;c<4;++c)for(unsigned x=0;x<n2;) {
                const unsigned count=in.byte();check(count!=0,"Invalid Radiance RLE run");
                if(count>128){const unsigned run=count-128;check(run<=n2-x,"Radiance RLE overrun");const auto v=in.byte();for(unsigned j=0;j<run;++j)row[(x++)*4+c]=v;}
                else {check(count<=n2-x,"Radiance RLE overrun");for(unsigned j=0;j<count;++j)row[(x++)*4+c]=in.byte();}
            }
        } else {
            unsigned shift=0;
            for(unsigned x=0;x<n2;) {
                uint8_t code[4];if(x==0)std::memcpy(code,first,4);else for(auto& v:code)v=in.byte();
                if(code[0]==1&&code[1]==1&&code[2]==1) {
                    check(x>0&&shift<32,"Invalid legacy Radiance run");const uint64_t run=uint64_t(code[3])<<shift;
                    check(run<=n2-x,"Legacy Radiance run overrun");for(uint64_t j=0;j<run;++j){std::memcpy(row.data()+x*4,row.data()+(x-1)*4,4);++x;}shift+=8;
                } else {std::memcpy(row.data()+x*4,code,4);++x;shift=0;}
            }
        }
        for(unsigned minor=0;minor<n2;++minor) {
            auto coordinate=[](const std::string& axis,unsigned index,unsigned count){return (axis[1]=='X'?axis[0]=='-':axis[0]=='+')?count-1-index:index;};
            const unsigned a=coordinate(axis1,major,unsigned(n1)),b=coordinate(axis2,minor,unsigned(n2));
            const unsigned x=axis1[1]=='X'?a:b,y=axis1[1]=='Y'?a:b;auto* p=image.floating_pixels.data()+(size_t(y)*image.width+x)*4;
            const auto* code=row.data()+minor*4;const float scale=code[3]?std::ldexp(1.f,int(code[3])-136):0;
            for(unsigned c=0;c<3;++c)p[c]=code[c]*scale;
            p[3]=1;
            if(xyz) {const float X=p[0],Y=p[1],Z=p[2];p[0]=3.2404542f*X-1.5371385f*Y-.4985314f*Z;p[1]=-.969266f*X+1.8760108f*Y+.041556f*Z;p[2]=.0556434f*X-.2040259f*Y+1.0572252f*Z;}
        }
    }
    // XYZE was converted to linear BT.709 above.
    if(xyz)image.chromaticities={.64f,.33f,.30f,.60f,.15f,.06f,.3127f,.3290f};
    return image;
}
RegularImage pfm(const uint8_t* data,size_t size,bool metadata) {
    TextInput in{data,size};const auto magic=in.line();check(magic=="PF"||magic=="Pf","Invalid PFM signature");
    auto next=[&](){std::string line;do{line=in.line();}while(line.empty()||line[0]=='#');return line;};
    uint64_t w=0,h=0;std::istringstream dimensions_line(next());check(bool(dimensions_line>>w>>h),"Invalid PFM dimensions");
    float scale=0;std::istringstream scale_line(next());check(bool(scale_line>>scale)&&std::isfinite(scale)&&scale!=0,"Invalid PFM scale");
    RegularImage image;image.format="PFM";dimensions(image,w,h);if(metadata)return image;
    const unsigned channels=magic=="PF"?3:1;const size_t count=size_t(w)*size_t(h)*channels;
    check(count<=(size-in.at)/4,"Truncated PFM pixels");image.floating_pixels.resize(size_t(w)*size_t(h)*4);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
        auto* p=image.floating_pixels.data()+(size_t(h-1-y)*size_t(w)+x)*4;
        for(unsigned c=0;c<channels;++c) {
            uint32_t bits=0;for(unsigned j=0;j<4;++j)bits|=uint32_t(data[in.at++])<<((scale<0?j:3-j)*8);
            float value;std::memcpy(&value,&bits,4);p[c]=value*std::abs(scale);
        }
        if(channels==1)p[1]=p[2]=p[0];
        p[3]=1;
    }
    return image;
}
}
bool is_linear_image(const uint8_t* data,size_t n) {
    return (n>=4&&data[0]==0x76&&data[1]==0x2f&&data[2]==0x31&&data[3]==1)||
           (n>=11&&!std::memcmp(data,"#?RADIANCE",10)&&(data[10]=='\n'||data[10]=='\r'))||(n>=7&&!std::memcmp(data,"#?RGBE",6)&&(data[6]=='\n'||data[6]=='\r'))||
           (n>=3&&data[0]=='P'&&(data[1]=='F'||data[1]=='f')&&(data[2]=='\n'||data[2]=='\r'));
}
RegularImage decode_linear_image(const uint8_t* data,size_t size,bool metadata) {
    check(is_linear_image(data,size),"Invalid linear image signature");
    auto image=data[0]==0x76?exr(data,size,metadata):data[0]=='#'?radiance(data,size,metadata):pfm(data,size,metadata);
    if(!metadata)for(float v:image.floating_pixels)check(std::isfinite(v),"Non-finite linear HDR sample");
    return image;
}
