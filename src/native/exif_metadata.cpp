#include "exif_metadata.h"
#include <exiv2/exiv2.hpp>
#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <stdexcept>

namespace {
bool photographic(const Exiv2::Exifdatum& tag) {
    const auto group=tag.groupName(),name=tag.tagName();
    if(group=="GPSInfo")return tag.tag()<=0x1f;
    if(group=="Image") {
        static const std::set<std::string> names={"Make","Model","Software","Artist","Copyright","ImageDescription","DateTime","HostComputer","DocumentName","XPTitle","XPComment","XPAuthor","XPKeywords","XPSubject"};
        return names.count(name);
    }
    if(group!="Photo")return false;
    static const std::set<std::string> names={
        "ExposureTime","FNumber","ExposureProgram","SpectralSensitivity","ISOSpeedRatings","SensitivityType","StandardOutputSensitivity","RecommendedExposureIndex","ISOSpeed","ISOSpeedLatitudeyyy","ISOSpeedLatitudezzz","ExifVersion",
        "DateTimeOriginal","DateTimeDigitized","OffsetTime","OffsetTimeOriginal","OffsetTimeDigitized","SubSecTime","SubSecTimeOriginal","SubSecTimeDigitized","ShutterSpeedValue","ApertureValue","BrightnessValue","ExposureBiasValue","MaxApertureValue","SubjectDistance","MeteringMode","LightSource","Flash","FocalLength","UserComment","RelatedSoundFile","FlashEnergy","ExposureIndex","SensingMethod","FileSource","SceneType","CustomRendered","ExposureMode","WhiteBalance","DigitalZoomRatio","FocalLengthIn35mmFilm","SceneCaptureType","GainControl","Contrast","Saturation","Sharpness","SubjectDistanceRange","ImageUniqueID","CameraOwnerName","BodySerialNumber","LensSpecification","LensMake","LensModel","LensSerialNumber","CompositeImage","SourceImageNumberOfCompositeImage","SourceExposureTimesOfCompositeImage"};
    return names.count(name);
}
std::vector<uint8_t> encode(const Exiv2::ExifData& source,unsigned w,unsigned h,unsigned orientation) {
    Exiv2::ExifData photo;size_t budget=48000;
    for(const auto& tag:source)if(photographic(tag)) {
        const auto length=tag.size();
        if(!length||length>budget||photo.findKey(Exiv2::ExifKey(tag.key()))!=photo.end())continue;
        photo.add(tag);budget-=length;
    }
    if(photo.empty())return {};
    photo["Exif.Image.Orientation"]=uint16_t(orientation);
    photo["Exif.Image.ImageWidth"]=uint32_t(w);photo["Exif.Image.ImageLength"]=uint32_t(h);
    photo["Exif.Photo.PixelXDimension"]=uint32_t(w);photo["Exif.Photo.PixelYDimension"]=uint32_t(h);
    photo["Exif.Photo.ColorSpace"]=uint16_t(65535); // The encoder supplies ICC/CICP.
    Exiv2::Blob result;Exiv2::ExifParser::encode(result,Exiv2::littleEndian,photo);
    if(result.size()>65527)throw std::runtime_error("Photographic EXIF exceeds JPEG metadata limit");
    return result;
}
// JPEG XR uses a TIFF-style directory with a different magic number. Read only
// photographic fields and linked EXIF/GPS IFDs, without copying image data.
Exiv2::ExifData jxr_exif(const uint8_t* data,size_t size) {
    Exiv2::ExifData exif;
    if(size<8||std::memcmp(data,"II\xbc\x01",4))return exif;
    auto u16=[&](size_t at){return uint32_t(data[at])|uint32_t(data[at+1])<<8;};
    auto u32=[&](size_t at){return u16(at)|u16(at+2)<<16;};
    std::set<size_t> visited;size_t budget=1024*1024;
    auto directory=[&](auto&& self,size_t at,const char* group,unsigned depth)->void {
        if(depth>2||at>size-2||!visited.insert(at).second)return;
        const auto count=u16(at);if(count>(size-at-2)/12)return;
        for(size_t i=0;i<count;++i) {
            const size_t entry=at+2+i*12;const auto tag=u16(entry),type=u16(entry+2),n=u32(entry+4);
            if((tag==0x8769||tag==0x8825)&&type==4&&n==1&&depth==0){self(self,u32(entry+8),tag==0x8769?"Photo":"GPSInfo",depth+1);continue;}
            if(type<1||type>12)continue;
            static const size_t widths[]={0,1,1,2,4,8,1,1,2,4,8,4,8};
            if(n>budget/widths[type])continue;
            const size_t length=size_t(n)*widths[type],value=length<=4?entry+8:u32(entry+8);
            if(value>size||length>size-value)continue;
            try {
                Exiv2::ExifKey key(uint16_t(tag),group);Exiv2::Exifdatum candidate(key);
                if(!photographic(candidate))continue;
                auto field=Exiv2::Value::create(static_cast<Exiv2::TypeId>(type));
                if(field->read(data+value,length,Exiv2::littleEndian)!=0)continue;
                exif.add(key,field.get());budget-=length;
            }catch(const Exiv2::Error&){}
        }
    };
    directory(directory,u32(4),"Image",0);return exif;
}
}
std::vector<uint8_t> portable_source_exif(const uint8_t* data,size_t size,unsigned w,unsigned h,unsigned orientation) {
    try {
        if(size>=4&&!std::memcmp(data,"II\xbc\x01",4))return encode(jxr_exif(data,size),w,h,orientation);
        auto image=Exiv2::ImageFactory::open(data,size);
        if(!image)return {};
        image->readMetadata();return encode(image->exifData(),w,h,orientation);
    }catch(const Exiv2::Error&){return {};}
}
void prepare_export_exif(std::vector<uint8_t>& bytes,unsigned w,unsigned h) {
    if(bytes.empty())return;
    Exiv2::ExifData source;
    try {Exiv2::ExifParser::decode(source,bytes.data(),bytes.size());bytes=encode(source,w,h,1);}
    catch(const Exiv2::Error&){bytes.clear();}
}
