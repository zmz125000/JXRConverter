#include "hdr_encode.h"
#include "exif_metadata.h"
#include "hdr_parallel.h"
#include "ultrahdr/icc.h"
#include "output_file.h"
#include "skcms.h"
#include <webp/encode.h>
#include <webp/mux.h>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace {
void check(bool ok,const char* message) {
    if(!ok)throw std::runtime_error(message);
}

size_t pixel_count(unsigned w,unsigned h) {
    check(w && h && w<=WEBP_MAX_DIMENSION && h<=WEBP_MAX_DIMENSION,
          "WebP dimensions must be between 1 and 16383 pixels per side");
    return size_t(w)*h;
}

void write_webp(unsigned w,unsigned h,const uint8_t* rgba,WebPData icc,
                const char* path,const StillEncodeQuality& quality) {
    WebPConfig config;
    check(WebPConfigInit(&config),"Cannot initialize WebP encoder");
    config.lossless=quality.preset==0;
    config.quality=float(quality.webp());
    config.exact=1; // Preserve hidden RGB in lossless images with transparency.
    config.use_sharp_yuv=!config.lossless;
    config.thread_level=hdr_pixel_jobs(size_t(w)*h)>1;

    struct Picture : WebPPicture {
        Picture() { check(WebPPictureInit(this),"Cannot initialize WebP picture"); }
        ~Picture() { WebPPictureFree(this); }
    } picture;
    struct Encoded : WebPMemoryWriter {
        Encoded() { WebPMemoryWriterInit(this); }
        ~Encoded() { WebPMemoryWriterClear(this); }
    } encoded;
    picture.width=int(w);picture.height=int(h);
    picture.use_argb=1;
    picture.writer=WebPMemoryWrite;
    picture.custom_ptr=&encoded;
    check(WebPPictureImportRGBA(&picture,rgba,int(w)*4),"Cannot import WebP pixels");
    if(!WebPEncode(&config,&picture))
        throw std::runtime_error("WebP encode failed (codec "+std::to_string(picture.error_code)+")");

    const WebPData bitstream{encoded.mem,encoded.size};
    const std::unique_ptr<WebPMux,decltype(&WebPMuxDelete)> mux(WebPMuxCreate(&bitstream,0),WebPMuxDelete);
    check(bool(mux),"Cannot initialize WebP container");
    check(WebPMuxSetChunk(mux.get(),"ICCP",&icc,0)==WEBP_MUX_OK,"Cannot store WebP ICC profile");
    if(quality.exif_size) {
        const WebPData exif{quality.exif,quality.exif_size};
        check(WebPMuxSetChunk(mux.get(),"EXIF",&exif,0)==WEBP_MUX_OK,"Cannot store WebP EXIF");
    }
    struct Container : WebPData {
        Container() { WebPDataInit(this); }
        ~Container() { WebPDataClear(this); }
    } container;
    check(WebPMuxAssemble(mux.get(),&container)==WEBP_MUX_OK,"Cannot assemble WebP container");

    FILE* file=open_output_file(path);
    check(file!=nullptr,"Cannot open WebP output");
    const bool wrote=std::fwrite(container.bytes,1,container.size,file)==container.size;
    const bool closed=std::fclose(file)==0;
    check(wrote && closed,"Cannot write WebP output");
}

void write_webp_srgb(unsigned w,unsigned h,const uint8_t* rgba,uhdr_color_gamut_t gamut,
                     const char* path,const StillEncodeQuality& quality) {
    auto profile=ultrahdr::IccHelper::writeIccProfile(UHDR_CT_SRGB,gamut);
    const size_t prefix=ultrahdr::kICCIdentifierSize;
    check(profile && profile->getLength()>prefix,"Cannot build WebP ICC profile");
    const WebPData icc{static_cast<const uint8_t*>(profile->getData())+prefix,profile->getLength()-prefix};
    write_webp(w,h,rgba,icc,path,quality);
}
}

