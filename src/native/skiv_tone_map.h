#pragma once
// Adapted from SKIV's SDR export path (src/utility/image.cpp and
// include/utility/image.h). Copyright (c) 2024 Aemony.
// Copyright 2024 Andon "Kaldaien" Coleman (tone_mapping.hlsli).
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
#include "jxrc/converter.hpp"
#include "hdr_parallel.h"
#include "sdr_auto_tone.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

inline bool skiv_tone_mapper(jxrc::SdrToneMapper mapper) {
    return mapper==jxrc::SdrToneMapper::skiv||mapper==jxrc::SdrToneMapper::skiv_custom||mapper==jxrc::SdrToneMapper::skiv_manual;
}
namespace skiv {
using RGB=std::array<double,3>;
using Matrix=std::array<RGB,3>;
constexpr Matrix rgb709_xyz={{{.412390798330307,.357584327459335,.180480793118477},
    {.212639003992081,.715168654918671,.072192318737507},
    {.019330818206072,.119194783270359,.950532138347626}}};
constexpr Matrix rgb2020_xyz={{{.636958062648773,.144616901874542,.168880969285965},
    {.262700200080872,.677998065948486,.059301715344191},
    {0.,.028072692453861,1.060985088348389}}};
constexpr Matrix rgbp3_xyz={{{.486570948648216,.265667693169093,.198217285234363},
    {.228974564069749,.691738521836506,.079286914093745},
    {0.,.045113381858903,1.043944368900976}}};
constexpr Matrix xyz_lms={{{.3592,.6976,-.0358},{-.1922,1.1004,.0755},{.007,.0749,.8434}}};
constexpr Matrix lms_xyz={{{2.070180056695614,-1.326456876103021,.206616006847855},
    {.364988250032657,.680467362852235,-.045421753075853},
    {-.049595542238932,-.049421161186757,1.187995941732803}}};
constexpr Matrix xyz709={{{3.240969896316528,-1.537383198738098,-.498610764741898},
    {-.969243645668030,1.875967502593994,.041555058211088},
    {.055630080401897,-.203976958990097,1.056971549987793}}};
constexpr Matrix lms_ictcp={{{.5,.5,0.},{1.6137,-3.3234,1.7097},{4.378,-4.2455,-.1325}}};
constexpr Matrix ictcp_lms={{{1.,.00860514569398152,.11103560447547328},
    {1.,-.00860514569398152,-.11103560447547328},
    {1.,.560048859562639,-.3206374702321221}}};
inline RGB transform(const Matrix& m,const RGB& v) {
    return {m[0][0]*v[0]+m[0][1]*v[1]+m[0][2]*v[2],
        m[1][0]*v[0]+m[1][1]*v[1]+m[1][2]*v[2],
        m[2][0]*v[0]+m[2][1]*v[1]+m[2][2]*v[2]};
}
inline Matrix multiply(const Matrix& a,const Matrix& b) {
    Matrix m{};
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)
        m[r][c]=a[r][0]*b[0][c]+a[r][1]*b[1][c]+a[r][2]*b[2][c];
    return m;
}
inline Matrix inverse(const Matrix& m) {
    const double a=m[0][0],b=m[0][1],c=m[0][2],d=m[1][0],e=m[1][1],f=m[1][2],g=m[2][0],h=m[2][1],i=m[2][2];
    const double det=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
    return {{{(e*i-f*h)/det,(c*h-b*i)/det,(b*f-c*e)/det},
        {(f*g-d*i)/det,(a*i-c*g)/det,(c*d-a*f)/det},
        {(d*h-e*g)/det,(b*g-a*h)/det,(a*e-b*d)/det}}};
}
// SKIV's intermediate scRGB scale is 80 nits per unit, with PQ peak 125.
inline double pq_exact(double sc) {
    const double p=std::pow(std::max(sc,0.)/125.,.1593017578125);
    return std::pow((.8359375+18.8515625*p)/(1.+18.6875*p),78.84375);
}
inline double pq(double sc) {
    static const double kPqZero=std::pow(.8359375,78.84375);
    if(!(sc>0.))return kPqZero;
    const double n=sc*(1./125.);
    if(n>=1.)return pq_exact(sc);
    constexpr uint32_t kMinExp=97;  // 2^-30
    constexpr uint32_t kMaxExp=127; // 2^0 = 1.0
    constexpr uint32_t kMantBits=9;
    static constexpr uint32_t kShift=23-kMantBits;
    static constexpr uint32_t kBase=kMinExp<<kMantBits;
    static constexpr uint32_t kSize=((kMaxExp-kMinExp)<<kMantBits)+1;
    static const std::vector<double> lut=[] {
        std::vector<double> table(kSize+1);
        for(uint32_t i=0;i<=kSize;++i) {
            const uint32_t bits=(kBase+i)<<kShift;
            float fv=0.f;std::memcpy(&fv,&bits,sizeof(fv));
            const double v=std::min(double(fv),1.);
            const double p=std::pow(v,.1593017578125);
            table[i]=std::pow((.8359375+18.8515625*p)/(1.+18.6875*p),78.84375);
        }
        return table;
    }();
    const float nf=float(n);
    uint32_t bits=0;std::memcpy(&bits,&nf,sizeof(bits));
    const uint32_t key=bits>>kShift;
    if(key<kBase)return pq_exact(sc);
    const uint32_t idx=std::min(key-kBase,kSize-1);
    const double frac=double(bits&((1u<<kShift)-1u))*(1./double(1u<<kShift));
    return lut[idx]+(lut[idx+1]-lut[idx])*frac;
}
inline double inverse_pq_exact(double code) {
    const double p=std::pow(std::max(code,0.),1./78.84375);
    return 125.*std::pow(std::max(p-.8359375,0.)/std::max(18.8515625-18.6875*p,1e-12),1./.1593017578125);
}
inline double inverse_pq(double code) {
    if(!(code>0.))return 0.;
    if(code>=1.)return inverse_pq_exact(code);
    static constexpr unsigned kSteps=8192;
    static const std::vector<double> lut=[] {
        std::vector<double> table(kSteps+1);
        for(unsigned i=0;i<=kSteps;++i)table[i]=inverse_pq_exact(double(i)/double(kSteps));
        return table;
    }();
    const double pos=code*double(kSteps);
    const unsigned idx=std::min(unsigned(pos),kSteps-1);
    const double frac=pos-double(idx);
    return lut[idx]+(lut[idx+1]-lut[idx])*frac;
}

