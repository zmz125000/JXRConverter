#pragma once
#include "tonal_zones.h"
#include <vector>

inline void apply_auto_sdr_tone(const std::vector<float>& source, const std::array<float,3>& luma,
                                std::array<float,3>& zones, float& shoulder, bool auto_shoulder) {
    double visible_area=0., hdr_area=0.;
    for(size_t i=0;i<source.size();i+=4) {
        const float alpha=source[i+3];
        if(!std::isfinite(alpha)||alpha<=0.f)continue;
        const float weight=std::min(alpha,1.f);
        const float y=source[i]*luma[0]+source[i+1]*luma[1]+source[i+2]*luma[2];
        visible_area+=weight;
        if(std::isfinite(y)&&y>1.f)hdr_area+=weight;
    }
    const float fraction=visible_area>0. ? float(hdr_area/visible_area) : 0.f;
    // Fade in the correction for small highlights and leave SDR scenes neutral.
    zones[1]=-.72f*std::min(fraction/.08f,1.f);
    if(auto_shoulder)shoulder=std::clamp(.95f-.75f*fraction,.20f,.95f);
}
