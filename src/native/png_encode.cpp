#include "codec_api.h"
#include "exif_metadata.h"
#include "ultrahdr/icc.h"
#include <png.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
struct PngWriter {
    FILE* file = nullptr;
    png_structp png = nullptr;
    png_infop info = nullptr;
    ~PngWriter() {
        if(png)png_destroy_write_struct(&png,&info);
        if(file)std::fclose(file);
    }
};

// Owners exist before setjmp; libpng errors cannot skip their destructors.
// Metadata precedes IDAT and 16-bit rows use PNG byte order.
template<class Row>
int write_png(unsigned w,unsigned h,unsigned depth,unsigned channels,bool gray,
              const StillSourceColor& color,const char* path,StillEncodeQuality quality,Row fill,bool matching_cicp=false) {
    if(!w||!h||uint64_t(w)*h>80000000||(depth!=8&&depth!=16)||!path)return -100;
    std::vector<uint8_t> row(size_t(w)*channels*(depth/8));
    PngWriter writer;
    writer.file=std::fopen(path,"wb");if(!writer.file)return -101;
    writer.png=png_create_write_struct(PNG_LIBPNG_VER_STRING,nullptr,nullptr,nullptr);
    if(!writer.png)return -102;
    writer.info=png_create_info_struct(writer.png);if(!writer.info)return -102;
    auto* png=writer.png;auto* info=writer.info;
    if(setjmp(png_jmpbuf(png)))return -103;
    png_init_io(png,writer.file);
    const bool alpha=channels==2||channels==4;
    const int type=gray?(alpha?PNG_COLOR_TYPE_GRAY_ALPHA:PNG_COLOR_TYPE_GRAY):(alpha?PNG_COLOR_TYPE_RGBA:PNG_COLOR_TYPE_RGB);
    png_set_IHDR(png,info,w,h,depth,type,PNG_INTERLACE_NONE,PNG_COMPRESSION_TYPE_DEFAULT,PNG_FILTER_TYPE_DEFAULT);
    const bool hdr=color.transfer==16||color.transfer==18;
    if(!hdr&&color.icc_size) {
        const char* name=matching_cicp?(color.primaries==9?"BT.2020":color.primaries==12?"Display P3":"sRGB"):"Source";
        png_set_iCCP(png,info,name,PNG_COMPRESSION_TYPE_BASE,color.icc,static_cast<png_uint_32>(color.icc_size));
    }
    if(hdr||!color.icc_size||matching_cicp) {
        const bool supported=(color.primaries==1||color.primaries==9||color.primaries==12)&&(hdr||color.transfer==13);
        if(!supported)return -105;
        png_set_cICP(png,info,static_cast<png_byte>(color.primaries),static_cast<png_byte>(color.transfer),0,1);
        if(color.primaries==1&&color.transfer==13&&!color.icc_size)png_set_sRGB(png,info,PNG_sRGB_INTENT_RELATIVE);
    }
    if(quality.exif_size)png_set_eXIf_1(png,info,static_cast<png_uint_32>(quality.exif_size),const_cast<png_bytep>(quality.exif));
    png_write_info(png,info);
    for(unsigned y=0;y<h;++y){fill(y,row.data());png_write_row(png,row.data());}
    png_write_end(png,info);
    const bool written=std::ferror(writer.file)==0;
    const bool closed=std::fclose(writer.file)==0;writer.file=nullptr;
    return written&&closed?0:-104;
}
void put_sample(uint8_t* row,size_t i,unsigned sample,unsigned depth) {
    if(depth==8)row[i]=uint8_t(sample);
    else {row[i*2]=uint8_t(sample>>8);row[i*2+1]=uint8_t(sample);}
}
}