class IctcpSdrMapping {
    // ITU-R BT.2100 PQ ICtCp matrices. Fold working RGB into the LMS
    // transform so the same mapping works in BT.709, Display P3 and BT.2020.
    static constexpr Matrix rgb2020_lms={{{1688./4096,2146./4096,262./4096},
        {683./4096,2951./4096,462./4096},{99./4096,309./4096,3688./4096}}};
    static constexpr Matrix lms_pq_ictcp={{{.5,.5,0.},
        {6610./4096,-13613./4096,7003./4096},{17933./4096,-17390./4096,-543./4096}}};
    static constexpr double sdr_to_scrgb=203./80.;
    Matrix to_lms_,from_lms_;
    double shoulder_,shoulder_pq_,white_pq_;

    static bool in_sdr_gamut(const RGB& rgb) {
        return std::all_of(rgb.begin(),rgb.end(),[](double c){return c>=-1e-9&&c<=1.+1e-9;});
    }
public:
    IctcpSdrMapping(int gamut,double shoulder):
        to_lms_(gamut==2?rgb2020_lms:multiply(rgb2020_lms,multiply(inverse(rgb2020_xyz),gamut==1?rgbp3_xyz:rgb709_xyz))),
        from_lms_(inverse(to_lms_)),shoulder_(shoulder),shoulder_pq_(pq(shoulder*sdr_to_scrgb)),white_pq_(pq(sdr_to_scrgb)) {}

