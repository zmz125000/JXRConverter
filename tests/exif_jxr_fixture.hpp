#pragma once
#include <JXRGlue.h>
#include <JXRMeta.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
// Real JPEG XR pixels plus photographic and GPS IFDs, written by jxrlib.
static void write_jxr_exif_fixture(const std::filesystem::path& path,const std::vector<uint8_t>& tiff) {
    auto require=[](ERR result){if(Failed(result))throw std::runtime_error("JXR EXIF fixture failed");};
    std::vector<uint8_t> memory(1024*1024);WMPStream* stream=nullptr;PKImageEncode* encoder=nullptr;
    require(CreateWS_Memory(&stream,memory.data(),memory.size()));require(PKImageEncode_Create_WMP(&encoder));
    struct Cleanup {PKImageEncode*& encoder;~Cleanup(){if(encoder)encoder->Release(&encoder);}} cleanup{encoder};
    CWMIStrCodecParam parameters{};parameters.cfColorFormat=YUV_444;parameters.bdBitDepth=BD_LONG;parameters.bfBitstreamFormat=SPATIAL;parameters.sbSubband=SB_ALL;parameters.uiDefaultQPIndex=1;
    require(encoder->Initialize(encoder,stream,&parameters,sizeof(parameters)));
    require(encoder->SetPixelFormat(encoder,GUID_PKPixelFormat24bppRGB));require(encoder->SetSize(encoder,64,48));require(encoder->SetResolution(encoder,96,96));
    DESCRIPTIVEMETADATA descriptive{};char make[]="JXRC",model[]="Test Camera";
    descriptive.pvarCameraMake.vt=DPKVT_LPSTR;descriptive.pvarCameraMake.VT.pszVal=make;
    descriptive.pvarCameraModel.vt=DPKVT_LPSTR;descriptive.pvarCameraModel.VT.pszVal=model;
    require(encoder->SetDescriptiveMetadata(encoder,&descriptive));
    auto u16=[&](size_t at){return uint32_t(tiff[at])|uint32_t(tiff[at+1])<<8;};auto u32=[&](size_t at){return u16(at)|u16(at+2)<<16;};
    const size_t root=u32(4);
    for(unsigned i=0;i<u16(root);++i) {
        const size_t at=root+2+i*12;const auto tag=u16(at);if(tag!=0x8769&&tag!=0x8825)continue;
        const U32 offset=u32(at+8);U32 length=0,destination=0;require(BufferCalcIFDSize(tiff.data(),tiff.size(),offset,WMP_INTEL_ENDIAN,&length));
        std::vector<uint8_t> ifd(length);require(BufferCopyIFD(tiff.data(),U32(tiff.size()),offset,WMP_INTEL_ENDIAN,ifd.data(),length,&destination));
        if(tag==0x8769)require(PKImageEncode_SetEXIFMetadata_WMP(encoder,ifd.data(),length));
        else require(PKImageEncode_SetGPSInfoMetadata_WMP(encoder,ifd.data(),length));
    }
    std::vector<uint8_t> rgb(64*48*3);for(size_t i=0;i<rgb.size();++i)rgb[i]=uint8_t(i%251);
    require(encoder->WritePixels(encoder,48,rgb.data(),64*3));const size_t length=size_t(encoder->WMP.nOffImage)+size_t(encoder->WMP.nCbImage);
    std::ofstream output(path,std::ios::binary);output.write(reinterpret_cast<const char*>(memory.data()),length);
    if(!output)throw std::runtime_error("Cannot save JXR fixture");
}
