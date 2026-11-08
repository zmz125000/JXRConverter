#include "regular_decode.h"
#include "linear_color_profile.h"
#include "hdr_encode.h"
#include <png.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <tiffio.h>
#include <libraw/libraw.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 80000000
#include <stb_image.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {
constexpr size_t metadata_limit = 16 * 1024 * 1024;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void dimensions(unsigned w, unsigned h) {
    check(w && h && uint64_t(w)*h <= 80000000, "Invalid or oversized raster dimensions");
}
bool signature(const uint8_t* data, size_t size, const char* text, size_t count) {
    return size >= count && !std::memcmp(data,text,count);
}
bool tiff_signature(const uint8_t* d, size_t n) {
    return n >= 4 && ((d[0]=='I' && d[1]=='I' && (d[2]==42 || d[2]==43) && !d[3]) ||
                     (d[0]=='M' && d[1]=='M' && !d[2] && (d[3]==42 || d[3]==43)));
}
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
void copy_metadata(std::vector<uint8_t>& out, const uint8_t* p, size_t n) {
    check(n<=metadata_limit,"Raster metadata exceeds 16 MiB");
    if(n)out.assign(p,p+n);
}
void strip_exif_prefix(std::vector<uint8_t>& exif) {
    if(exif.size()>=6 && !std::memcmp(exif.data(),"Exif\0\0",6))exif.erase(exif.begin(),exif.begin()+6);
}
// Serialize a matrix/TRC profile for PNG gAMA/cHRM without changing pixel codes.
std::vector<uint8_t> png_profile(double gamma, const skcms_Matrix3x3& matrix) {
    std::vector<uint8_t> icc(132+7*12,0);
    auto u32=[&](size_t p,uint32_t v){for(int i=3;i>=0;--i){icc[p+i]=uint8_t(v);v>>=8;}};
    auto fixed=[&](size_t p,double v){u32(p,static_cast<uint32_t>(static_cast<int32_t>(std::lround(v*65536))));};
    u32(8,0x02100000);std::memcpy(icc.data()+12,"mntrRGB XYZ ",12);std::memcpy(icc.data()+36,"acsp",4);
    fixed(68,0.9642);fixed(72,1);fixed(76,0.8249);u32(128,7);
    for(unsigned t=0;t<7;++t) {
        const char* names[]={"rXYZ","gXYZ","bXYZ","wtpt","rTRC","gTRC","bTRC"};
        const size_t at=icc.size(),entry=132+t*12,len=t<4?20:40;
        icc.resize(at+len,0);std::memcpy(icc.data()+entry,names[t],4);u32(entry+4,static_cast<uint32_t>(at));u32(entry+8,static_cast<uint32_t>(len));
        if(t<4) {
            std::memcpy(icc.data()+at,"XYZ ",4);
            for(unsigned c=0;c<3;++c)fixed(at+8+c*4,t==3?(c==0?0.9642:c==1?1:0.8249):matrix.vals[c][t]);
        } else {
            std::memcpy(icc.data()+at,"para",4);icc[at+9]=4;
            const auto* srgb=skcms_sRGB_TransferFunction();
            const double values[]={gamma>0?1/gamma:srgb->g,gamma>0?1:srgb->a,gamma>0?0:srgb->b,
                gamma>0?1:srgb->c,gamma>0?0:srgb->d,0,0};
            for(unsigned c=0;c<7;++c)fixed(at+12+c*4,values[c]);
        }
    }
    u32(0,static_cast<uint32_t>(icc.size()));return icc;
}
struct PngInput { const uint8_t* data; size_t size, at=0; };
void png_read(png_structp png, png_bytep out, png_size_t count) {
    auto& in=*static_cast<PngInput*>(png_get_io_ptr(png));
    if(count>in.size-in.at)png_error(png,"Truncated PNG");
    std::memcpy(out,in.data+in.at,count);in.at+=count;
}
// Warnings about ancillary metadata (for example conflicting ICC/sRGB
// combinations) are recoverable. Keep libpng's fatal errors and strict CRC
// validation, but do not turn a usable PNG into a failed thumbnail.
void png_warning(png_structp, png_const_charp text) { std::fprintf(stderr,"PNG warning: %s\n",text); }
RegularImage decode_png(const uint8_t* data,size_t size,bool metadata_only=false) {
    RegularImage image;image.format="PNG";
    bool saw_cicp=false,saw_idat=false;
    for(size_t at=8;at+12<=size;) {
        const auto* p=data+at;
        const uint32_t n=uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3];
        check(n<=size-at-12,"Truncated PNG chunk");
        check(std::memcmp(p+4,"acTL",4)!=0,"Animated PNG is not supported for still conversion");
        if(metadata_only&&!std::memcmp(p+4,"eXIf",4))copy_metadata(image.exif,p+8,n);
        if(!std::memcmp(p+4,"IDAT",4))saw_idat=true;
        if(!std::memcmp(p+4,"cICP",4)) {
            // libpng may warn and discard invalid cICP. Never reinterpret an
            // advertised HDR encoding as ordinary SDR when recovering warnings.
            check(!saw_cicp&&!saw_idat&&n==4,"Invalid PNG CICP chunk");
            const auto cp=p[8],tc=p[9];
            check((cp==1||cp==9||cp==12)&&(tc==16||tc==18||tc==13||tc==8)&&p[10]==0&&p[11]==1,
                  "Unsupported PNG CICP encoding (requires supported primaries and full-range RGB)");
            saw_cicp=true;
        }
        at+=size_t(n)+12;
    }
    PngInput input{data,size};std::vector<png_bytep> rows;
    png_structp png=png_create_read_struct(PNG_LIBPNG_VER_STRING,nullptr,nullptr,png_warning);
    check(png!=nullptr,"Cannot allocate PNG decoder");
    png_infop info=png_create_info_struct(png),end=png_create_info_struct(png);
    struct Cleanup {png_structp& p;png_infop& i;png_infop& e;~Cleanup(){png_destroy_read_struct(&p,&i,&e);}} cleanup{png,info,end};
    check(info && end,"Cannot allocate PNG metadata");
    if(setjmp(png_jmpbuf(png)))throw std::runtime_error("Invalid or unsupported PNG");
    png_set_read_fn(png,&input,png_read);png_set_crc_action(png,PNG_CRC_ERROR_QUIT,PNG_CRC_ERROR_QUIT);
    png_set_user_limits(png,80000000,80000000);png_set_chunk_malloc_max(png,metadata_limit);
    png_read_info(png,info);
    image.width=png_get_image_width(png,info);image.height=png_get_image_height(png,info);dimensions(image.width,image.height);
    const int color=png_get_color_type(png,info),depth=png_get_bit_depth(png,info);
    image.gray=color==PNG_COLOR_TYPE_GRAY||color==PNG_COLOR_TYPE_GRAY_ALPHA;
    image.bits=depth==16?16:8;
    bool cicp=false;
    // cICP takes precedence over ICC/sRGB/gAMA/cHRM. Keep source codes intact.