    template<class Curve>
    void map(float* pixel,const Curve& curve) const {
        const RGB input={std::isfinite(pixel[0])?pixel[0]:0.,std::isfinite(pixel[1])?pixel[1]:0.,
                         std::isfinite(pixel[2])?pixel[2]:0.};
        auto lms=transform(to_lms_,input);
        if(lms[0]<=shoulder_&&lms[1]<=shoulder_&&in_sdr_gamut(input)) {
            for(unsigned c=0;c<3;++c)pixel[c]=float(std::clamp(input[c],0.,1.));
            return;
        }
        for(auto& value:lms)value=pq(std::clamp(value*sdr_to_scrgb,0.,125.));
        const auto ictcp=transform(lms_pq_ictcp,lms);
        const double intensity=ictcp[0];
        const double mapped=intensity<=shoulder_pq_?intensity:
            pq(std::clamp(double(curve(inverse_pq(intensity)/sdr_to_scrgb)),0.,1.)*sdr_to_scrgb);
        if(mapped==intensity&&in_sdr_gamut(input)) {
            for(unsigned c=0;c<3;++c)pixel[c]=float(std::clamp(input[c],0.,1.));
            return;
        }
        static const Matrix ictcp_lms_pq=inverse(lms_pq_ictcp);
        const double sdr_intensity=std::min(mapped,white_pq_);
        const auto decode=[&](double chroma) {
            auto linear=transform(ictcp_lms_pq,{sdr_intensity,ictcp[1]*chroma,ictcp[2]*chroma});
            for(auto& value:linear)value=inverse_pq(std::clamp(value,0.,1.))/sdr_to_scrgb;
            return transform(from_lms_,linear);
        };
        const double chroma=intensity>0.?std::min(sdr_intensity/intensity,1.):0.;
        auto rgb=decode(chroma);
        if(!in_sdr_gamut(rgb)) {
            // Reduce both chroma axes together to retain the ICtCp hue angle.
            rgb=decode(0.);
            double lo=0.,hi=chroma;
            for(int step=0;step<8;++step) {
                const double mid=(lo+hi)*.5;
                const auto candidate=decode(mid);
                if(in_sdr_gamut(candidate)){lo=mid;rgb=candidate;}
                else hi=mid;
            }
        }
        for(unsigned c=0;c<3;++c)pixel[c]=float(std::clamp(rgb[c],0.,1.));
    }
};

