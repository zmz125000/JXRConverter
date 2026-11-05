#include "gain_map.h"
#include <avif/internal.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
void gain_map_f32_tests(const std::filesystem::path&);

namespace {
using Image=std::unique_ptr<avifImage,decltype(&avifImageDestroy)>;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void near(float a,float b,float tolerance,const char* message){check(std::abs(a-b)<=tolerance*std::max(1.f,std::abs(b)),message);}
Image rgb(unsigned w,unsigned h,unsigned depth,unsigned cp,unsigned tc,const std::vector<float>& pixels) {
    Image image(avifImageCreate(w,h,depth,AVIF_PIXEL_FORMAT_YUV444),avifImageDestroy);
    image->colorPrimaries=avifColorPrimaries(cp);image->transferCharacteristics=avifTransferCharacteristics(tc);
    image->matrixCoefficients=AVIF_MATRIX_COEFFICIENTS_IDENTITY;image->yuvRange=AVIF_RANGE_FULL;
    const unsigned maximum=(1u<<depth)-1;std::vector<uint8_t> codes(pixels.size()*(depth==8?1:2));
    for(size_t i=0;i<pixels.size();++i){const uint16_t v=uint16_t(std::lround(pixels[i]*maximum));if(depth==8)codes[i]=uint8_t(v);else std::memcpy(codes.data()+i*2,&v,2);}
    avifRGBImage source;avifRGBImageSetDefaults(&source,image.get());source.format=AVIF_RGB_FORMAT_RGBA;
    source.pixels=codes.data();source.rowBytes=w*4*(depth==8?1:2);
    check(avifImageRGBToYUV(image.get(),&source)==AVIF_RESULT_OK,"Gain-map fixture packing");return image;
}
void metadata(avifImage* image,Image gain) {
    image->gainMap=avifGainMapCreate();auto* gm=image->gainMap;gm->image=gain.release();gm->useBaseColorSpace=AVIF_TRUE;
    gm->baseHdrHeadroom={0,1};gm->alternateHdrHeadroom={4,1};gm->altColorPrimaries=image->colorPrimaries;
    gm->altTransferCharacteristics=AVIF_TRANSFER_CHARACTERISTICS_PQ;gm->altMatrixCoefficients=AVIF_MATRIX_COEFFICIENTS_BT709;
    gm->altDepth=10;gm->altPlaneCount=3;gm->altYUVRange=AVIF_RANGE_FULL;
    for(unsigned c=0;c<3;++c){gm->gainMapMin[c]={-1,1};gm->gainMapMax[c]={3,1};gm->gainMapGamma[c]={c+1,1};gm->baseOffset[c]={1,64};gm->alternateOffset[c]={1,128};}
}
float quant(float v,unsigned depth){const float maximum=float((1u<<depth)-1);return std::round(v*maximum)/maximum;}
std::vector<uint8_t> profile(const std::filesystem::path& dir,unsigned cp) {
    const float pixels[4]={.2f,.3f,.4f,1};const auto path=dir/("gain-profile-"+std::to_string(cp)+".jpg");
    check(encode_still_export_f32(pixels,1,1,cp==9?2:cp==12?1:0,0,path.string().c_str(),StillEncodeQuality(0))==0,"Gain ICC fixture encode");
    std::ifstream file(path,std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
    return source_jpeg_icc(bytes.data(),bytes.size());
}
}

void gain_map_f32_tests(const std::filesystem::path& dir) {
    const std::vector<float> base={.1f,.3f,.6f,.7f, .2f,.4f,.7f,.8f, .3f,.5f,.8f,.9f};
    const std::vector<float> gain={.1f,.3f,.7f,1, .2f,.4f,.8f,1, .3f,.5f,.9f,1};
    for(unsigned depth:{8u,10u,12u})for(unsigned cp:{1u,9u,12u})for(bool useBase:{false,true}) {
        auto image=rgb(3,1,depth,cp,13,base);metadata(image.get(),rgb(3,1,depth,2,8,gain));
        auto* gm=image->gainMap;gm->useBaseColorSpace=useBase;gm->altColorPrimaries=AVIF_COLOR_PRIMARIES_SMPTE432;
        // The gain image's transfer label must never gamma-decode gain codes.
        gm->image->transferCharacteristics=AVIF_TRANSFER_CHARACTERISTICS_PQ;
        std::vector<float> codes,hdr,sdr;unsigned w=0,h=0;uhdr_color_gamut_t gamut;
        check(decode_gain_map_f32(gm,codes,w,h)&&w==3&&h==1,"Gain-only F32 decode");
        for(unsigned i=0;i<3;++i)for(unsigned c=0;c<3;++c)near(codes[i*3+c],quant(gain[i*4+c],depth),1e-7f,"Gain-only decoder applied a color transfer");
        check(reconstruct_gain_map_f32(image.get(),hdr,gamut,&sdr)&&gamut==UHDR_CG_DISPLAY_P3,"General gain reconstruction");
        const auto eotf=avifTransferCharacteristicsGetGammaToLinearFunction(AVIF_TRANSFER_CHARACTERISTICS_SRGB);
        double input[3][3],output[3][3];const auto math=useBase?avifColorPrimaries(cp):avifColorPrimaries(AVIF_COLOR_PRIMARIES_SMPTE432);
        check(avifColorPrimariesComputeRGBToRGBMatrix(avifColorPrimaries(cp),math,input)&&avifColorPrimariesComputeRGBToRGBMatrix(math,AVIF_COLOR_PRIMARIES_SMPTE432,output),"Reference color matrices");
        for(unsigned i=0;i<3;++i) {
            float linear[3],work[3]{},expected[3]{},expectedSdr[3]{};
            for(unsigned c=0;c<3;++c)linear[c]=eotf(quant(base[i*4+c],depth));
            for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)work[r]+=float(input[r][c])*linear[c];
            float boosted[3];for(unsigned c=0;c<3;++c)boosted[c]=(work[c]+1.f/64)*std::exp2(-1+4*std::pow(codes[i*3+c],1.f/(c+1)))-1.f/128;
            for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c){expected[r]+=float(output[r][c])*boosted[c];expectedSdr[r]+=float(output[r][c])*work[c];}
            for(unsigned c=0;c<3;++c){near(hdr[i*4+c],expected[c],3e-5f,"F32 gain gamma/offset/gamut math");near(sdr[i*4+c],expectedSdr[c],3e-5f,"Paired SDR source changed");}
            near(hdr[i*4+3],quant(base[i*4+3],depth),1e-7f,"Gain reconstruction changed alpha");
        }
        gm->gainMapGamma[0].n=0;check(!reconstruct_gain_map_f32(image.get(),hdr,gamut),"Invalid gain gamma accepted");
    }
    // Bilinear interpolation is in encoded float codes, before gain gamma.
    auto scaled=rgb(3,1,8,1,8,{.25f,.25f,.25f,1,.25f,.25f,.25f,1,.25f,.25f,.25f,1});
    metadata(scaled.get(),rgb(2,1,8,2,8,{0,0,0,1,1,1,1,1}));
    for(unsigned c=0;c<3;++c){scaled->gainMap->gainMapMin[c]={0,1};scaled->gainMap->gainMapMax[c]={4,1};scaled->gainMap->gainMapGamma[c]={2,1};scaled->gainMap->baseOffset[c]={0,1};scaled->gainMap->alternateOffset[c]={0,1};}
    std::vector<float> hdr,sdr;uhdr_color_gamut_t gamut;
    check(reconstruct_gain_map_f32(scaled.get(),hdr,gamut,&sdr),"F32 gain resampling");
    near(hdr[4],quant(.25f,8)*std::exp2(4*std::sqrt(.5f)),1e-5f,"Gain resampling/gamma order or intermediate quantization");
    // F32 must retain headroom beyond both PQ and half-float limits.
    for(unsigned c=0;c<3;++c)scaled->gainMap->gainMapMax[c]={20,1};
    scaled->gainMap->alternateHdrHeadroom={20,1};
    check(reconstruct_gain_map_f32(scaled.get(),hdr,gamut)&&hdr[8]>200000,"F32 gain reconstruction clipped extended values");
    // An HDR base uses zero gain for HDR and a negative gain weight for SDR.
    scaled->gainMap->baseHdrHeadroom={20,1};scaled->gainMap->alternateHdrHeadroom={0,1};
    check(reconstruct_gain_map_f32(scaled.get(),hdr,gamut,&sdr),"HDR-base reconstruction");
    near(hdr[8],quant(.25f,8),1e-5f,"HDR base changed at full headroom");near(sdr[8],quant(.25f,8)/1048576.f,1e-7f,"Inverse gain from HDR base");
    // Equivalent ICC/CICP bases reconstruct to the same rendition. Unspecified
    // base CICP is legitimate when ICC supplies the transfer and primaries.
    auto tagged=rgb(3,1,10,12,13,base);metadata(tagged.get(),rgb(3,1,10,2,8,gain));
    std::vector<float> reference;check(reconstruct_gain_map_f32(tagged.get(),reference,gamut),"CICP reconstruction reference");
    const auto p3=profile(dir,12);check(!p3.empty()&&avifImageSetProfileICC(tagged.get(),p3.data(),p3.size())==AVIF_RESULT_OK,"Gain ICC fixture");
    tagged->colorPrimaries=AVIF_COLOR_PRIMARIES_UNSPECIFIED;tagged->transferCharacteristics=AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
    check(reconstruct_gain_map_f32(tagged.get(),hdr,gamut),"ICC gain reconstruction with unspecified CICP");
    for(size_t i=0;i<hdr.size();++i)near(hdr[i],reference[i],.001f,"ICC gain reconstruction disagrees with CICP");
    const uint8_t invalid[]={1,2,3,4};check(avifImageSetProfileICC(tagged.get(),invalid,sizeof(invalid))==AVIF_RESULT_OK,"Malformed ICC fixture allocation");
    check(!reconstruct_gain_map_f32(tagged.get(),hdr,gamut),"Malformed ICC silently treated as SDR");
    auto alternate=rgb(3,1,10,1,13,base);metadata(alternate.get(),rgb(3,1,10,2,8,gain));
    alternate->gainMap->useBaseColorSpace=AVIF_FALSE;alternate->gainMap->altColorPrimaries=AVIF_COLOR_PRIMARIES_SMPTE432;
    check(reconstruct_gain_map_f32(alternate.get(),reference,gamut),"Alternate CICP reference");
    check(avifRWDataSet(&alternate->gainMap->altICC,p3.data(),p3.size())==AVIF_RESULT_OK,"Alternate ICC fixture");
    alternate->gainMap->altColorPrimaries=AVIF_COLOR_PRIMARIES_UNSPECIFIED;
    check(reconstruct_gain_map_f32(alternate.get(),hdr,gamut)&&gamut==UHDR_CG_BT_2100,"Alternate ICC math gamut");
    double matrix[3][3];check(avifColorPrimariesComputeRGBToRGBMatrix(AVIF_COLOR_PRIMARIES_SMPTE432,AVIF_COLOR_PRIMARIES_BT2020,matrix),"Alternate ICC comparison matrix");
    for(size_t i=0;i<hdr.size();i+=4)for(unsigned r=0;r<3;++r){float expected=0;for(unsigned c=0;c<3;++c)expected+=float(matrix[r][c])*reference[i+c];near(hdr[i+r],expected,.001f,"Alternate ICC reconstruction");}
    // Monochrome gain maps remain data, with the single code replicated to RGB.
    Image mono(avifImageCreate(2,1,8,AVIF_PIXEL_FORMAT_YUV400),avifImageDestroy);mono->yuvRange=AVIF_RANGE_FULL;mono->matrixCoefficients=AVIF_MATRIX_COEFFICIENTS_BT709;
    check(avifImageAllocatePlanes(mono.get(),AVIF_PLANES_YUV)==AVIF_RESULT_OK,"Monochrome map allocation");mono->yuvPlanes[0][0]=0;mono->yuvPlanes[0][1]=255;
    auto monoBase=rgb(3,1,8,1,8,{.25f,.25f,.25f,1,.25f,.25f,.25f,1,.25f,.25f,.25f,1});metadata(monoBase.get(),std::move(mono));
    std::vector<float> monoCodes;unsigned mw=0,mh=0;check(decode_gain_map_f32(monoBase->gainMap,monoCodes,mw,mh)&&monoCodes==std::vector<float>({0,0,0,1,1,1}),"Monochrome gain codes changed");
    // HLG applies the luma-based OOTF once, not a per-channel gamma shortcut.
    const std::vector<float> hlgCodes={.7f,.4f,.2f,1};auto hlg=rgb(1,1,12,9,18,hlgCodes);metadata(hlg.get(),rgb(1,1,8,2,8,{0,0,0,1}));
    for(unsigned c=0;c<3;++c){hlg->gainMap->gainMapMin[c]={0,1};hlg->gainMap->gainMapMax[c]={0,1};hlg->gainMap->baseOffset[c]={0,1};hlg->gainMap->alternateOffset[c]={0,1};}
    check(reconstruct_gain_map_f32(hlg.get(),hdr,gamut),"HLG base gain reconstruction");
    float scene[3];for(unsigned c=0;c<3;++c){const float v=quant(hlgCodes[c],12);scene[c]=v<=.5f?v*v/3.f:(std::exp((v-.55991073f)/.17883277f)+.28466892f)/12.f;}
    const float luminance=.2627f*scene[0]+.678f*scene[1]+.0593f*scene[2];
    for(unsigned c=0;c<3;++c)near(hdr[c],scene[c]*std::pow(luminance,.2f)*1000.f/203.f,2e-5f,"HLG gain reconstruction OOTF");
    const std::vector<float> floatingCodes={.1234567f,.2345678f,.3456789f};
    auto precise=rgb(1,1,8,1,8,{.25f,.25f,.25f,1});metadata(precise.get(),rgb(1,1,8,2,8,{0,0,0,1}));
    check(reconstruct_gain_map_f32(precise.get(),hdr,gamut,nullptr,&floatingCodes),"Unquantized HEIF gain codes");
    for(unsigned c=0;c<3;++c)near(hdr[c],(quant(.25f,8)+1.f/64)*std::exp2(-1+4*std::pow(floatingCodes[c],1.f/(c+1)))-1.f/128,1e-5f,"Floating gain codes were narrowed to integer samples");

    // Real encoder/decoder and multi-output session: preserve a deliberately
    // distinct SDR rendition rather than synthesizing it from the HDR pixels.
    std::vector<float> input(16*12*4),originalSdr(input.size());
    for(unsigned y=0;y<12;++y)for(unsigned x=0;x<16;++x){const size_t at=(size_t(y)*16+x)*4;
        input[at]=.1f+4*x/15.f;input[at+1]=.2f+2*y/11.f;input[at+2]=.5f;input[at+3]=1;
        originalSdr[at]=.05f+.4f*x/15;originalSdr[at+1]=.1f+.3f*y/11;originalSdr[at+2]=.2f;originalSdr[at+3]=1;
    }
    const auto source=dir/"f32-gain-source.avif";
    check(encode_avif_f32(16,12,input.data(),UHDR_CG_DISPLAY_P3,2,source.string().c_str(),StillEncodeQuality(0),originalSdr.data())==0,"Paired F32 gain fixture");
    auto read=[](const std::filesystem::path& path){std::ifstream f(path,std::ios::binary);return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),{});};
    const auto sourceBytes=read(source);unsigned w=0,h=0;int hasGain=0,transfer=0;
    check(decode_avif_to_linear_f32(sourceBytes.data(),sourceBytes.size(),hdr,w,h,&hasGain,&transfer,&gamut,nullptr,&sdr)==0&&hasGain&&w==16&&h==12,"Paired AVIF F32 decode");
    for(size_t i=0;i<sdr.size();++i){near(sdr[i],originalSdr[i],.001f,"Original SDR rendition was tone mapped again");near(hdr[i],input[i],.01f,"F32 gain fixture HDR reconstruction");}
    // Fast repair preserves gain metadata while changing lossy identity/GBR
    // packing for both the base and the gain image to normal YCbCr.
    jxrc::Options fast;fast.mode=jxrc::Mode::process;fast.target=jxrc::Target::avif;fast.quality.preset=jxrc::Preset::low;
    const auto repaired=dir/"fast-gain-repair.avif";jxrc::convert(source,repaired,fast);
    auto before=std::unique_ptr<avifDecoder,decltype(&avifDecoderDestroy)>(avifDecoderCreate(),avifDecoderDestroy);
    auto after=std::unique_ptr<avifDecoder,decltype(&avifDecoderDestroy)>(avifDecoderCreate(),avifDecoderDestroy);
    before->imageContentToDecode=after->imageContentToDecode=AVIF_IMAGE_CONTENT_ALL;
    check(avifDecoderSetIOFile(before.get(),source.string().c_str())==AVIF_RESULT_OK&&avifDecoderParse(before.get())==AVIF_RESULT_OK,"Fast repair reference parse");
    check(avifDecoderSetIOFile(after.get(),repaired.string().c_str())==AVIF_RESULT_OK&&avifDecoderParse(after.get())==AVIF_RESULT_OK,"Fast repair output parse");
    check(after->image->matrixCoefficients==AVIF_MATRIX_COEFFICIENTS_BT709&&after->image->gainMap->image->matrixCoefficients==AVIF_MATRIX_COEFFICIENTS_BT709,"Lossy base/gain identity packing was retained");
    check(avifSameGainMapMetadata(before->image->gainMap,after->image->gainMap),"Fast repair modified gain-map math");
    std::vector<uint8_t> storedBase;
    for(auto reconstruction:{jxrc::GainMapReconstruction::off,jxrc::GainMapReconstruction::automatic,jxrc::GainMapReconstruction::full_image}) {
        jxrc::Options policy;policy.mode=jxrc::Mode::process;policy.gain_map_reconstruction=reconstruction;policy.target=jxrc::Target::png;policy.quality.preset=jxrc::Preset::lossless;
        const auto png=dir/("gain-policy-"+std::to_string(int(reconstruction))+".png");jxrc::convert(source,png,policy);
        if(reconstruction==jxrc::GainMapReconstruction::off)storedBase=read(png);
        else if(reconstruction==jxrc::GainMapReconstruction::automatic)check(read(png)==storedBase,"Auto reconstructed the full image for a source target");
        else check(read(png)!=storedBase,"Full failed to reconstruct for an SDR output target");
        for(auto target:{jxrc::Target::jpeg,jxrc::Target::avif}) {
            policy.target=target;const auto output=dir/("gain-policy-"+std::to_string(int(reconstruction))+"-"+std::to_string(int(target)));
            jxrc::convert(source,output,policy);check(jxrc::inspect(output).gainmap==(reconstruction!=jxrc::GainMapReconstruction::full_image),"Source/Full gain-map routing");
        }
        policy.target=jxrc::Target::avif_pq;
        if(reconstruction!=jxrc::GainMapReconstruction::full_image) {
            bool rejected=false;try{jxrc::convert(source,dir/("forbidden-pq-"+std::to_string(int(reconstruction))),policy);}catch(const std::exception& e){rejected=std::string(e.what()).find("Full gain-map reconstruction")!=std::string::npos;}
            check(rejected,"Off/Auto silently reconstructed a full HDR gain-map source");
        }
    }
    // This control is exclusively for gain-map inputs. Ordinary HDR sources
    // still take their normal processing path with any of the three settings.
    const auto ordinaryHdr=dir/"ordinary-hdr.jxl";
    check(encode_jxl_f32(16,12,input.data(),UHDR_CG_DISPLAY_P3,true,ordinaryHdr.string().c_str(),StillEncodeQuality(0),false)==0,"Non-gain-map HDR fixture");
    std::vector<uint8_t> ordinaryReference;
    for(auto reconstruction:{jxrc::GainMapReconstruction::off,jxrc::GainMapReconstruction::automatic,jxrc::GainMapReconstruction::full_image}) {
        jxrc::Options policy;policy.mode=jxrc::Mode::process;policy.target=jxrc::Target::png_hdr;policy.gain_map_reconstruction=reconstruction;
        const auto output=dir/("ordinary-policy-"+std::to_string(int(reconstruction))+".png");jxrc::convert(ordinaryHdr,output,policy);
        if(ordinaryReference.empty())ordinaryReference=read(output);else check(read(output)==ordinaryReference,"Gain-map policy changed a non-gain-map HDR source");
    }
    jxrc::ConversionSession session(source);jxrc::Options options;options.mode=jxrc::Mode::process;options.gain_map_reconstruction=jxrc::GainMapReconstruction::full_image;options.quality.preset=jxrc::Preset::lossless;
    for(auto target:{jxrc::Target::jxl_linear,jxrc::Target::avif_gainmap,jxrc::Target::jxl_pq,jxrc::Target::png_hdr,jxrc::Target::png_hlg,jxrc::Target::jxl_hlg,jxrc::Target::avif_hlg}) {
        options.target=target;const auto shared=dir/("gain-shared-"+std::to_string(int(target))),independent=dir/("gain-independent-"+std::to_string(int(target)));
        session.convert(shared,options);jxrc::convert(source,independent,options);
        check(read(shared)==read(independent),"Gain-map multi-output F32 cache changed output");
        if(target==jxrc::Target::avif_gainmap) {
            const auto encoded=read(shared);std::vector<float> rendered,paired;
            check(decode_avif_to_linear_f32(encoded.data(),encoded.size(),rendered,w,h,&hasGain,&transfer,&gamut,nullptr,&paired)==0,"Regenerated gain-map decode");
            for(size_t i=0;i<paired.size();++i)near(paired[i],originalSdr[i],.002f,"Regenerated gain map discarded the original SDR base");
        }
    }
    const auto jpeg=dir/"f32-gain-source.jpg";
    check(encode_still_export_f32(input.data(),16,12,1,1,jpeg.string().c_str(),StillEncodeQuality(0),originalSdr.data())==0,"JPEG gain fixture");
    const auto jpegBytes=read(jpeg);auto jpegSource=decode_jpeg_gain_map(jpegBytes.data(),jpegBytes.size());
    check(bool(jpegSource)&&reconstruct_gain_map_f32(jpegSource.get(),hdr,gamut,&sdr)&&hdr.size()==input.size()&&sdr.size()==input.size(),"JPEG gain-map F32 reconstruction");
    jxrc::Options jpegOptions;jpegOptions.mode=jxrc::Mode::process;jpegOptions.gain_map_reconstruction=jxrc::GainMapReconstruction::full_image;jpegOptions.target=jxrc::Target::jxl_linear;
    jxrc::convert(jpeg,dir/"jpeg-gain-f32.jxl",jpegOptions);
}
