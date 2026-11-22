#pragma once
#include "jxrc/converter.hpp"
#include "sdr_auto_tone.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// Working RGB is linear display light with 1.0 = 203 nits. Preserve every
// pixel below the selected metric shoulder; alpha is preserved.
inline std::vector<float> tone_map_sdr_base(const std::vector<float>& hdr,
                                            float shoulder, float range_nits,
                                            jxrc::SdrToneMapper mapper = jxrc::SdrToneMapper::logarithmic,
                                            jxrc::SdrColorMapping mapping = jxrc::SdrColorMapping::max_rgb,
                                            std::array<float,3> luma = {.2126f,.7152f,.0722f},
                                            std::array<float,3> zones = {}, bool auto_range = false, bool auto_curve = false) {
    std::vector<float> sdr=hdr;
    if(auto_curve)zones[1]=auto_sdr_midtones(hdr,luma);
    if(zones!=std::array<float,3>{}) {
        float peak=0.f;
        for(size_t i=0;i<sdr.size();i+=4) {
            if(!std::isfinite(sdr[i+3])||sdr[i+3]<=0.f)continue;
            for(unsigned c=0;c<3;++c)if(!std::isfinite(sdr[i+c]))sdr[i+c]=0.f;
            apply_tonal_zones(sdr[i],sdr[i+1],sdr[i+2],zones[0],zones[1],zones[2],luma);
            peak=std::max({peak,sdr[i],sdr[i+1],sdr[i+2]});
        }
        range_nits=auto_range?peak*203.f:std::min(range_nits,peak*203.f);
    }
    const float range=std::max(1.f,range_nits/203.f);
    if(range<=1.f) {
        for(size_t i=0;i<sdr.size();i+=4)for(unsigned c=0;c<3;++c)
            sdr[i+c]=std::clamp(std::isfinite(sdr[i+c])?sdr[i+c]:0.f,0.f,1.f);
        return sdr;
    }
    const float span=range-shoulder;
    // Normalized Reinhard shoulder: continuous slope 1 at the join, 0..1
    // output, and the selected HDR endpoint maps exactly to SDR white.
    const float knee=(1.f-shoulder)*span/(range-1.f);
    // Solve a*log(1+span/a)=headroom once. The join has slope 1,
    // with a much slower highlight rolloff than the rational shoulders.
    double log_scale=1.;
    if(mapper==jxrc::SdrToneMapper::logarithmic) {
        double lo=1e-12,hi=double(span)*1e8;
        for(unsigned n=0;n<90;++n) {
            const double mid=(lo+hi)*.5;
            if(mid*std::log1p(span/mid)>1.-shoulder)hi=mid;else lo=mid;
        }
        log_scale=(lo+hi)*.5;
    }
    const auto curve=[&](float peak) {
        float mapped=peak;
        if(peak>shoulder) {
            const float above=std::min(peak,range)-shoulder;
            const float headroom=1.f-shoulder;
            if(mapper==jxrc::SdrToneMapper::logarithmic) {
                mapped=shoulder+float(log_scale*std::log1p(above/log_scale));
            } else if(mapper==jxrc::SdrToneMapper::extended_reinhard) {
                // White-point extension keeps more slope at the endpoint than
                // the normalized asymptotic curve, retaining highlight texture.
                mapped=shoulder+above*(1.f+above*headroom/(span*span))/(1.f+above/headroom);
            } else {
                mapped=shoulder+headroom*(above/(above+knee))*((span+knee)/span);
            }
        }
        return mapped;
    };
    for(size_t i=0;i<sdr.size();i+=4) {
        std::array<float,3> rgb;
        for(unsigned c=0;c<3;++c)rgb[c]=std::max(std::isfinite(sdr[i+c])?sdr[i+c]:0.f,0.f);
        if(mapping==jxrc::SdrColorMapping::per_channel) {
            for(unsigned c=0;c<3;++c)sdr[i+c]=std::clamp(curve(rgb[c]),0.f,1.f);
        } else {
            const float metric=mapping==jxrc::SdrColorMapping::luminance?
                rgb[0]*luma[0]+rgb[1]*luma[1]+rgb[2]*luma[2]:std::max({rgb[0],rgb[1],rgb[2]});
            const float scale=metric>0.f?curve(metric)/metric:0.f;
            for(unsigned c=0;c<3;++c)rgb[c]*=scale;
            for(unsigned c=0;c<3;++c)sdr[i+c]=std::clamp(rgb[c],0.f,1.f);
        }
    }
    return sdr;
}
