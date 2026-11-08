#include "image_size.h"
// HEIC container/HEVC decoding is native on every platform. Apple/ProXDR
// gain-map interpretation follows LocalViewer's HeifContainer/AppleGainMapDecode.
#include "heif_decode.h"
#include "hdr_parallel.h"
#include "ultrahdr/jpegdecoderhelper.h"
#include <libheif/heif.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace {

void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void checked(heif_error error){if(error.code!=heif_error_Ok)throw std::runtime_error(std::string("HEIC: ")+(error.message?error.message:"decode failed"));}
using Context=std::unique_ptr<heif_context,decltype(&heif_context_free)>;
using Handle=std::unique_ptr<heif_image_handle,decltype(&heif_image_handle_release)>;
using Image=std::unique_ptr<heif_image,decltype(&heif_image_release)>;
struct Input {
    Context context{heif_context_alloc(),heif_context_free};
    Handle base{nullptr,heif_image_handle_release},gain{nullptr,heif_image_handle_release};
    std::vector<uint8_t> exif;
    explicit Input(const uint8_t* data,size_t size){
        check(context!=nullptr,"Cannot allocate HEIC decoder");
        auto* limits=heif_context_get_security_limits(context.get());limits->max_image_size_pixels=UINT32_MAX;limits->max_color_profile_size=16*1024*1024;
        heif_context_set_max_decoding_threads(context.get(),std::min(4u,hdr_pixel_jobs(UINT32_MAX)));
        checked(heif_context_read_from_memory_without_copy(context.get(),data,size,nullptr));
        heif_image_handle* handle=nullptr;checked(heif_context_get_primary_image_handle(context.get(),&handle));base.reset(handle);
        check(heif_image_handle_get_width(handle)>0&&heif_image_handle_get_height(handle)>0&&fits_working_image(heif_image_handle_get_width(handle),heif_image_handle_get_height(handle)),"HEIC dimensions exceed limit");
        heif_item_id id=0;
        if(heif_image_handle_get_list_of_metadata_block_IDs(handle,"Exif",&id,1)==1){
            const auto n=heif_image_handle_get_metadata_size(handle,id);check(n<=1024*1024,"HEIC EXIF exceeds limit");
            std::vector<uint8_t> raw(n);checked(heif_image_handle_get_metadata(handle,id,raw.data()));
            if(n>=4){const size_t at=4+size_t(raw[0])*16777216+size_t(raw[1])*65536+size_t(raw[2])*256+raw[3];if(at<n)exif.assign(raw.begin()+at,raw.end());}
        }
        const int count=heif_image_handle_get_number_of_auxiliary_images(handle,0);check(count>=0&&count<=1024,"HEIC auxiliary count exceeds limit");
        std::vector<heif_item_id> ids(count);heif_image_handle_get_list_of_auxiliary_image_IDs(handle,0,ids.data(),count);
        for(auto aux:ids){
            heif_image_handle* candidate=nullptr;checked(heif_image_handle_get_auxiliary_image_handle(handle,aux,&candidate));Handle owned(candidate,heif_image_handle_release);
            const char* type=nullptr;checked(heif_image_handle_get_auxiliary_type(candidate,&type));const std::string urn=type?type:"";heif_image_handle_release_auxiliary_type(candidate,&type);
            if(urn.find("urn:com:apple:photo:2020:aux:hdrgainmap")!=std::string::npos){check(!gain,"Multiple HEIC gain maps are unsupported");gain=std::move(owned);}
            else if(urn.find("gainmap")!=std::string::npos||urn.find("21496")!=std::string::npos)throw std::runtime_error("Unsupported HEIC gain-map metadata; refusing to discard the map");
        }
    }
};
struct Proxdr {const uint8_t* jpeg=nullptr;size_t size=0;float boost=1,display=1;};
Proxdr proxdr(const uint8_t* data,size_t size){
    Proxdr out;if(size<256)return out;const auto* end=data+size;const auto* tail=end-std::min<size_t>(size,768);
    const uint8_t magic[]={'j','x','r','s'},name[]={'l','o','c','a','l','.','h','d','r'};
    if(std::search(tail,end,magic,magic+4)==end||std::search(tail,end,name,name+9)==end)return out;
    const auto* marker=std::find_end(data,end,magic,magic+4);
    const uint8_t json[]={'[','{'};const auto* catalog=std::find_end(data,marker,json,json+2);
    check(catalog!=marker,"Malformed ProXDR catalog");
    const uint8_t eoi[]={255,217},soi[]={255,216,255};const auto* last=std::find_end(data,catalog,eoi,eoi+2);check(last!=catalog,"Missing ProXDR gain map");
    const auto* first=std::find_end(data,last,soi,soi+3);check(first!=last&&first-data>=84,"Malformed ProXDR JPEG");
    const size_t metadataOffset=size_t(last-data)+18,catalogOffset=size_t(catalog-data);check(metadataOffset<=catalogOffset&&catalogOffset-metadataOffset>=144,"Malformed ProXDR metadata");const auto* metadata=data+metadataOffset;
    auto f32=[&](size_t offset){uint32_t word=0;for(unsigned i=0;i<4;++i)word|=uint32_t(metadata[offset+i])<<(i*8);float f;std::memcpy(&f,&word,4);return f;};
    // A neutral gain map (maximum ratio 1) is valid too. Do not invent
    // headroom or reject the HEIC just because the camera recorded no boost.
    out.boost=f32(16);out.display=f32(132);check(std::isfinite(out.boost)&&out.boost>=1&&out.boost<=64,"Invalid ProXDR boost");
    if(!std::isfinite(out.display)||out.display<1.01f||out.display>64)out.display=1000.f/203;
    out.display=std::max(out.display,out.boost);out.jpeg=first;out.size=size_t(last+2-first);return out;
}
RegularImage decode(heif_image_handle* handle,bool map,unsigned edge){
    heif_decoding_options* rawOptions=heif_decoding_options_alloc();check(rawOptions,"Cannot allocate HEIC options");
    std::unique_ptr<heif_decoding_options,decltype(&heif_decoding_options_free)> options(rawOptions,heif_decoding_options_free);options->convert_hdr_to_8bit=0;
    // Keeping integer precision alone does not retain the source color space:
    // libheif otherwise targets sRGB and can leave PQ/HLG codes tagged as SDR.
    // Perform only the source YCbCr-to-RGB conversion here; our shared color
    // pipeline applies the source transfer and gamut afterward.
    options->output_image_nclx_profile_passthrough=1;
    heif_image* decoded=nullptr;
    // Decode original integer precision. Apple gain maps use the luma codes,
    // rather than RGB conversion which could apply an unrelated color profile.
    const int bits=heif_image_handle_get_luma_bits_per_pixel(handle);check(bits>=8&&bits<=16,"Unsupported HEIC sample depth");
    const auto chroma=map?heif_chroma_undefined:bits>8?heif_chroma_interleaved_RRGGBBAA_LE:heif_chroma_interleaved_RGBA;
    checked(heif_decode_image(handle,&decoded,map?heif_colorspace_undefined:heif_colorspace_RGB,chroma,options.get()));Image image(decoded,heif_image_release);
    if(edge&&std::max(heif_image_get_primary_width(decoded),heif_image_get_primary_height(decoded))>int(edge)){
        const unsigned w=heif_image_get_primary_width(decoded),h=heif_image_get_primary_height(decoded),longest=std::max(w,h);heif_image* small=nullptr;
        checked(heif_image_scale_image(decoded,&small,std::max(1u,unsigned(uint64_t(w)*edge/longest)),std::max(1u,unsigned(uint64_t(h)*edge/longest)),nullptr));image.reset(small);decoded=small;
    }
    RegularImage raster;raster.width=heif_image_get_primary_width(decoded);raster.height=heif_image_get_primary_height(decoded);raster.bits=bits;raster.format="HEIC";
    check(raster.width&&raster.height&&fits_working_image(raster.width,raster.height),"Invalid HEIC image dimensions");
    int stride=0;const auto channel=map?heif_channel_Y:heif_channel_interleaved;const uint8_t* pixels=heif_image_get_plane_readonly(decoded,channel,&stride);check(pixels&&stride>0,"Missing HEIC pixels");
    const unsigned actualBits=heif_image_get_bits_per_pixel_range(decoded,channel);check(actualBits>=8&&actualBits<=16,"Invalid HEIC decoded precision");
    const bool sixteen=bits>8;const unsigned max=(1u<<actualBits)-1;
    raster.pixels.resize(size_t(raster.width)*raster.height*(sixteen?8:4));
    const bool premultiplied=!map&&heif_image_is_premultiplied_alpha(decoded);
    for(unsigned y=0;y<raster.height;++y)for(unsigned x=0;x<raster.width;++x){
        const uint8_t* p=pixels+size_t(y)*stride+x*(map?(sixteen?2:1):(sixteen?8:4));
        unsigned codes[4]{};
        for(unsigned c=0;c<4;++c){if(map&&c==3)codes[c]=max;else if(sixteen){if(map){uint16_t code;std::memcpy(&code,p,2);codes[c]=code;}else codes[c]=p[c*2]|unsigned(p[c*2+1])<<8;}else codes[c]=p[map?0:c];}
        for(unsigned c=0;c<4;++c){double value=std::min(codes[c],max)/double(max);if(premultiplied&&c<3)value=codes[3]?std::min(1.,double(codes[c])/codes[3]):0.;
            const size_t at=(size_t(y)*raster.width+x)*4+c;if(sixteen){const uint16_t code=uint16_t(std::lround(value*65535));std::memcpy(raster.pixels.data()+at*2,&code,2);}else raster.pixels[at]=uint8_t(std::lround(value*255));}
    }
    heif_color_profile_nclx* raw=nullptr;
    // Some conversion shortcuts (notably identity/GBR to interleaved RGB),
    // and scaling, omit profiles on the decoded image. With passthrough enabled
    // the source handle still describes these unchanged transfer-encoded codes.
    if(heif_image_get_nclx_color_profile(decoded,&raw).code!=heif_error_Ok||!raw)
        heif_image_handle_get_nclx_color_profile(handle,&raw);
    const bool nclx=raw!=nullptr;
    if(nclx){raster.primaries=raw->color_primaries;raster.transfer=raw->transfer_characteristics;heif_nclx_color_profile_free(raw);}
    const bool decodedIcc=heif_image_get_raw_color_profile_size(decoded)!=0;
    const auto iccSize=decodedIcc?heif_image_get_raw_color_profile_size(decoded):heif_image_handle_get_raw_color_profile_size(handle);
    check(iccSize<=16u*1024*1024,"HEIC ICC exceeds limit");
    if(iccSize){raster.icc.resize(iccSize);checked(decodedIcc?heif_image_get_raw_color_profile(decoded,raster.icc.data()):heif_image_handle_get_raw_color_profile(handle,raster.icc.data()));if(!nclx){raster.primaries=2;raster.transfer=2;}}
    return raster;
}
}