inline double pow118(double input) {
    if(!(input>0.))return 0.;
    if(input>=1.)return 1.;
    static constexpr unsigned kSteps=2048;
    static const std::vector<float> lut=[] {
        std::vector<float> table(kSteps+1);
        for(unsigned i=0;i<=kSteps;++i) {
            const double u=double(i)/double(kSteps);
            table[i]=float(std::pow(u*u,1.18));
        }
        return table;
    }();
    const float pos=std::sqrt(float(input))*float(kSteps);
    const unsigned idx=std::min(unsigned(pos),kSteps-1);
    const float frac=pos-float(idx);
    return double(lut[idx]+(lut[idx+1]-lut[idx])*frac);
}
inline double intensity(double input,double peak,double white) {
    return pow118(input)*(1.+white/(peak*peak)*input)/(1.+input/white);
}
inline RGB input_xyz(const std::vector<float>& pixels,size_t at,const Matrix& space) {
    RGB rgb{};
    for(unsigned c=0;c<3;++c)rgb[c]=(std::isfinite(pixels[at+c])?double(pixels[at+c]):0.)*(203./80.);
    return transform(space,rgb);
}
// Match SKIV's 65536-bin, 99.94th-percentile luminance estimate. Ignore fully
// invisible pixels and handle a constant image without a zero-range division.
inline double content_peak(const std::vector<float>& pixels,const Matrix& space,bool max_rgb=false) {
    const auto metric=[&](size_t i) {
        if(!max_rgb)return input_xyz(pixels,i,space)[1];
        double peak=0.;
        for(unsigned c=0;c<3;++c)if(std::isfinite(pixels[i+c]))peak=std::max(peak,double(pixels[i+c]));
        return peak*(203./80.);
    };
    struct Limits {
        double low=std::numeric_limits<double>::infinity(),high=-low;
        uint64_t count=0;
    };
    const size_t size=pixels.size()/4;
    const unsigned jobs=hdr_pixel_jobs(size);
    std::vector<Limits> limits(jobs);
    hdr_parallel_pixels(size,jobs,[&](size_t begin,size_t end,unsigned job) {
        Limits local;
        for(size_t i=begin*4;i<end*4;i+=4)if(std::isfinite(pixels[i+3])&&pixels[i+3]>0.f) {
            const double y=metric(i);
            local.low=std::min(local.low,y);local.high=std::max(local.high,y);++local.count;
        }
        limits[job]=local;
    });
    Limits total;
    for(const auto& local:limits) {
        total.low=std::min(total.low,local.low);total.high=std::max(total.high,local.high);total.count+=local.count;
    }
    const double low=total.low,high=total.high;
    const uint64_t count=total.count;
    if(!count)return 0.;
    if(high<=low)return std::max(high,0.);
    constexpr size_t bins=65536;
    thread_local std::vector<uint64_t> histograms;
    histograms.assign(bins*jobs,0);
    uint64_t* const hist_base=histograms.data();
    const double inv_span=65536./(high-low);
    hdr_parallel_pixels(size,jobs,[&](size_t begin,size_t end,unsigned job) {
        auto* histogram=hist_base+bins*job;
        for(size_t i=begin*4;i<end*4;i+=4)if(std::isfinite(pixels[i+3])&&pixels[i+3]>0.f) {
            const double y=std::max(metric(i),0.);
            const auto bin=size_t(std::clamp(std::round((y-low)*inv_span),0.,65535.));
            ++histogram[bin];
        }
    });
    double percent=100.;
    const double inv_count=100./double(count);
    for(int bin=65535;bin>=0;--bin) {
        uint64_t frequency=0;
        for(unsigned job=0;job<jobs;++job)frequency+=histograms[bins*job+size_t(bin)];
        percent-=double(frequency)*inv_count;
        if(percent<=99.94)return std::max(0.,low+(high-low)*double(bin)/65536.);
    }
    return std::max(high,0.);
}
} // namespace skiv

