#pragma once
#include "jxrc/converter.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

// Working RGB is linear display light with 1.0 = 203 nits. Preserve every
// channel below the shoulder, and scale RGB together above it to retain hue.
inline std::vector<float> tone_map_sdr_base(const std::vector<float>& hdr,
                                            float shoulder, float range_nits,
                                            jxrc::SdrToneMapper mapper = jxrc::SdrToneMapper::logarithmic) {
    std::vector<float> sdr=hdr;
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
    for(size_t i=0;i<sdr.size();i+=4) {
        const float peak=std::max({std::isfinite(hdr[i])?hdr[i]:0.f,
                                   std::isfinite(hdr[i+1])?hdr[i+1]:0.f,
                                   std::isfinite(hdr[i+2])?hdr[i+2]:0.f,0.f});
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
        const float scale=peak>0.f?mapped/peak:0.f;
        for(unsigned c=0;c<3;++c)sdr[i+c]=std::clamp((std::isfinite(hdr[i+c])?hdr[i+c]:0.f)*scale,0.f,1.f);
    }
    return sdr;
}
