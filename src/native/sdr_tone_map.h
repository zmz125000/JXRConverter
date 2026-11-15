#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

// Working RGB is linear display light with 1.0 = 203 nits. Preserve every
// channel below the shoulder, and scale RGB together above it to retain hue.
inline std::vector<float> tone_map_sdr_base(const std::vector<float>& hdr,
                                            float shoulder, float range_nits) {
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
    for(size_t i=0;i<sdr.size();i+=4) {
        const float peak=std::max({std::isfinite(hdr[i])?hdr[i]:0.f,
                                   std::isfinite(hdr[i+1])?hdr[i+1]:0.f,
                                   std::isfinite(hdr[i+2])?hdr[i+2]:0.f,0.f});
        float mapped=peak;
        if(peak>shoulder) {
            const float above=std::min(peak,range)-shoulder;
            mapped=shoulder+(1.f-shoulder)*(above/(above+knee))*((span+knee)/span);
        }
        const float scale=peak>0.f?mapped/peak:0.f;
        for(unsigned c=0;c<3;++c)sdr[i+c]=std::clamp((std::isfinite(hdr[i+c])?hdr[i+c]:0.f)*scale,0.f,1.f);
    }
    return sdr;
}
