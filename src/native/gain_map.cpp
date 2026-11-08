#include "gain_map.h"
#include "hdr_parallel.h"
#include "linear_color_profile.h"
#include <avif/internal.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace {
using Image=std::unique_ptr<avifImage,decltype(&avifImageDestroy)>;
struct Rgb {
    avifRGBImage value{};
    explicit Rgb(const avifImage* image) {
        avifRGBImageSetDefaults(&value,image);value.format=AVIF_RGB_FORMAT_RGBA;
        value.alphaPremultiplied=AVIF_FALSE;value.maxThreads=int(hdr_pixel_jobs(size_t(image->width)*image->height));
    }
    ~Rgb(){avifRGBImageFreePixels(&value);}
    bool decode(const avifImage* image){return avifRGBImageAllocatePixels(&value)==AVIF_RESULT_OK&&avifImageYUVToRGB(image,&value)==AVIF_RESULT_OK;}
    unsigned code(unsigned x,unsigned y,unsigned c) const {
        const auto* row=value.pixels+size_t(y)*value.rowBytes;
        if(value.depth==8)return row[size_t(x)*4+c];
        uint16_t result;std::memcpy(&result,row+(size_t(x)*4+c)*2,2);return result;
    }
};
bool dimensions(const avifImage* image){return image&&image->width&&image->height&&uint64_t(image->width)*image->height<=80000000&&image->depth>=8&&image->depth<=16;}
double fraction(avifUnsignedFraction f){return f.d?double(f.n)/f.d:NAN;}
double fraction(avifSignedFraction f){return f.d?double(f.n)/f.d:NAN;}
bool profile(avifColorPrimaries cp,const avifRWData& icc,skcms_ICCProfile& linear) {
    skcms_Matrix3x3 matrix{};
    if(icc.size) {
        skcms_ICCProfile source{};
        if(icc.size>16u*1024*1024||!skcms_Parse(icc.data,icc.size,&source)||!source.has_toXYZD50)return false;
        matrix=source.toXYZD50;
    } else {
        if(cp==0||cp==2||cp==3||cp>22)return false;
        double m[3][3];if(!avifColorPrimariesComputeRGBToXYZD50Matrix(cp,m))return false;
        for(int r=0;r<3;++r)for(int c=0;c<3;++c)matrix.vals[r][c]=float(m[r][c]);
    }
    skcms_Init(&linear);skcms_SetTransferFunction(&linear,skcms_Identity_TransferFunction());skcms_SetXYZD50(&linear,&matrix);
    return skcms_MakeUsableAsDestination(&linear);
}
bool conversion(const skcms_ICCProfile& source,const skcms_ICCProfile& dest,skcms_Matrix3x3& matrix) {
    skcms_Matrix3x3 inverse{};if(!skcms_Matrix3x3_invert(&dest.toXYZD50,&inverse))return false;
    matrix=skcms_Matrix3x3_concat(&inverse,&source.toXYZD50);return true;
}
void convert(const skcms_Matrix3x3& matrix,float* rgb) {
    float result[3]{};for(int r=0;r<3;++r)for(int c=0;c<3;++c)result[r]+=matrix.vals[r][c]*rgb[c];std::copy_n(result,3,rgb);
}
bool known(avifColorPrimaries cp){return cp==1||cp==9||cp==12;}
float weight(float headroom,float base,float alternate) {
    if(base==alternate)return 0;
    const float w=std::clamp((headroom-base)/(alternate-base),0.f,1.f);
    return alternate<base?-w:w;
}
// Sample encoded gain codes at pixel centers. Interpolate before undoing gain
// gamma, without an 8/10/12-bit resampling intermediate.
void sample(const std::vector<float>& codes,unsigned w,unsigned h,unsigned x,unsigned y,unsigned dw,unsigned dh,float* out) {
    const double fx=std::clamp((double(x)+.5)*w/dw-.5,0.,double(w-1));
    const double fy=std::clamp((double(y)+.5)*h/dh-.5,0.,double(h-1));
    const unsigned x0=unsigned(fx),y0=unsigned(fy),x1=std::min(x0+1,w-1),y1=std::min(y0+1,h-1);
    const float dx=float(fx-x0),dy=float(fy-y0);
    for(unsigned c=0;c<3;++c) {
        const float a=codes[(size_t(y0)*w+x0)*3+c],b=codes[(size_t(y0)*w+x1)*3+c];
        const float d=codes[(size_t(y1)*w+x0)*3+c],e=codes[(size_t(y1)*w+x1)*3+c];
        out[c]=(a+(b-a)*dx)*(1-dy)+(d+(e-d)*dx)*dy;
    }
}
}

