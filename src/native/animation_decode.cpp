#include "animation_decode.h"
#include "image_size.h"
#include <webp/demux.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace {
void require(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
void check_cancelled(const std::function<bool()>& cancelled) {
    if(cancelled&&cancelled())throw std::runtime_error("Animation decode cancelled");
}
bool signature(const uint8_t* data,size_t size,const char* value,size_t length) {
    return size>=length&&!std::memcmp(data,value,length);
}
unsigned le16(const uint8_t* data) {return unsigned(data[0])|(unsigned(data[1])<<8);}
uint32_t be32(const uint8_t* data) {
    return (uint32_t(data[0])<<24)|(uint32_t(data[1])<<16)|(uint32_t(data[2])<<8)|data[3];
}
void append32(std::vector<uint8_t>& bytes,uint32_t value) {
    for(int shift=24;shift>=0;shift-=8)bytes.push_back(uint8_t(value>>shift));
}

struct FrameRect {
    unsigned x=0,y=0,width=0,height=0;
    void validate(unsigned w,unsigned h) const {
        require(width&&height&&x<=w&&y<=h&&width<=w-x&&height<=h-y,"Animation frame exceeds the canvas");
    }
};
size_t canvas_samples(unsigned w,unsigned h) {
    require(w&&h&&fits_working_image(w,h),"Invalid animation canvas dimensions");
    return size_t(w)*h*4;
}
template<class T>
void clear_rect(std::vector<T>& canvas,unsigned width,const FrameRect& rect,size_t channels=4) {
    for(unsigned y=0;y<rect.height;++y) {
        const auto start=canvas.begin()+(size_t(y+rect.y)*width+rect.x)*channels;
        std::fill_n(start,size_t(rect.width)*channels,T{});
    }
}

// Parse GIF blocks without decompressing any frame for inspection.
class GifReader {
public:
    GifReader(const uint8_t* data,size_t size):data_(data),size_(size) {
        require(size>=13,"Truncated GIF header");
        width=le16(data+6);height=le16(data+8);
        canvas_samples(width,height);
        start_=13+table_size(data[10]);
        require(start_<=size,"Truncated GIF color table");
    }
    unsigned width=0,height=0;
    unsigned walk(const std::function<void(const FrameRect&,unsigned,size_t,size_t,size_t)>& frame,
                  bool first_only,const std::function<bool()>& cancelled={}) const {
        size_t at=start_,control=0;
        unsigned count=0,disposal=0;
        const auto subblocks=[&] {
            for(;;) {
                require(at<size_,"Truncated GIF data");
                const unsigned n=data_[at++];
                if(!n)break;
                require(n<=size_-at,"Truncated GIF data");
                at+=n;
            }
        };
        while(at<size_) {
            check_cancelled(cancelled);
            const size_t begin=at;
            const auto type=data_[at++];
            if(type==0x3b) {
                require(count!=0,"GIF contains no image frames");
                return count;
            }
            if(type==0x21) {
                require(at<size_,"Truncated GIF extension");
                const auto label=data_[at++];
                if(label==0xf9) {
                    require(size_-at>=6&&data_[at]==4&&data_[at+5]==0,"Invalid GIF frame control");
                    control=begin;disposal=(data_[at+1]>>2)&7;
                } else if(label==0x01) {
                    control=0;disposal=0; // A plain-text graphic consumes the GCE.
                }
                subblocks();
                continue;
            }
            require(type==0x2c&&size_-at>=9,"Invalid GIF image block");
            const FrameRect rect{le16(data_+at),le16(data_+at+2),le16(data_+at+4),le16(data_+at+6)};
            rect.validate(width,height);
            at+=9+table_size(data_[at+8]);
            require(at<size_,"Truncated GIF frame palette or data");
            ++at; // LZW code size.
            subblocks();
            ++count;
            if(frame)frame(rect,disposal,control,begin,at);
            if(first_only||(!frame&&count==2))return count;
            control=0;disposal=0;
        }
        throw std::runtime_error("Missing GIF trailer");
    }

    void decode(const AnimationFrameReader& frame,bool first_only,const std::function<bool()>& cancelled) const {
        std::vector<uint8_t> canvas(canvas_samples(width,height)),previous,patch;
        std::array<uint8_t,4> background{};
        const size_t palette_index=13+size_t(data_[11])*3;
        if(palette_index+3<=start_) {
            std::copy_n(data_+palette_index,3,background.begin());
            background[3]=255;
        }
        const auto fill=[&](const FrameRect& rect,bool transparent) {
            if(transparent||!background[3]) {
                clear_rect(canvas,width,rect);
                return;
            }
            for(unsigned y=0;y<rect.height;++y) {
                auto* dest=canvas.data()+(size_t(y+rect.y)*width+rect.x)*4;
                for(unsigned x=0;x<rect.width;++x,dest+=4)
                    std::copy(background.begin(),background.end(),dest);
            }
        };
        bool first=true;
        walk([&](const FrameRect& rect,unsigned disposal,size_t control,size_t begin,size_t end) {
            const bool transparent=control&&(data_[control+3]&1);
            if(first&&!transparent)fill({0,0,width,height},false);
            first=false;
            // Decode the rectangle as an independent GIF. Composition and
            // disposal belong to the canvas, including restore-to-previous.
            patch.assign(data_,data_+start_);
            patch[6]=uint8_t(rect.width);patch[7]=uint8_t(rect.width>>8);
            patch[8]=uint8_t(rect.height);patch[9]=uint8_t(rect.height>>8);
            patch[11]=0; // Transparent pixels in the rectangle must stay transparent.
            if(control)patch.insert(patch.end(),data_+control,data_+control+8);
            const auto descriptor=patch.size();
            patch.insert(patch.end(),data_+begin,data_+end);
            std::fill_n(patch.begin()+descriptor+1,4,0);
            patch.push_back(0x3b);
            const auto image=decode_gif_patch(patch.data(),patch.size());
            if(!first_only&&disposal==3)previous=canvas;
            for(unsigned y=0;y<rect.height;++y) {
                const auto* src=image.pixels.data()+size_t(y)*rect.width*4;
                auto* dst=canvas.data()+(size_t(y+rect.y)*width+rect.x)*4;
                for(unsigned x=0;x<rect.width;++x,src+=4,dst+=4)
                    if(src[3])std::copy_n(src,4,dst);
            }
            RegularImage result;
            result.format="GIF";result.width=width;result.height=height;
            result.pixels=first_only?std::move(canvas):canvas;
            frame({std::move(result),{}});
            if(first_only)return;
            if(disposal==2)fill(rect,transparent);
            else if(disposal==3)canvas.swap(previous);
        },first_only,cancelled);
    }
private:
    static size_t table_size(unsigned packed) {return packed&128?size_t(3)<<(1+(packed&7)):0;}
    const uint8_t* data_;
    size_t size_,start_;
};

bool decode_webp_animation(const uint8_t* data,size_t size,const AnimationFrameReader& frame,
                           bool first_only,const std::function<bool()>& cancelled) {
    const WebPData input{data,size};
    std::unique_ptr<WebPDemuxer,decltype(&WebPDemuxDelete)> demux(WebPDemux(&input),WebPDemuxDelete);
    require(bool(demux),"Invalid WebP");
    if(!(WebPDemuxGetI(demux.get(),WEBP_FF_FORMAT_FLAGS)&ANIMATION_FLAG))return false;
    const auto metadata=webp_metadata(data,size);
    const auto count=canvas_samples(metadata.width,metadata.height);
    WebPAnimDecoderOptions options{};
    require(WebPAnimDecoderOptionsInit(&options)!=0,"Cannot initialize WebP animation");
    options.color_mode=MODE_RGBA;
    std::unique_ptr<WebPAnimDecoder,decltype(&WebPAnimDecoderDelete)> decoder(
        WebPAnimDecoderNew(&input,&options),WebPAnimDecoderDelete);
    require(bool(decoder),"Cannot open animated WebP");
    unsigned frames=0;
    while(WebPAnimDecoderHasMoreFrames(decoder.get())) {
        check_cancelled(cancelled);
        uint8_t* rgba=nullptr;
        int timestamp=0;
        require(WebPAnimDecoderGetNext(decoder.get(),&rgba,&timestamp)!=0,"Invalid animated WebP frame");
        auto image=metadata;
        image.pixels.assign(rgba,rgba+count);
        frame({std::move(image),{}});
        ++frames;
        if(first_only)break;
    }
    require(frames!=0,"WebP contains no image frames");
    return true;
}

struct PngChunk {
    const uint8_t* name;
    uint32_t size;
    const uint8_t* data() const {return name+4;}
    bool is(const char* type) const {return !std::memcmp(name,type,4);}
};
template<class Function>
void png_chunks(const uint8_t* data,size_t size,const Function& consume) {
    for(size_t at=8;size-at>=12;) {
        const PngChunk chunk{data+at+4,be32(data+at)};
        require(chunk.size<=size-at-12&&chunk.size<=0x7fffffffu,"Truncated PNG chunk");
        if(!consume(chunk))return;
        if(chunk.is("IEND"))return;
        at+=size_t(chunk.size)+12;
    }
    throw std::runtime_error("Missing PNG end chunk");
}
void check_crc(const PngChunk& chunk) {
    const auto crc=crc32(crc32(0L,Z_NULL,0),chunk.name,chunk.size+4);
    require(crc==be32(chunk.data()+chunk.size),"Invalid PNG animation chunk CRC");
}
void append_chunk(std::vector<uint8_t>& out,const char* type,const uint8_t* data,uint32_t size) {
    append32(out,size);
    const size_t start=out.size();
    out.insert(out.end(),type,type+4);
    if(size)out.insert(out.end(),data,data+size);
    append32(out,uint32_t(crc32(crc32(0L,Z_NULL,0),out.data()+start,size+4)));
}
struct PngFrame {
    FrameRect rect;
    uint8_t disposal=0,blend=0;
};
void compose_png_linear(std::vector<float>& canvas,const std::vector<float>& frame,unsigned stride,
                        const PngFrame& control) {
    const auto& rect=control.rect;
    for(unsigned y=0;y<rect.height;++y) {
        const auto* src=frame.data()+size_t(y)*rect.width*4;
        auto* dst=canvas.data()+(size_t(y+rect.y)*stride+rect.x)*4;
        for(unsigned x=0;x<rect.width;++x,src+=4,dst+=4) {
            if(!control.blend||src[3]>=1.f)std::copy_n(src,4,dst);
            else if(src[3]>0.f) {
                const float base=dst[3]*(1.f-src[3]),alpha=src[3]+base;
                for(unsigned c=0;c<3;++c)dst[c]=(src[c]*src[3]+dst[c]*base)/alpha;
                dst[3]=alpha;
            }
        }
    }
}

bool decode_apng(const uint8_t* data,size_t size,const AnimationFrameReader& frame,bool first_only,
                 const std::function<bool()>& cancelled) {
    unsigned expected=0;
    bool animated=false;
    // acTL must precede the default image. Avoid traversing still PNG payloads.
    png_chunks(data,size,[&](const PngChunk& chunk) {
        if(chunk.is("acTL")) {
            require(!animated&&chunk.size==8,"Invalid PNG animation control");
            check_crc(chunk);animated=true;expected=be32(chunk.data());
            require(expected!=0,"PNG animation contains no image frames");
        }
        return !chunk.is("IDAT");
    });
    if(!animated)return false;
    require(size>=33&&be32(data+8)==13&&!std::memcmp(data+12,"IHDR",4),"Missing PNG animation header");
    check_crc({data+12,13});
    const unsigned w=be32(data+16),h=be32(data+20);
    const size_t samples=canvas_samples(w,h);
    const unsigned bytes_per_sample=data[24]==16?2:1;
    std::vector<uint8_t> global,local,canvas,previous,exif;
    std::vector<float> linear,linear_previous;
    bool blend=false,idat=false;
    png_chunks(data,size,[&](const PngChunk& chunk) {
        check_cancelled(cancelled);
        if(chunk.is("IDAT"))idat=true;
        if(chunk.is("fcTL"))blend|=!first_only&&chunk.size==26&&chunk.data()[25]==1;
        else if(chunk.is("eXIf")) {
            check_crc(chunk);
            require(chunk.size<=16u*1024*1024,"PNG EXIF exceeds 16 MiB");
            exif.assign(chunk.data(),chunk.data()+chunk.size);
        } else if(!idat&&!chunk.is("IHDR")&&!chunk.is("acTL")&&!chunk.is("IEND")&&!chunk.is("fdAT"))
            global.insert(global.end(),chunk.name-4,chunk.data()+chunk.size+4);
        return true;
    });
    if(blend)linear.resize(samples);
    else canvas.resize(samples*bytes_per_sample);
    unsigned decoded=0;
    uint64_t sequence=0;
    std::optional<PngFrame> control;
    const auto emit=[&] {
        if(!control)return;
        require(++decoded<=expected,"PNG animation frame count mismatch");
        append_chunk(local,"IEND",nullptr,0);
        auto image=decode_png_frame(local.data(),local.size());
        const auto& rect=control->rect;
        std::optional<uhdr_color_gamut_t> gamut;
        if(blend) {
            std::vector<float> patch;
            uhdr_color_gamut_t cg;
            regular_to_linear(image,patch,cg);
            gamut=cg;
            if(control->disposal==2)linear_previous=linear;
            compose_png_linear(linear,patch,w,*control);
            image.floating_pixels=linear;
            image.pixels.clear();
        } else {
            if(!first_only&&control->disposal==2)previous=canvas;
            const size_t row=size_t(rect.width)*4*bytes_per_sample;
            for(unsigned y=0;y<rect.height;++y)
                std::copy_n(image.pixels.data()+size_t(y)*row,row,
                            canvas.data()+(size_t(y+rect.y)*w+rect.x)*4*bytes_per_sample);
            image.pixels=first_only?std::move(canvas):canvas;
        }
        image.width=w;image.height=h;
        image.exif=exif;image.orientation=regular_orientation(exif);
        frame({std::move(image),gamut});
        if(first_only)return;
        if(control->disposal==1) {
            if(blend)clear_rect(linear,w,rect);
            else clear_rect(canvas,w,rect,4*bytes_per_sample);
        } else if(control->disposal==2) {
            if(blend)linear.swap(linear_previous);
            else canvas.swap(previous);
        }
    };
    png_chunks(data,size,[&](const PngChunk& chunk) {
        check_cancelled(cancelled);
        const auto* bytes=chunk.data();
        if(chunk.is("fcTL")||chunk.is("IEND")) {
            emit();
            if(first_only&&decoded)return false;
            if(chunk.is("IEND"))return false;
            require(chunk.size==26&&be32(bytes)==sequence++,"Invalid PNG frame control sequence");
            check_crc(chunk);
            control=PngFrame{{be32(bytes+12),be32(bytes+16),be32(bytes+4),be32(bytes+8)},bytes[24],bytes[25]};
            control->rect.validate(w,h);
            require(control->disposal<=2&&control->blend<=1,"Invalid PNG frame disposal or blend mode");
            local.assign(data,data+8);
            std::vector<uint8_t> header;
            append32(header,control->rect.width);append32(header,control->rect.height);
            header.insert(header.end(),data+24,data+29);
            append_chunk(local,"IHDR",header.data(),uint32_t(header.size()));
            local.insert(local.end(),global.begin(),global.end());
        } else if(chunk.is("fdAT")) {
            require(control&&chunk.size>=4&&be32(bytes)==sequence++,"Invalid PNG frame data sequence");
            check_crc(chunk);
            append_chunk(local,"IDAT",bytes+4,chunk.size-4);
        } else if(chunk.is("IDAT")&&control) {
            // An IDAT preceding the first fcTL is a poster and is skipped.
            require(decoded==0&&control->rect.x==0&&control->rect.y==0&&
                    control->rect.width==w&&control->rect.height==h,"Invalid PNG default animation frame");
            local.insert(local.end(),chunk.name-4,bytes+chunk.size+4);
        }
        return true;
    });
    require(decoded&&(first_only||decoded==expected),"PNG animation frame count mismatch");
    return true;
}
}

jxrc::ImageInfo inspect_gif(const uint8_t* data,size_t size) {
    GifReader gif(data,size);
    RegularImage image;image.width=gif.width;image.height=gif.height;image.format="GIF";
    auto info=inspect_regular(image);info.animated=gif.walk({},false)>1;return info;
}
bool decode_raster_animation(const uint8_t* data,size_t size,const AnimationFrameReader& frame,
                             bool first_only,const std::function<bool()>& cancelled) {
    if(signature(data,size,"GIF87a",6)||signature(data,size,"GIF89a",6)) {
        GifReader(data,size).decode(frame,first_only,cancelled);
        return true;
    }
    if(signature(data,size,"RIFF",4)&&size>=12&&!std::memcmp(data+8,"WEBP",4))
        return decode_webp_animation(data,size,frame,first_only,cancelled);
    if(signature(data,size,"\x89PNG\r\n\x1a\n",8))return decode_apng(data,size,frame,first_only,cancelled);
    return false;
}