// Source codes are already transfer encoded and straight alpha. Rotate integer
// samples directly so readers need not implement EXIF display orientation.
int encode_png_source(unsigned w,unsigned h,const void* pixels,bool sixteen,
                      const StillSourceColor& color,const char* path,StillEncodeQuality quality) {
    if(!pixels||color.exponent_bits||color.channels<1||color.channels>4||color.orientation<1||color.orientation>8)return -100;
    const unsigned dw=color.orientation>=5?h:w,dh=color.orientation>=5?w:h;
    const bool gray=color.channels<=2||(color.icc_size>=20&&!std::memcmp(color.icc+16,"GRAY",4));
    const bool alpha=color.channels==2||color.channels==4;
    const unsigned channels=(gray?1:3)+(alpha?1:0),depth=quality.png_depth(color.bits);
    std::vector<uint8_t> exif;
    if(quality.exif_size)exif.assign(quality.exif,quality.exif+quality.exif_size);
    prepare_export_exif(exif,dw,dh);quality.exif=exif.data();quality.exif_size=exif.size();
    return write_png(dw,dh,depth,channels,gray,color,path,quality,[&](unsigned y,uint8_t* row){
        for(unsigned x=0;x<dw;++x) {
            unsigned sx=x,sy=y;
            switch(color.orientation) {
                case 2:sx=w-1-x;break;
                case 3:sx=w-1-x;sy=h-1-y;break;
                case 4:sy=h-1-y;break;
                case 5:sx=y;sy=x;break;
                case 6:sx=y;sy=h-1-x;break;
                case 7:sx=w-1-y;sy=h-1-x;break;
                case 8:sx=w-1-y;sy=x;break;
            }
            for(unsigned c=0;c<channels;++c) {
                const unsigned sc=alpha&&c==channels-1?color.channels-1:gray?0:c;
                const size_t at=(size_t(sy)*w+sx)*color.channels+sc;
                unsigned v;
                if(sixteen){uint16_t sample;std::memcpy(&sample,static_cast<const uint8_t*>(pixels)+at*2,2);v=sample;}
                else v=static_cast<const uint8_t*>(pixels)[at]*257u;
                if(depth==8)v=(v*255u+32767u)/65535u;
                put_sample(row,size_t(x)*channels+c,v,depth);
            }
        }
    });
}

// F32 stays in display-linear light until the final transfer and quantization.
int encode_png_f32(unsigned w,unsigned h,const float* rgba,uhdr_color_gamut_t gamut,
                   int transfer,int depth,const char* path,StillEncodeQuality quality) {
    if(!rgba||(transfer!=13&&transfer!=16&&transfer!=18))return -100;
    StillSourceColor color;color.primaries=gamut==UHDR_CG_BT_2100?9:gamut==UHDR_CG_DISPLAY_P3?12:1;
    color.transfer=transfer;
    // Match AVIF's SDR profile so readers without PNG cICP support still see
    // the actual output primaries and sRGB curve. HDR keeps its explicit CICP.
    auto profile=transfer==13?ultrahdr::IccHelper::writeIccProfile(UHDR_CT_SRGB,gamut):nullptr;
    if(transfer==13) {
        if(!profile||profile->getLength()<=ultrahdr::kICCIdentifierSize)return -106;
        color.icc=static_cast<const uint8_t*>(profile->getData())+ultrahdr::kICCIdentifierSize;
        color.icc_size=profile->getLength()-ultrahdr::kICCIdentifierSize;
    }
    return write_png(w,h,depth,4,false,color,path,quality,[&](unsigned y,uint8_t* row){
        for(unsigned x=0;x<w;++x) {
            const auto* p=rgba+(size_t(y)*w+x)*4;
            float rgb[3]={p[0],p[1],p[2]};
            if(transfer==18)linear_to_hlg_rgb(rgb[0],rgb[1],rgb[2],gamut);
            for(unsigned c=0;c<4;++c) {
                float v=c==3?p[3]:rgb[c];if(!std::isfinite(v))v=0.f;
                if(c==3||transfer==18)v=std::clamp(v,0.f,1.f);
                else v=transfer==16?linear_to_pq_code(v):srgb_oetf(v);
                put_sample(row,size_t(x)*4+c,unsigned(std::lround(v*(depth==8?255.f:65535.f))),depth);
            }
        }
    },true);
}