// gamut: 0 BT.709, 1 Display P3, 2 BT.2020. Working pixels use 203-nit units.
// Stock follows SKIV export's 120-nit PQ target, 10000-nit cap and 1.18 power.
// Custom anchors that curve at the manual shoulder and 203-nit SDR white.
inline void tone_map_skiv_inplace(std::vector<float>& output,int gamut,bool custom,float shoulder,float cap_nits) {
    using namespace skiv;
    const Matrix& space=gamut==2?rgb2020_xyz:gamut==1?rgbp3_xyz:rgb709_xyz;
    const Matrix from_xyz=inverse(space);
    const Matrix to_lms=multiply(xyz_lms,space);
    const Matrix lms_to_rgb709=multiply(xyz709,lms_xyz);
    const Matrix rgb709_to_gamut=gamut?multiply(from_xyz,rgb709_xyz):Matrix{};
    const double cap=custom?std::clamp(double(cap_nits),204.,10000.):10000.;
    const double peak=std::min(content_peak(output,space),cap/80.);
    const double white=pq(1.5),peak_pq=std::max(white,pq(peak));
    const double knee=std::clamp(double(shoulder),0.,.99);
    const double linear_knee=knee*203./80.;
    const double input_knee=pq(linear_knee),output_white=pq(203./80.);
    const double custom_peak=std::max(peak_pq,output_white);
    const double curve_knee=intensity(input_knee,custom_peak,white),curve_peak=intensity(custom_peak,custom_peak,white);
    const double custom_scale=(output_white-input_knee)/std::max(curve_peak-curve_knee,1e-12);
    const double inv_white=1./white;
    const double stock_k=white/(peak_pq*peak_pq);
    const double custom_k=white/(custom_peak*custom_peak);
    const double sc_scale=203./80.;
    const double out_scale=custom?80./203.:1.;
    const size_t pixels=output.size()/4;
    hdr_parallel_pixels(pixels,hdr_pixel_jobs(pixels),[&](size_t begin,size_t end,unsigned) {
    for(size_t pixel=begin;pixel<end;++pixel) {
        const size_t i=pixel*4;
        if(!std::isfinite(output[i+3])||output[i+3]<=0.f){output[i]=output[i+1]=output[i+2]=0.f;continue;}
        const RGB in_rgb={
            (std::isfinite(output[i])?double(output[i]):0.)*sc_scale,
            (std::isfinite(output[i+1])?double(output[i+1]):0.)*sc_scale,
            (std::isfinite(output[i+2])?double(output[i+2]):0.)*sc_scale};
        auto lms=transform(to_lms,in_rgb);
        // ICtCp intensity is the average of PQ L and M. Below the knee
        // in both channels, the custom curve leaves this pixel unchanged.
        if(custom&&lms[0]<=linear_knee&&lms[1]<=linear_knee)continue;
        lms[0]=pq(lms[0]);lms[1]=pq(lms[1]);
        if(custom&&.5*lms[0]+.5*lms[1]<=input_knee)continue;
        lms[2]=pq(lms[2]);
        auto ictcp=transform(lms_ictcp,lms);const double input=std::max(ictcp[0],0.);
        double reference,mapped;
        if(custom) {
            const double clipped=std::min(input,custom_peak);
            const double curve_val=pow118(clipped)*(1.+custom_k*clipped)/(1.+clipped*inv_white);
            mapped=std::clamp(input_knee+(curve_val-curve_knee)*custom_scale,input_knee,output_white);
            reference=input;
        } else {
            reference=pow118(input);
            mapped=reference*(1.+stock_k*input)/(1.+input*inv_white);
        }
        const double scale=reference>0.&&mapped>0.?std::min(reference/mapped,mapped/reference):0.;
        ictcp[0]=mapped;ictcp[1]*=scale;ictcp[2]*=scale;
        lms=transform(ictcp_lms,ictcp);for(auto& value:lms)value=inverse_pq(value);
        auto rgb=transform(lms_to_rgb709,lms);
        for(auto& value:rgb)value=std::clamp(value*out_scale,0.,1.);
        // SKIV clips in its SDR BT.709 space. Rematrix that result back to the
        // export gamut so encoders keep their existing, matching ICC/CICP tags.
        if(gamut)rgb=transform(rgb709_to_gamut,rgb);
        for(unsigned c=0;c<3;++c)output[i+c]=float(std::clamp(rgb[c],0.,1.));
    }
    });
}

inline std::vector<float> tone_map_skiv_base(const std::vector<float>& hdr,int gamut,bool custom,float shoulder,float cap_nits) {
    std::vector<float> output=hdr;
    tone_map_skiv_inplace(output,gamut,custom,shoulder,cap_nits);
    return output;
}