#ifdef PNG_cICP_SUPPORTED
    png_byte cp=0,tc=0,mc=0,full=0;
    if(png_get_cICP(png,info,&cp,&tc,&mc,&full)) {
        check((cp==1 || cp==9 || cp==12) && (tc==16 || tc==18 || tc==13 || tc==8) && mc==0 && full==1,
              "Unsupported PNG CICP encoding (requires supported primaries and full-range RGB)");
        image.primaries=cp;image.transfer=tc;cicp=true;
    }
#endif
    png_charp name=nullptr;int compression=0;png_bytep profile=nullptr;png_uint_32 profile_size=0;
    if(!cicp && png_get_iCCP(png,info,&name,&compression,&profile,&profile_size))copy_metadata(image.icc,profile,profile_size);
    else if(!cicp) {
        int intent=0;double gamma=0,wx=0.3127,wy=0.3290,rx=0.64,ry=0.33,gx=0.30,gy=0.60,bx=0.15,by=0.06;
        if(!png_get_sRGB(png,info,&intent)) {
            const bool has_gamma=png_get_gAMA(png,info,&gamma)!=0;
            const bool has_chrm=png_get_cHRM(png,info,&wx,&wy,&rx,&ry,&gx,&gy,&bx,&by)!=0;
            if(has_gamma || has_chrm) {
                check(!has_gamma || (gamma>0 && std::isfinite(gamma)),"Invalid PNG gamma");
                skcms_Matrix3x3 matrix{};
                check(skcms_PrimariesToXYZD50(float(rx),float(ry),float(gx),float(gy),float(bx),float(by),float(wx),float(wy),&matrix),"Invalid PNG chromaticities");
                image.icc=png_profile(has_gamma?gamma:0,matrix);
            }
        }
    }
    if(metadata_only){image.orientation=regular_orientation(image.exif);return image;}
    const bool transparency=png_get_valid(png,info,PNG_INFO_tRNS)!=0;
    if(color==PNG_COLOR_TYPE_PALETTE)png_set_palette_to_rgb(png);
    if(color==PNG_COLOR_TYPE_GRAY && depth<8)png_set_expand_gray_1_2_4_to_8(png);
    if(transparency)png_set_tRNS_to_alpha(png);
    if(color==PNG_COLOR_TYPE_GRAY || color==PNG_COLOR_TYPE_GRAY_ALPHA)png_set_gray_to_rgb(png);
    if(!(color & PNG_COLOR_MASK_ALPHA) && !transparency)png_set_add_alpha(png,depth==16?65535:255,PNG_FILLER_AFTER);
    const uint16_t endian=1;if(depth==16 && *reinterpret_cast<const uint8_t*>(&endian))png_set_swap(png);
    png_set_interlace_handling(png);png_read_update_info(png,info);
    const size_t stride=size_t(image.width)*4*(image.bits/8);
    check(png_get_rowbytes(png,info)==stride,"Unsupported PNG row layout");
    image.pixels.resize(stride*image.height);rows.resize(image.height);
    for(unsigned y=0;y<image.height;++y)rows[y]=image.pixels.data()+stride*y;
    png_read_image(png,rows.data());png_read_end(png,end);
    png_uint_32 exif_size=0;png_bytep exif=nullptr;
    if(png_get_eXIf_1(png,info,&exif_size,&exif) || png_get_eXIf_1(png,end,&exif_size,&exif))copy_metadata(image.exif,exif,exif_size);
    image.orientation=regular_orientation(image.exif);return image;
}
RegularImage decode_webp(const uint8_t* data,size_t size) {
    RegularImage image;image.format="WebP";
    WebPData input{data,size};
    std::unique_ptr<WebPDemuxer,decltype(&WebPDemuxDelete)> demux(WebPDemux(&input),WebPDemuxDelete);
    check(bool(demux),"Invalid WebP");image.width=WebPDemuxGetI(demux.get(),WEBP_FF_CANVAS_WIDTH);image.height=WebPDemuxGetI(demux.get(),WEBP_FF_CANVAS_HEIGHT);dimensions(image.width,image.height);
    // A still converter must not silently discard animation frames.
    check(!(WebPDemuxGetI(demux.get(),WEBP_FF_FORMAT_FLAGS)&ANIMATION_FLAG),"Animated WebP is not supported for still conversion");
    image.pixels.resize(size_t(image.width)*image.height*4);
    check(WebPDecodeRGBAInto(data,size,image.pixels.data(),image.pixels.size(),int(image.width*4))!=nullptr,"WebP decode failed");
    for(const auto* chunk:{"ICCP","EXIF"}) {
        WebPChunkIterator it{};
        if(WebPDemuxGetChunk(demux.get(),chunk,1,&it)) {
            struct Release{WebPChunkIterator& i;~Release(){WebPDemuxReleaseChunkIterator(&i);}} release{it};
            copy_metadata(chunk[0]=='I'?image.icc:image.exif,it.chunk.bytes,it.chunk.size);
        }
    }
    strip_exif_prefix(image.exif);image.orientation=regular_orientation(image.exif);return image;
}
struct TiffInput { const uint8_t* data; size_t size,at=0; };
tmsize_t tiff_read(thandle_t h,void* out,tmsize_t n) {
    auto& in=*static_cast<TiffInput*>(h);if(n<0)return -1;
    const size_t count=std::min(size_t(n),in.size-in.at);std::memcpy(out,in.data+in.at,count);in.at+=count;return tmsize_t(count);
}
tmsize_t tiff_write(thandle_t,void*,tmsize_t){return -1;}
toff_t tiff_seek(thandle_t h,toff_t offset,int whence) {
    auto& in=*static_cast<TiffInput*>(h);
    const uint64_t base=whence==SEEK_SET?0:whence==SEEK_CUR?in.at:in.size;
    // libtiff represents negative relative offsets in the unsigned toff_t type.
    const int64_t delta=static_cast<int64_t>(offset);
    if((delta<0 && uint64_t(-(delta+1))+1>base) || (delta>=0 && uint64_t(delta)>in.size-base))return toff_t(-1);
    const uint64_t at=delta<0?base-(uint64_t(-(delta+1))+1):base+uint64_t(delta);
    in.at=size_t(at);return toff_t(at);
}
int tiff_close(thandle_t){return 0;}
toff_t tiff_size(thandle_t h){return static_cast<TiffInput*>(h)->size;}
int tiff_map(thandle_t,void**,toff_t*){return 0;}
void tiff_unmap(thandle_t,void*,toff_t){}
RegularImage decode_tiff(const uint8_t* data,size_t size,bool metadata_only=false) {
    RegularImage image;image.format="TIFF";TiffInput input{data,size};
    std::unique_ptr<TIFF,decltype(&TIFFClose)> tiff(TIFFClientOpen("memory","rm",&input,tiff_read,tiff_write,tiff_seek,tiff_close,tiff_size,tiff_map,tiff_unmap),TIFFClose);
    check(bool(tiff),"Invalid TIFF");auto* tif=tiff.get();
    uint16_t bits=0,samples=0,photo=0,planar=0,format=0,orient=1,extras=0;uint16_t* extra_types=nullptr;
    TIFFGetField(tif,TIFFTAG_IMAGEWIDTH,&image.width);TIFFGetField(tif,TIFFTAG_IMAGELENGTH,&image.height);dimensions(image.width,image.height);
    TIFFGetFieldDefaulted(tif,TIFFTAG_BITSPERSAMPLE,&bits);TIFFGetFieldDefaulted(tif,TIFFTAG_SAMPLESPERPIXEL,&samples);
    TIFFGetFieldDefaulted(tif,TIFFTAG_PHOTOMETRIC,&photo);TIFFGetFieldDefaulted(tif,TIFFTAG_PLANARCONFIG,&planar);
    TIFFGetFieldDefaulted(tif,TIFFTAG_SAMPLEFORMAT,&format);TIFFGetFieldDefaulted(tif,TIFFTAG_ORIENTATION,&orient);
    const bool log=photo==PHOTOMETRIC_LOGLUV||photo==PHOTOMETRIC_LOGL;
    uint16_t compression=0;TIFFGetFieldDefaulted(tif,TIFFTAG_COMPRESSION,&compression);
    if(log) {
        check(TIFFSetField(tif,TIFFTAG_SGILOGDATAFMT,SGILOGDATAFMT_FLOAT)==1,"Unsupported LogLuv TIFF compression");bits=32;format=SAMPLEFORMAT_IEEEFP;
    } else if(compression==COMPRESSION_PIXARLOG) {
        check(TIFFSetField(tif,TIFFTAG_PIXARLOGDATAFMT,PIXARLOGDATAFMT_FLOAT)==1,"Unsupported PixarLog TIFF");bits=32;format=SAMPLEFORMAT_IEEEFP;
    }
    const bool floating=format==SAMPLEFORMAT_IEEEFP;
    check((floating&&(bits==16||bits==32||bits==64))||((bits==8||bits==16)&&format==SAMPLEFORMAT_UINT),"Unsupported TIFF sample representation");
    const unsigned colors=photo==PHOTOMETRIC_RGB||photo==PHOTOMETRIC_LOGLUV?3:1;
    image.gray=colors==1;
    check(log || photo==PHOTOMETRIC_RGB || photo==PHOTOMETRIC_MINISBLACK || photo==PHOTOMETRIC_MINISWHITE,"TIFF requires RGB or grayscale photometric data");
    check(planar==PLANARCONFIG_CONTIG || planar==PLANARCONFIG_SEPARATE,"Unsupported TIFF planar layout");
    check(samples==colors || samples==colors+1,"Unsupported TIFF channel count");
    TIFFGetFieldDefaulted(tif,TIFFTAG_EXTRASAMPLES,&extras,&extra_types);
    if(samples>colors)check(extras==1 && extra_types && (extra_types[0]==EXTRASAMPLE_UNASSALPHA||(floating&&extra_types[0]==EXTRASAMPLE_ASSOCALPHA)),"Unsupported TIFF alpha association");
    check(orient>=1 && orient<=8,"Invalid TIFF orientation");image.orientation=orient;image.bits=bits;if(floating)image.transfer=8;
    uint32_t icc_size=0;void* icc=nullptr;
    if(TIFFGetField(tif,TIFFTAG_ICCPROFILE,&icc_size,&icc))copy_metadata(image.icc,static_cast<uint8_t*>(icc),icc_size);
    if(floating&&image.icc.empty()&&!log) {
        float* primaries=nullptr;float* white=nullptr;
        if(TIFFGetField(tif,TIFFTAG_PRIMARYCHROMATICITIES,&primaries)&&primaries)std::copy_n(primaries,6,image.chromaticities.begin());
        if(TIFFGetField(tif,TIFFTAG_WHITEPOINT,&white)&&white){image.chromaticities[6]=white[0];image.chromaticities[7]=white[1];}
    }
    uint16_t *tr=nullptr,*tg=nullptr,*tb=nullptr;
    check(!image.icc.empty() || !TIFFGetField(tif,TIFFTAG_TRANSFERFUNCTION,&tr,&tg,&tb),"TIFF transfer tables require an ICC profile");
    // Multiple pages are deliberately rejected rather than exporting one silently.
    check(TIFFNumberOfDirectories(tif)==1,"Multipage TIFF is not supported for still conversion");
    if(metadata_only)return image;
    if(floating) {
        image.floating_pixels.resize(size_t(image.width)*image.height*4);
        for(size_t i=3;i<image.floating_pixels.size();i+=4)image.floating_pixels[i]=1;
    } else image.pixels.resize(size_t(image.width)*image.height*4*(bits/8));
    auto store=[&](unsigned x,unsigned y,unsigned c,const uint8_t* src){
        if(floating) {
            float value=0;
            if(bits==16){uint16_t half;std::memcpy(&half,src,2);value=half_to_float(half);}
            else if(bits==32)std::memcpy(&value,src,4);
            else {double v;std::memcpy(&v,src,8);value=float(v);}
            check(std::isfinite(value),"Non-finite TIFF sample");
            if(photo==PHOTOMETRIC_MINISWHITE&&c==0)value=1-value;
            auto* pixel=image.floating_pixels.data()+(size_t(y)*image.width+x)*4;
            if(c<colors){if(colors==1)pixel[0]=pixel[1]=pixel[2]=value;else pixel[c]=value;}else pixel[3]=value;
            return;
        }
        uint16_t value=src[0];if(bits==16)std::memcpy(&value,src,2);
        if(photo==PHOTOMETRIC_MINISWHITE && c==0)value=uint16_t((bits==16?65535:255)-value);
        const size_t at=(size_t(y)*image.width+x)*4*(bits/8);
        for(unsigned out=0;out<4;++out)if((c<colors && (colors==1?out<3:out==c)) || (c==colors && out==3)) {
            if(bits==16)std::memcpy(image.pixels.data()+at+out*2,&value,2);else image.pixels[at+out]=uint8_t(value);
        }
    };
    // Initialize opaque alpha; all source samples are copied without F16 conversion.
    std::fill(image.pixels.begin(),image.pixels.end(),255);
    const unsigned planes=planar==PLANARCONFIG_SEPARATE?samples:1;
    if(TIFFIsTiled(tif)) {
        uint32_t tw=0,th=0;TIFFGetField(tif,TIFFTAG_TILEWIDTH,&tw);TIFFGetField(tif,TIFFTAG_TILELENGTH,&th);
        const tmsize_t tile_size=TIFFTileSize(tif),row_size=TIFFTileRowSize(tif);
        check(tw && th && tile_size>0 && row_size>0 && uint64_t(tile_size)<=640000000,"Invalid TIFF tile layout");
        check(uint64_t(row_size)>=uint64_t(tw)*(bits/8)*(planes==1?samples:1) && uint64_t(row_size)*th<=uint64_t(tile_size),"Invalid TIFF tile stride");
        std::vector<uint8_t> tile(static_cast<size_t>(tile_size));
        for(unsigned p=0;p<planes;++p)for(unsigned y=0;y<image.height;y+=th)for(unsigned x=0;x<image.width;x+=tw) {
            check(TIFFReadTile(tif,tile.data(),x,y,0,uint16_t(p))==tile_size,"TIFF tile decode failed");
            for(unsigned dy=0;dy<std::min(th,image.height-y);++dy)for(unsigned dx=0;dx<std::min(tw,image.width-x);++dx)
                for(unsigned c=0;c<(planes==1?samples:1);++c)store(x+dx,y+dy,planes==1?c:p,tile.data()+size_t(dy)*row_size+(size_t(dx)*(planes==1?samples:1)+c)*(bits/8));
        }
    } else {
        const tmsize_t stride=TIFFScanlineSize(tif);
        check(stride>0 && uint64_t(stride)>=uint64_t(image.width)*(bits/8)*(planes==1?samples:1) && uint64_t(stride)<=640000000,"Invalid TIFF row layout");
        std::vector<uint8_t> row(static_cast<size_t>(stride));
        for(unsigned p=0;p<planes;++p)for(unsigned y=0;y<image.height;++y) {
            check(TIFFReadScanline(tif,row.data(),y,uint16_t(p))==1,"TIFF row decode failed");
            for(unsigned x=0;x<image.width;++x)for(unsigned c=0;c<(planes==1?samples:1);++c)store(x,y,planes==1?c:p,row.data()+(size_t(x)*(planes==1?samples:1)+c)*(bits/8));
        }
    }
    if(floating) {
        double stonits=0;const bool absolute=TIFFGetField(tif,TIFFTAG_STONITS,&stonits)&&std::isfinite(stonits)&&stonits>0;
        for(size_t i=0;i<image.floating_pixels.size();i+=4) {
            auto* p=image.floating_pixels.data()+i;
            if(log&&colors==3) {const float X=p[0],Y=p[1],Z=p[2];p[0]=3.2404542f*X-1.5371385f*Y-.4985314f*Z;p[1]=-.969266f*X+1.8760108f*Y+.041556f*Z;p[2]=.0556434f*X-.2040259f*Y+1.0572252f*Z;}
            if(samples>colors&&extra_types[0]==EXTRASAMPLE_ASSOCALPHA)for(unsigned c=0;c<3;++c)p[c]=p[3]>0?p[c]/p[3]:0;
            if(absolute)for(unsigned c=0;c<3;++c)p[c]*=float(stonits/203.);
        }
    }
    return image;
}
RegularImage decode_bitmap(const uint8_t* data,size_t size) {
    RegularImage image;image.format=data[0]=='B'?"BMP":"GIF";
    if(image.format=="GIF") {
        check(size>=13,"Truncated GIF");size_t at=13;unsigned frames=0;
        if(data[10]&128)at+=size_t(3)<<(1+(data[10]&7));
        auto subblocks=[&]{for(;;){check(at<size,"Truncated GIF block");const unsigned n=data[at++];if(!n)break;check(n<=size-at,"Truncated GIF block");at+=n;}};
        while(at<size) {
            const unsigned type=data[at++];if(type==0x3b)break;
            if(type==0x21){check(at<size,"Truncated GIF extension");++at;subblocks();}
            else if(type==0x2c) {
                check(++frames==1,"Animated GIF is not supported for still conversion");
                check(size-at>=9,"Truncated GIF image");const unsigned packed=data[at+8];at+=9;
                if(packed&128)at+=size_t(3)<<(1+(packed&7));
                check(at<size,"Truncated GIF palette");++at;subblocks();
            } else throw std::runtime_error("Invalid GIF block");
        }
    }
    int w=0,h=0,channels=0;
    check(size<=size_t(std::numeric_limits<int>::max()) && stbi_info_from_memory(data,int(size),&w,&h,&channels),"Invalid bitmap header");
    dimensions(unsigned(w),unsigned(h));
    std::unique_ptr<stbi_uc,decltype(&stbi_image_free)> pixels(stbi_load_from_memory(data,int(size),&w,&h,&channels,4),stbi_image_free);
    check(bool(pixels),"Bitmap decode failed");image.width=unsigned(w);image.height=unsigned(h);dimensions(image.width,image.height);
    image.pixels.assign(pixels.get(),pixels.get()+size_t(w)*h*4);
    if(image.format=="BMP" && size>=138 && le32(data+14)>=124) {
        const uint32_t cs=le32(data+70);
        check(cs!=0x4c494e4b,"Linked BMP profiles are unsupported");
        if(cs==0x4d424544) {
            const uint64_t at=uint64_t(14)+le32(data+126),n=le32(data+130);
            check(at<=size && n<=size-at,"Invalid BMP ICC offset");copy_metadata(image.icc,data+at,size_t(n));
        } else check(cs==0x73524742 || cs==0x57696e20,"Calibrated BMP requires an embedded ICC profile");
    } else if(image.format=="BMP" && size>=122 && le32(data+14)>=108) {
        const uint32_t cs=le32(data+70);check(cs==0x73524742 || cs==0x57696e20,"Calibrated BMP requires an embedded ICC profile");
    }
    return image;
}
} // namespace