bool is_heif_input(const uint8_t* data,size_t size){
    if(!data||size<16||std::memcmp(data+4,"ftyp",4))return false;
    const size_t box=size_t(data[0])*16777216+size_t(data[1])*65536+size_t(data[2])*256+data[3];if(box<16||box>size)return false;
    for(size_t at=8;at+4<=box;at+=4){if(at==12)continue;for(const auto* brand:{"heic","heix","hevc","hevx","heim","heis","hevm","hevs"})if(!std::memcmp(data+at,brand,4))return true;}
    return false;
}
float apple_gainmap_headroom(const std::vector<uint8_t>& bytes){
    if(bytes.size()<8)return 0;
    auto u16=[&](size_t p,bool be)->uint32_t{if(p>bytes.size()-2)throw std::runtime_error("Truncated Apple EXIF");return be?uint32_t(bytes[p])*256+bytes[p+1]:uint32_t(bytes[p+1])*256+bytes[p];};
    auto u32=[&](size_t p,bool be)->uint32_t{if(p>bytes.size()-4)throw std::runtime_error("Truncated Apple EXIF");return be?u16(p,be)*65536+u16(p+2,be):u16(p+2,be)*65536+u16(p,be);};
    const bool be=bytes[0]=='M';if(!be&&bytes[0]!='I')return 0;
    size_t at=u32(4,be);bool maker=false,have=false;size_t origin=0;bool order=be;double tag33=0,tag48=0;
    for(unsigned depth=0;at&&depth<4;++depth){const size_t count=u16(at,order);check(count<=512&&at+2<=bytes.size()&&count<=(bytes.size()-at-2)/12,"Invalid Apple EXIF directory");bool jumped=false;
        for(size_t i=0;i<count;++i){const size_t p=at+2+i*12;const auto tag=u16(p,order),type=u16(p+2,order),value=u32(p+8,order);
            if(!maker&&tag==0x8769){at=value;jumped=true;break;}
            if(!maker&&tag==0x927c&&value<=bytes.size()-14&&!std::memcmp(bytes.data()+value,"Apple iOS\0",10)){origin=value;order=bytes[value+12]=='M';at=value+14;maker=true;jumped=true;break;}
            if(maker&&(tag==33||tag==48)&&(type==5||type==10)&&u32(p+4,order)==1){const size_t pos=origin+value;const auto denominator=u32(pos+4,order);if(!denominator)continue;const double numerator=type==10?int32_t(u32(pos,order)):double(u32(pos,order));if(tag==33)tag33=numerator/denominator;else tag48=numerator/denominator;have=true;}
        }
        if(maker&&have)break;if(!jumped)at=u32(at+2+count*12,order);
    }
    if(!have)return 0;
    const double stops=tag33<1?(tag48<=.01?-20*tag48+1.8:-.101*tag48+1.601):(tag48<=.01?-70*tag48+3:-.303*tag48+2.303);
    const double headroom=std::pow(2.,std::max(0.,stops));check(std::isfinite(headroom)&&headroom<=64,"Invalid Apple HDR headroom");return float(headroom);
}
jxrc::ImageInfo inspect_heif(const uint8_t* data,size_t size){
    Input input(data,size);const auto* base=input.base.get();jxrc::ImageInfo info;info.format="HEIC";info.width=heif_image_handle_get_width(base);info.height=heif_image_handle_get_height(base);info.bits=heif_image_handle_get_luma_bits_per_pixel(base);
    info.gainmap=bool(input.gain)||proxdr(data,size).jpeg;heif_color_profile_nclx* color=nullptr;
    if(heif_image_handle_get_nclx_color_profile(base,&color).code==heif_error_Ok&&color){info.transfer=color->transfer_characteristics==16?"PQ":color->transfer_characteristics==18?"HLG":color->transfer_characteristics==8?"Linear":"SDR";info.primaries=color->color_primaries==9?"BT.2020":color->color_primaries==12?"Display P3":"BT.709";heif_nclx_color_profile_free(color);}else{info.transfer="SDR";info.primaries="ICC";}
    info.hdr=info.gainmap||info.transfer=="PQ"||info.transfer=="HLG"||info.transfer=="Linear";if(info.gainmap)info.transfer="Gain map";return info;
}
HeifSource decode_heif(const uint8_t* data,size_t size,unsigned edge){
    Input input(data,size);HeifSource source;source.base=decode(input.base.get(),false,edge);source.base.exif=input.exif;normalize_exif(source.base.exif);
    const auto oppo=proxdr(data,size);
    if(oppo.jpeg){
        ultrahdr::JpegDecoderHelper decoder;check(decoder.parseImage(oppo.jpeg,oppo.size).error_code==UHDR_CODEC_OK,"Invalid ProXDR gain-map JPEG");
        check(fits_working_image(decoder.getDecompressedImageWidth(),decoder.getDecompressedImageHeight()),"ProXDR gain map exceeds limit");
        check(decoder.decompressImage(oppo.jpeg,oppo.size,ultrahdr::DECODE_STREAM).error_code==UHDR_CODEC_OK,"ProXDR gain-map decode failed");const auto image=decoder.getDecompressedImage();
        check(image.fmt==UHDR_IMG_FMT_8bppYCbCr400||image.fmt==UHDR_IMG_FMT_24bppRGB888||image.fmt==UHDR_IMG_FMT_32bppRGBA8888,"Unsupported ProXDR map layout");source.gain.width=image.w;source.gain.height=image.h;source.gain.bits=8;source.gain.pixels.resize(size_t(image.w)*image.h*4);const unsigned channels=image.fmt==UHDR_IMG_FMT_8bppYCbCr400?1:image.fmt==UHDR_IMG_FMT_24bppRGB888?3:4;
        for(unsigned y=0;y<image.h;++y)for(unsigned x=0;x<image.w;++x){for(unsigned c=0;c<3;++c)source.gain.pixels[(size_t(y)*image.w+x)*4+c]=static_cast<const uint8_t*>(image.planes[0])[size_t(y)*image.stride[0]*channels+x*channels+(channels==1?0:c)];source.gain.pixels[(size_t(y)*image.w+x)*4+3]=255;}
        source.max_boost=oppo.boost;source.display_ratio=oppo.display;source.encoded_gain=oppo.jpeg;source.encoded_gain_size=oppo.size;
    }else if(input.gain){
        source.max_boost=source.display_ratio=apple_gainmap_headroom(input.exif);check(source.max_boost>1,"Missing Apple gain-map headroom; refusing to discard the map");
        auto map=decode(input.gain.get(),true,edge);source.gain.width=map.width;source.gain.height=map.height;source.gain.bits=8;source.gain.pixels.resize(size_t(map.width)*map.height*4);source.gain_codes.resize(size_t(map.width)*map.height*3);
        for(size_t i=0;i<size_t(map.width)*map.height;++i){double v;if(map.bits>8){uint16_t code;std::memcpy(&code,map.pixels.data()+i*8,2);v=code/65535.;}else v=map.pixels[i*4]/255.;const double linear=v<.081?v/4.5:std::pow((v+.099)/1.099,1/.45);const float code=float(std::log1p((source.max_boost-1)*linear)/std::log(source.max_boost));const auto value=uint8_t(std::lround(255*code));for(unsigned c=0;c<3;++c){source.gain.pixels[i*4+c]=value;source.gain_codes[i*3+c]=code;}source.gain.pixels[i*4+3]=255;}
    }
    return source;
}
