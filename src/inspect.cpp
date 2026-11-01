#include "jxrc/converter.hpp"
#include "codec_api.h"
#include "input_format.h"
#include "regular_decode.h"
#include "ultrahdr/jpegdecoderhelper.h"
#include <avif/avif.h>
#include <algorithm>
#include <jxl/decode_cxx.h>
#include <libraw/libraw.h>
#include <fstream>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace jxrc {
ImageInfo inspect(const std::filesystem::path& input) {
    return inspect(input,{});
}
ImageInfo inspect(const std::filesystem::path& input,const std::function<bool()>& cancelled) {
    if(cancelled&&cancelled())throw std::runtime_error("Inspection cancelled");
    std::ifstream stream(input,std::ios::binary|std::ios::ate);
    if(!stream)throw std::runtime_error("Cannot open image");
    auto length=stream.tellg();if(length<=0||length>512LL*1024*1024)throw std::runtime_error("Input exceeds inspection limit");
    std::vector<uint8_t> bytes(static_cast<size_t>(length));stream.seekg(0);
    for(size_t offset=0;offset<bytes.size();) {
        if(cancelled&&cancelled())throw std::runtime_error("Inspection cancelled");
        const size_t count=cancelled?std::min<size_t>(256*1024,bytes.size()-offset):bytes.size()-offset;
        if(!stream.read(reinterpret_cast<char*>(bytes.data()+offset),count))throw std::runtime_error("Cannot read image");offset+=count;
    }
    stream.close();
    if(cancelled&&cancelled())throw std::runtime_error("Inspection cancelled");
    return inspect_bytes(bytes);
}
ImageInfo inspect_bytes(const std::vector<uint8_t>& bytes) {
    ImageInfo out;
    if(bytes.size()>2&&bytes[0]==255&&bytes[1]==216) {
        ultrahdr::JpegDecoderHelper jpeg;
        if(jpeg.parseImage(bytes.data(),bytes.size()).error_code!=UHDR_CODEC_OK)throw std::runtime_error("Invalid JPEG");
        out.width=jpeg.getDecompressedImageWidth();out.height=jpeg.getDecompressedImageHeight();out.bits=8;out.format="JPEG";
        out.gainmap=is_uhdr_image(const_cast<uint8_t*>(bytes.data()),static_cast<int>(bytes.size()))==1;out.transfer="sRGB";
        if(out.gainmap)out.transfer="Gain map";
        out.hdr=out.gainmap;
        auto icc=source_jpeg_icc(bytes.data(),bytes.size());out.primaries=icc.empty()?"sRGB":"ICC";
        int orientation=source_orientation(bytes.data(),bytes.size(),0,nullptr);if(orientation>=5)std::swap(out.width,out.height);
    } else if(is_avif_input(bytes.data(),bytes.size())) {
        std::unique_ptr<avifDecoder,decltype(&avifDecoderDestroy)> dec(avifDecoderCreate(),avifDecoderDestroy);
        if(!dec)throw std::runtime_error("Cannot allocate AVIF inspector");
        dec->imageSizeLimit=80000000;
        if(avifDecoderSetIOMemory(dec.get(),bytes.data(),bytes.size())!=AVIF_RESULT_OK||avifDecoderParse(dec.get())!=AVIF_RESULT_OK)throw std::runtime_error("Unsupported AVIF/HEIF header");
        auto* image=dec->image;out.width=image->width;out.height=image->height;out.bits=image->depth;out.format="AVIF";
        out.gainmap=image->gainMap!=nullptr;
        out.transfer=out.gainmap?"Gain map":image->transferCharacteristics==16?"PQ":image->transferCharacteristics==18?"HLG":image->transferCharacteristics==8?"Linear":"SDR";
        out.hdr=out.gainmap||image->transferCharacteristics==16||image->transferCharacteristics==18;
        out.primaries=image->icc.size?"ICC":image->colorPrimaries==9?"BT.2020":image->colorPrimaries==12?"Display P3":"BT.709";
        int orientation=source_orientation(bytes.data(),bytes.size(),1,nullptr);if(orientation>=5)std::swap(out.width,out.height);
    } else if(bytes.size()>=2&&((bytes[0]==255&&bytes[1]==10)||(bytes.size()>=12&&!std::memcmp(bytes.data(),"\0\0\0\x0cJXL \r\n\x87\n",12)))) {
        auto dec=JxlDecoderMake(nullptr);
        if(!dec||JxlDecoderSubscribeEvents(dec.get(),JXL_DEC_BASIC_INFO|JXL_DEC_COLOR_ENCODING)!=JXL_DEC_SUCCESS)throw std::runtime_error("Cannot allocate JXL inspector");
        JxlDecoderSetInput(dec.get(),bytes.data(),bytes.size());JxlDecoderCloseInput(dec.get());
        float intensity=0;
        for(;;){auto status=JxlDecoderProcessInput(dec.get());
            if(status==JXL_DEC_BASIC_INFO){JxlBasicInfo info{};if(JxlDecoderGetBasicInfo(dec.get(),&info)!=JXL_DEC_SUCCESS)break;out.width=info.xsize;out.height=info.ysize;out.bits=info.bits_per_sample;out.format="JXL";intensity=info.intensity_target;out.hdr=info.intensity_target>=400.f;}
            else if(status==JXL_DEC_COLOR_ENCODING){JxlColorEncoding color{};
                if(JxlDecoderGetColorAsEncodedProfile(dec.get(),JXL_COLOR_PROFILE_TARGET_ORIGINAL,&color)==JXL_DEC_SUCCESS){out.transfer=color.transfer_function==JXL_TRANSFER_FUNCTION_PQ?"PQ":color.transfer_function==JXL_TRANSFER_FUNCTION_HLG?"HLG":color.transfer_function==JXL_TRANSFER_FUNCTION_LINEAR?"Linear":"SDR";out.hdr=out.hdr||(intensity==80.f && color.transfer_function==JXL_TRANSFER_FUNCTION_LINEAR && color.primaries==JXL_PRIMARIES_SRGB)||color.transfer_function==JXL_TRANSFER_FUNCTION_PQ||color.transfer_function==JXL_TRANSFER_FUNCTION_HLG;out.primaries=color.primaries==JXL_PRIMARIES_2100?"BT.2020":color.primaries==JXL_PRIMARIES_P3?"Display P3":"BT.709";}else{out.transfer="ICC";out.primaries="ICC";}break;
            }else if(status==JXL_DEC_ERROR||status==JXL_DEC_NEED_MORE_INPUT||status==JXL_DEC_SUCCESS)break;
        }
    } else if(bytes.size()>=4&&bytes[0]=='I'&&bytes[1]=='I'&&bytes[2]==0xbc&&bytes[3]==1) {
        out=inspect_jxr(bytes.data(),bytes.size());
    } else if(is_regular_image(bytes.data(),bytes.size())) {
        const auto raster=decode_regular(bytes.data(),bytes.size());
        out.width=raster.width;out.height=raster.height;out.bits=raster.bits;out.format=raster.format;
        out.hdr=raster.hdr();
        out.transfer=raster.hdr()?(raster.transfer==16?"PQ":"HLG"):raster.icc.empty()?"sRGB":"ICC";
        out.primaries=raster.hdr()?(raster.primaries==9?"BT.2020":raster.primaries==12?"Display P3":"BT.709"):raster.icc.empty()?"BT.709":"ICC";
        if(raster.orientation>=5)std::swap(out.width,out.height);
    } else {
        auto storage=std::make_unique<LibRaw>();
        auto& raw=*storage;
        if(raw.open_buffer(bytes.data(),bytes.size())!=LIBRAW_SUCCESS)throw std::runtime_error("Image header inspection unavailable");
        out.width=raw.imgdata.sizes.width;out.height=raw.imgdata.sizes.height;out.raw=true;out.format="RAW";out.transfer="Sensor";out.primaries="Camera";
        if(raw.imgdata.sizes.flip==5||raw.imgdata.sizes.flip==6)std::swap(out.width,out.height);
        out.bits=raw.get_internal_data_pointer()->unpacker_data.tiff_bps;
    }
    if(!out.width||!out.height||uint64_t(out.width)*out.height>80000000)throw std::runtime_error("Invalid or oversized image dimensions");
    return out;
}
}