// Export encoded SDR samples directly. Zero means the source needs a color
// transform or HDR rendering before it can be written.
int encode_webp_source(unsigned w,unsigned h,const void* pixels,bool sixteen,
                       const StillSourceColor& color,const char* path,StillEncodeQuality quality) {
    if(!pixels || color.cmyk || color.exponent_bits || color.transfer==8 || color.transfer==16 || color.transfer==18)return 0;
    if(!color.icc_size && (color.transfer!=13 || (color.primaries!=1 && color.primaries!=9 && color.primaries!=12)))return 0;
    check(color.channels>=1 && color.channels<=4 && color.orientation>=1 && color.orientation<=8,
          "Invalid WebP source layout");

    const unsigned dw=color.orientation>=5?h:w,dh=color.orientation>=5?w:h;
    const size_t count=pixel_count(dw,dh);
    const bool gray_profile=color.icc_size>=20 && !std::memcmp(color.icc+16,"GRAY",4);
    const bool gray=color.gray||gray_profile||color.channels<=2;
    const bool alpha=color.channels==2||color.channels==4;
    std::vector<uint8_t> packed;
    const auto* samples=static_cast<const uint8_t*>(pixels);
    if(sixteen || color.channels!=4 || color.orientation!=1 || gray) {
        packed.resize(count*4);
        const auto sample=[&](size_t i) -> uint8_t {
            if(!sixteen)return samples[i];
            uint16_t v;std::memcpy(&v,samples+i*2,2);
            return uint8_t((v+128u)/257u);
        };
        hdr_parallel_pixels(dh,hdr_pixel_jobs(count),[&](size_t begin,size_t end,unsigned) {
            for(unsigned y=unsigned(begin);y<end;++y)for(unsigned x=0;x<dw;++x) {
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
                const size_t from=(size_t(sy)*w+sx)*color.channels;
                auto* to=packed.data()+(size_t(y)*dw+x)*4;
                for(unsigned c=0;c<3;++c)to[c]=sample(from+(gray?0:c));
                to[3]=alpha?sample(from+color.channels-1):255;
            }
        });
    }
    std::vector<uint8_t> exif;
    if(quality.exif_size)exif.assign(quality.exif,quality.exif+quality.exif_size);
    prepare_export_exif(exif,dw,dh);
    quality.exif=exif.data();quality.exif_size=exif.size();

    const auto* rgba=packed.empty()?samples:packed.data();
    if(gray_profile) {
        // WebP always exposes RGB pixels. Convert the gray profile to an RGB
        // profile so color-managed readers can apply it to the stored pixels.
        skcms_ICCProfile source;
        check(skcms_Parse(color.icc,color.icc_size,&source) &&
              skcms_Transform(rgba,skcms_PixelFormat_RGBA_8888,skcms_AlphaFormat_Unpremul,&source,
                              packed.data(),skcms_PixelFormat_RGBA_8888,skcms_AlphaFormat_Unpremul,
                              skcms_sRGB_profile(),count),"Cannot convert grayscale ICC for WebP");
        write_webp_srgb(dw,dh,packed.data(),UHDR_CG_BT_709,path,quality);
    } else if(color.icc_size) {
        write_webp(dw,dh,rgba,{color.icc,color.icc_size},path,quality);
    } else {
        const auto gamut=color.primaries==9?UHDR_CG_BT_2100:color.primaries==12?UHDR_CG_DISPLAY_P3:UHDR_CG_BT_709;
        write_webp_srgb(dw,dh,rgba,gamut,path,quality);
    }
    return 1;
}

int encode_webp_f32(unsigned w,unsigned h,const float* rgba,uhdr_color_gamut_t gamut,
                    const char* path,StillEncodeQuality quality) {
    const size_t count=pixel_count(w,h);
    std::vector<uint8_t> packed(count*4);
    hdr_parallel_pixels(count,hdr_pixel_jobs(count),[&](size_t begin,size_t end,unsigned) {
        for(size_t i=begin;i<end;++i) {
            for(unsigned c=0;c<3;++c)packed[4*i+c]=linear_to_srgb_u8(rgba[4*i+c]);
            const float alpha=rgba[4*i+3];
            packed[4*i+3]=std::isfinite(alpha)?uint8_t(std::clamp(alpha,0.f,1.f)*255.f+.5f):0;
        }
    });
    write_webp_srgb(w,h,packed.data(),gamut,path,quality);
    return 0;
}