unsigned regular_orientation(const std::vector<uint8_t>& exif) {
    if(exif.size()<8)return 1;
    const auto* d=exif.data();const bool le=d[0]=='I' && d[1]=='I';
    if(!le && !(d[0]=='M' && d[1]=='M'))return 1;
    auto u16=[&](size_t p){return le?unsigned(d[p])|unsigned(d[p+1])<<8:unsigned(d[p])<<8|d[p+1];};
    auto u32=[&](size_t p){return le?uint32_t(u16(p))|uint32_t(u16(p+2))<<16:uint32_t(u16(p))<<16|u16(p+2);};
    const size_t at=u32(4);if(at>exif.size()-2)return 1;const unsigned n=u16(at);
    if(n>(exif.size()-at-2)/12)return 1;
    for(unsigned i=0;i<n;++i){const size_t p=at+2+12*i;if(u16(p)==0x112 && u16(p+2)==3 && u32(p+4)==1){const auto v=u16(p+8);return v>=1 && v<=8?v:1;}}
    return 1;
}
bool is_regular_image(const uint8_t* data,size_t size) {
    if(is_linear_image(data,size))return true;
    if(signature(data,size,"\x89PNG\r\n\x1a\n",8) || signature(data,size,"BM",2) || signature(data,size,"GIF87a",6) || signature(data,size,"GIF89a",6))return true;
    if(size>=12 && signature(data,size,"RIFF",4) && !std::memcmp(data+8,"WEBP",4))return true;
    if(tiff_signature(data,size)) {
        // DNG/NEF/CR2 and other TIFF-based sensor containers belong to LibRaw.
        auto raw=std::make_unique<LibRaw>();
        if(raw->open_buffer(const_cast<uint8_t*>(data),size)==LIBRAW_SUCCESS && raw->imgdata.idata.raw_count)return false;
        return true;
    }
    return false;
}
RegularImage decode_regular(const uint8_t* data,size_t size) {
    check(data && size>=2,"Truncated raster");
    if(is_linear_image(data,size))return decode_linear_image(data,size);
    if(signature(data,size,"\x89PNG\r\n\x1a\n",8))return decode_png(data,size);
    if(size>=12 && signature(data,size,"RIFF",4) && !std::memcmp(data+8,"WEBP",4))return decode_webp(data,size);
    if(tiff_signature(data,size))return decode_tiff(data,size);
    if(signature(data,size,"BM",2) || signature(data,size,"GIF87a",6) || signature(data,size,"GIF89a",6))return decode_bitmap(data,size);
    throw std::runtime_error("Unsupported regular image format");
}
jxrc::ImageInfo inspect_regular(const uint8_t* data,size_t size) {
    // Color and dimensions are available before IDAT. Inspection must not
    // decompress a full raster just to decide whether an HDR target accepts it.
    if(is_linear_image(data,size))return inspect_regular(decode_linear_image(data,size,true));
    if(tiff_signature(data,size))return inspect_regular(decode_tiff(data,size,true));
    return inspect_regular(signature(data,size,"\x89PNG\r\n\x1a\n",8)?decode_png(data,size,true):decode_regular(data,size));
}
jxrc::ImageInfo inspect_regular(const RegularImage& image) {
    jxrc::ImageInfo out;out.width=image.width;out.height=image.height;out.bits=image.bits;out.format=image.format;out.hdr=image.hdr();
    out.transfer=image.transfer==8?"Linear":image.hdr()?(image.transfer==16?"PQ":"HLG"):image.icc.empty()?"sRGB":"ICC";
    out.primaries=image.icc.empty()?(image.primaries==9?"BT.2020":image.primaries==12?"Display P3":"BT.709"):"ICC";
    if(image.transfer==8&&image.icc.empty()&&(image.format=="EXR"||image.format=="Radiance"||image.format=="PFM"||image.format=="TIFF")) {
        const auto& c=image.chromaticities;
        if(std::abs(c[0]-.708f)<.0001f&&std::abs(c[2]-.170f)<.0001f)out.primaries="BT.2020";
        else if(std::abs(c[0]-.64f)<.0001f&&std::abs(c[2]-.30f)<.0001f&&std::abs(c[6]-.3127f)<.0001f)out.primaries="BT.709";
        else out.primaries="Custom RGB";
    }
    if(image.orientation>=5)std::swap(out.width,out.height);
    return out;
}
void regular_to_linear(const RegularImage& image,std::vector<uint16_t>& pixels,uhdr_color_gamut_t& gamut) {
    if(!image.floating_pixels.empty()) {
        std::vector<float> linear;regular_to_linear(image,linear,gamut);pixels.resize(linear.size());
        std::transform(linear.begin(),linear.end(),pixels.begin(),[](float v){return float_to_half(v);});return;
    }
    if(image.hdr() || (image.icc.empty() && image.primaries!=1)) {
        cicp_rgb_to_linear(image.pixels.data(),size_t(image.width)*image.height,image.bits,
                           image.primaries,image.transfer,pixels,gamut);return;
    }
    skcms_ICCProfile source=*skcms_sRGB_profile(),destination;
    if(!image.icc.empty())check(skcms_Parse(image.icc.data(),image.icc.size(),&source),"Invalid raster ICC profile");
    check(make_linear_bt2020_profile(&destination),"Cannot build working profile");
    pixels.resize(size_t(image.width)*image.height*4);const uint16_t endian=1;
    const auto format=image.bits==8?skcms_PixelFormat_RGBA_8888:*reinterpret_cast<const uint8_t*>(&endian)?skcms_PixelFormat_RGBA_16161616LE:skcms_PixelFormat_RGBA_16161616BE;
    check(skcms_Transform(image.pixels.data(),format,skcms_AlphaFormat_Unpremul,&source,pixels.data(),skcms_PixelFormat_RGBA_hhhh,skcms_AlphaFormat_Unpremul,&destination,size_t(image.width)*image.height),"Raster color transform failed");
    gamut=UHDR_CG_BT_2100;
}
void regular_to_linear(const RegularImage& image,std::vector<float>& pixels,uhdr_color_gamut_t& gamut) {
    if(!image.floating_pixels.empty()) {
        skcms_ICCProfile source,destination;skcms_Init(&source);skcms_SetTransferFunction(&source,skcms_Identity_TransferFunction());
        const auto& c=image.chromaticities;skcms_Matrix3x3 matrix{};
        check(skcms_PrimariesToXYZD50(c[0],c[1],c[2],c[3],c[4],c[5],c[6],c[7],&matrix),"Invalid linear image primaries");skcms_SetXYZD50(&source,&matrix);
        if(!image.icc.empty())check(skcms_Parse(image.icc.data(),image.icc.size(),&source),"Invalid floating-point TIFF ICC profile");
        check(make_linear_bt2020_profile(&destination),"Cannot build working profile");pixels.resize(image.floating_pixels.size());
        check(skcms_Transform(image.floating_pixels.data(),skcms_PixelFormat_RGBA_ffff,skcms_AlphaFormat_Unpremul,&source,pixels.data(),skcms_PixelFormat_RGBA_ffff,skcms_AlphaFormat_Unpremul,&destination,size_t(image.width)*image.height),"Linear HDR color transform failed");
        for(float v:pixels)check(std::isfinite(v),"Non-finite linear HDR color transform");
        gamut=UHDR_CG_BT_2100;return;
    }
    if(image.hdr() || (image.icc.empty() && image.primaries!=1)) {
        cicp_rgb_to_linear(image.pixels.data(),size_t(image.width)*image.height,image.bits,
                           image.primaries,image.transfer,pixels,gamut);return;
    }
    skcms_ICCProfile source=*skcms_sRGB_profile(),destination;
    if(!image.icc.empty())check(skcms_Parse(image.icc.data(),image.icc.size(),&source),"Invalid raster ICC profile");
    check(make_linear_bt2020_profile(&destination),"Cannot build working profile");
    pixels.resize(size_t(image.width)*image.height*4);const uint16_t endian=1;
    const auto format=image.bits==8?skcms_PixelFormat_RGBA_8888:*reinterpret_cast<const uint8_t*>(&endian)?skcms_PixelFormat_RGBA_16161616LE:skcms_PixelFormat_RGBA_16161616BE;
    check(skcms_Transform(image.pixels.data(),format,skcms_AlphaFormat_Unpremul,&source,pixels.data(),skcms_PixelFormat_RGBA_ffff,skcms_AlphaFormat_Unpremul,&destination,size_t(image.width)*image.height),"Raster color transform failed");
    gamut=UHDR_CG_BT_2100;
}