bool decode_gain_map_f32(const avifGainMap* gain,std::vector<float>& codes,unsigned& width,unsigned& height) {
    codes.clear();width=height=0;if(!gain||!dimensions(gain->image))return false;
    Rgb rgb(gain->image);if(!rgb.decode(gain->image))return false;
    width=gain->image->width;height=gain->image->height;const float maximum=float((1u<<rgb.value.depth)-1);
    codes.resize(size_t(width)*height*3);
    hdr_parallel_pixels(height,hdr_pixel_jobs(size_t(width)*height),[&](size_t begin,size_t end,unsigned){
        for(unsigned y=unsigned(begin);y<end;++y)for(unsigned x=0;x<width;++x)for(unsigned c=0;c<3;++c)
            codes[(size_t(y)*width+x)*3+c]=rgb.code(x,y,c)/maximum;
    });return true;
}

bool reconstruct_gain_map_f32(const avifImage* image,std::vector<float>& rgba,uhdr_color_gamut_t& gamut,std::vector<float>* sdr,const std::vector<float>* gainCodes) {
    if(!dimensions(image)||!image->gainMap||!dimensions(image->gainMap->image))return false;
    const auto* gain=image->gainMap;avifDiagnostics diag{};
    if(avifGainMapValidateMetadata(gain,&diag)!=AVIF_RESULT_OK)return false;
    const float baseHead=float(fraction(gain->baseHdrHeadroom)),altHead=float(fraction(gain->alternateHdrHeadroom));
    if(!std::isfinite(baseHead)||!std::isfinite(altHead))return false;
    const float hdrWeight=weight(std::max(baseHead,altHead),baseHead,altHead);
    const float sdrWeight=weight(std::min(baseHead,altHead),baseHead,altHead);
    const bool mathBase=gain->useBaseColorSpace||(gain->altColorPrimaries==2&&!gain->altICC.size);
    const auto mathCp=mathBase?image->colorPrimaries:gain->altColorPrimaries;
    const auto& mathIcc=mathBase?image->icc:gain->altICC;
    const auto renditionCp=baseHead>=altHead?image->colorPrimaries:gain->altColorPrimaries;
    const auto& renditionIcc=baseHead>=altHead?image->icc:gain->altICC;
    const auto outputCp=!renditionIcc.size&&known(renditionCp)?renditionCp:avifColorPrimaries(AVIF_COLOR_PRIMARIES_BT2020);
    skcms_ICCProfile mathProfile{},outputProfile{},sourceProfile{};const avifRWData empty=AVIF_DATA_EMPTY;
    if(!profile(mathCp,mathIcc,mathProfile)||!profile(outputCp,empty,outputProfile))return false;
    skcms_Matrix3x3 outputMatrix{},inputMatrix{};
    if(!conversion(mathProfile,outputProfile,outputMatrix))return false;
    bool iccSource=image->icc.size!=0;
    if(iccSource) {
        if(image->icc.size>16u*1024*1024||!skcms_Parse(image->icc.data,image->icc.size,&sourceProfile))return false;
    } else if(!profile(image->colorPrimaries,empty,sourceProfile)||!conversion(sourceProfile,mathProfile,inputMatrix))return false;
    Rgb base(image);if(!base.decode(image))return false;
    const unsigned w=image->width,h=image->height;const size_t count=size_t(w)*h;
    const unsigned maximum=(1u<<base.value.depth)-1;std::vector<float> linear(count*4);
    std::vector<float> transfer(maximum+1);
    if(!iccSource) {
        // Reject unspecified transfer functions rather than guessing an EOTF.
        if(image->transferCharacteristics==0||image->transferCharacteristics==2||image->transferCharacteristics==3||image->transferCharacteristics>18)return false;
        const auto eotf=avifTransferCharacteristicsGetGammaToLinearFunction(image->transferCharacteristics);
        if(!eotf)return false;
        for(unsigned i=0;i<=maximum;++i)transfer[i]=eotf(i/float(maximum));
        if(image->transferCharacteristics==AVIF_TRANSFER_CHARACTERISTICS_HLG)for(unsigned i=0;i<=maximum;++i) {
            const float v=i/float(maximum);
            transfer[i]=v<=.5f?4*v*v:(std::exp((v-.55991073f)/.17883277f)+.28466892f);
        }
    }
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
        auto* p=linear.data()+(size_t(y)*w+x)*4;
        for(unsigned c=0;c<3;++c)p[c]=iccSource?base.code(x,y,c)/float(maximum):transfer[base.code(x,y,c)];
        p[3]=base.code(x,y,3)/float(maximum);
        if(!iccSource) {
            if(image->transferCharacteristics==AVIF_TRANSFER_CHARACTERISTICS_HLG) {
                float coeffs[3];avifColorPrimariesComputeYCoeffs(image->colorPrimaries,coeffs);
                const float sceneY=(coeffs[0]*p[0]+coeffs[1]*p[1]+coeffs[2]*p[2])/12.f;
                const float scale=std::pow(std::max(sceneY,0.f),.2f)*1000.f/(12.f*203.f);
                for(unsigned c=0;c<3;++c)p[c]*=scale;
            }
            convert(inputMatrix,p);
        }
    }
    if(iccSource&&!skcms_Transform(linear.data(),skcms_PixelFormat_RGBA_ffff,skcms_AlphaFormat_Unpremul,&sourceProfile,
        linear.data(),skcms_PixelFormat_RGBA_ffff,skcms_AlphaFormat_Unpremul,&mathProfile,count))return false;
    // Release integer base pixels before allocating optional paired output.
    avifRGBImageFreePixels(&base.value);
    std::vector<float> decodedCodes;const auto& codes=gainCodes?*gainCodes:decodedCodes;
    unsigned gw=gain->image->width,gh=gain->image->height;
    if(gainCodes) {
        if(codes.size()!=size_t(gw)*gh*3)return false;
        for(float code:codes)if(!std::isfinite(code)||code<0||code>1)return false;
    } else if((hdrWeight!=0||(sdr&&sdrWeight!=0))&&!decode_gain_map_f32(gain,decodedCodes,gw,gh))return false;
    float lo[3],hi[3],gamma[3],baseOffset[3],altOffset[3];
    for(unsigned c=0;c<3;++c) {
        lo[c]=float(fraction(gain->gainMapMin[c]));hi[c]=float(fraction(gain->gainMapMax[c]));gamma[c]=1.f/float(fraction(gain->gainMapGamma[c]));
        baseOffset[c]=float(fraction(gain->baseOffset[c]));altOffset[c]=float(fraction(gain->alternateOffset[c]));
        if(!std::isfinite(lo[c])||!std::isfinite(hi[c])||!std::isfinite(gamma[c])||!std::isfinite(baseOffset[c])||!std::isfinite(altOffset[c]))return false;
    }
    // Native-resolution maps have a finite set of codes. Keep the common
    // Apple/ISO path lookup-based instead of evaluating pow/exp per pixel.
    const bool native=!gainCodes&&!codes.empty()&&gw==w&&gh==h;const unsigned gainMax=(1u<<gain->image->depth)-1;
    std::vector<float> hdrBoost[3],sdrBoost[3];
    if(native)for(unsigned c=0;c<3;++c) {
        if(hdrWeight!=0)hdrBoost[c].resize(gainMax+1);
        if(sdr&&sdrWeight!=0)sdrBoost[c].resize(gainMax+1);
        for(unsigned i=0;i<=gainMax;++i) {
            const float logGain=lo[c]+(hi[c]-lo[c])*std::pow(i/float(gainMax),gamma[c]);
            if(!hdrBoost[c].empty())hdrBoost[c][i]=std::exp2(logGain*hdrWeight);
            if(!sdrBoost[c].empty())sdrBoost[c][i]=std::exp2(logGain*sdrWeight);
        }
    }
    std::vector<float> paired(sdr?count*4:0);std::atomic_bool valid{true};
    hdr_parallel_pixels(h,hdr_pixel_jobs(count),[&](size_t begin,size_t end,unsigned){
        for(unsigned y=unsigned(begin);y<end;++y)for(unsigned x=0;x<w;++x) {
            const size_t at=(size_t(y)*w+x)*4;auto* p=linear.data()+at;float logGain[3]{};
            unsigned codeIndex[3]{};
            if(native)for(unsigned c=0;c<3;++c)codeIndex[c]=unsigned(std::lround(codes[(size_t(y)*w+x)*3+c]*gainMax));
            else if(!codes.empty()) {
                float encoded[3];sample(codes,gw,gh,x,y,w,h,encoded);
                for(unsigned c=0;c<3;++c)logGain[c]=lo[c]+(hi[c]-lo[c])*std::pow(encoded[c],gamma[c]);
            }
            auto apply=[&](float value,float* out,const std::vector<float>* boost) {
                for(unsigned c=0;c<3;++c)out[c]=value==0?p[c]:(p[c]+baseOffset[c])*(native?boost[c][codeIndex[c]]:std::exp2(logGain[c]*value))-altOffset[c];
                convert(outputMatrix,out);out[3]=p[3];
                for(unsigned c=0;c<4;++c)if(!std::isfinite(out[c]))valid=false;
            };
            if(sdr)apply(sdrWeight,paired.data()+at,sdrBoost);
            float hdr[4];apply(hdrWeight,hdr,hdrBoost);std::copy_n(hdr,4,p);
        }
    });
    if(!valid)return false;
    rgba.swap(linear);if(sdr)sdr->swap(paired);
    gamut=outputCp==9?UHDR_CG_BT_2100:outputCp==12?UHDR_CG_DISPLAY_P3:UHDR_CG_BT_709;return true;
}