// Manual tonal edits remain HDR. Apply the selected color mapping with SKIV's
// intensity curve.
inline void tone_map_skiv_manual_inplace(std::vector<float>& adjusted,int gamut,
        float shoulder,float cap_nits,jxrc::SdrColorMapping mapping,
        std::array<float,3> luma,std::array<float,3> zones,bool auto_curve=false) {
    using namespace skiv;
    const AutoSdrTone auto_tone=auto_curve?analyze_auto_sdr_tone(adjusted,luma):AutoSdrTone{};
    if(auto_curve)zones[1]=0.f;
    const size_t pixels=adjusted.size()/4;
    const bool has_zones=zones!=std::array<float,3>{};
    hdr_parallel_pixels(pixels,hdr_pixel_jobs(pixels),[&](size_t begin,size_t end,unsigned) {
        for(size_t pixel=begin;pixel<end;++pixel) {
            const size_t i=pixel*4;
            if(!std::isfinite(adjusted[i+3])||adjusted[i+3]<=0.f) {
                adjusted[i]=adjusted[i+1]=adjusted[i+2]=0.f;continue;
            }
            for(unsigned c=0;c<3;++c) {
                if(!std::isfinite(adjusted[i+c]))adjusted[i+c]=0.f;
                if(mapping!=jxrc::SdrColorMapping::ictcp)adjusted[i+c]=std::max(adjusted[i+c],0.f);
            }
            if(auto_tone.active())apply_auto_sdr_tone(adjusted[i],adjusted[i+1],adjusted[i+2],auto_tone,luma);
            if(has_zones)apply_tonal_zones(adjusted[i],adjusted[i+1],adjusted[i+2],zones[0],zones[1],zones[2],luma);
        }
    });
    const Matrix& space=gamut==2?rgb2020_xyz:gamut==1?rgbp3_xyz:rgb709_xyz;
    const double cap=std::clamp(double(cap_nits),204.,10000.);
    const bool peak_rgb=mapping==jxrc::SdrColorMapping::max_rgb||mapping==jxrc::SdrColorMapping::per_channel;
    const double peak=std::min(content_peak(adjusted,space,peak_rgb),cap/80.);
    const double white=pq(1.5),output_white=pq(203./80.);
    const double peak_pq=std::max(output_white,pq(peak));
    const double knee=std::clamp(double(shoulder),0.,.99),input_knee=pq(knee*203./80.);
    const double curve_knee=intensity(input_knee,peak_pq,white),curve_peak=intensity(peak_pq,peak_pq,white);
    const double curve_scale=(output_white-input_knee)/std::max(curve_peak-curve_knee,1e-12);
    const auto curve=[&](double value) {
        if(value<=knee)return value;
        const double input=std::min(pq(value*(203./80.)),peak_pq);
        const double mapped=input_knee+(intensity(input,peak_pq,white)-curve_knee)*curve_scale;
        return std::clamp(inverse_pq(std::clamp(mapped,input_knee,output_white))*(80./203.),0.,1.);
    };
    if(mapping==jxrc::SdrColorMapping::ictcp) {
        const IctcpSdrMapping color(gamut,knee);
        hdr_parallel_pixels(pixels,hdr_pixel_jobs(pixels),[&](size_t begin,size_t end,unsigned) {
            for(size_t i=begin*4;i<end*4;i+=4)color.map(&adjusted[i],curve);
        });
        return;
    }
    hdr_parallel_pixels(pixels,hdr_pixel_jobs(pixels),[&](size_t begin,size_t end,unsigned) {
        for(size_t pixel=begin;pixel<end;++pixel) {
            const size_t i=pixel*4;
            if(mapping==jxrc::SdrColorMapping::per_channel) {
                for(unsigned c=0;c<3;++c)adjusted[i+c]=float(curve(adjusted[i+c]));
            } else {
                const double metric=mapping==jxrc::SdrColorMapping::luminance?
                    double(adjusted[i])*luma[0]+double(adjusted[i+1])*luma[1]+double(adjusted[i+2])*luma[2]:
                    std::max({adjusted[i],adjusted[i+1],adjusted[i+2]});
                if(metric>knee) {
                    const double scale=curve(metric)/metric;
                    for(unsigned c=0;c<3;++c)adjusted[i+c]=float(std::clamp(adjusted[i+c]*scale,0.,1.));
                }
            }
        }
    });
}

inline std::vector<float> tone_map_skiv_manual(const std::vector<float>& hdr,int gamut,
        float shoulder,float cap_nits,jxrc::SdrColorMapping mapping,
        std::array<float,3> luma,std::array<float,3> zones,bool auto_curve=false) {
    auto adjusted=hdr;
    tone_map_skiv_manual_inplace(adjusted,gamut,shoulder,cap_nits,mapping,luma,zones,auto_curve);
    return adjusted;
}
