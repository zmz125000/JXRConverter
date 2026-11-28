#pragma once
#include "jxrc/converter.hpp"
#include "skiv_tone_map.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// Working RGB is linear display light with 1.0 = 203 nits. Preserve every
// pixel below the selected metric shoulder; alpha is preserved.
inline void tone_map_sdr_inplace(std::vector<float>& sdr,
                                 float shoulder, float range_nits,
                                 jxrc::SdrToneMapper mapper = jxrc::SdrToneMapper::logarithmic,
                                 jxrc::SdrColorMapping mapping = jxrc::SdrColorMapping::max_rgb,
                                 std::array<float,3> luma = {.2126f,.7152f,.0722f},
                                 std::array<float,3> zones = {}, bool auto_range = false, bool auto_curve = false,
                                 int gamut = 0) {
    const size_t pixels=sdr.size()/4;
    const unsigned jobs=hdr_pixel_jobs(pixels);
    const AutoSdrTone auto_tone=auto_curve?analyze_auto_sdr_tone(sdr,luma):AutoSdrTone{};
    if(auto_curve)zones[1]=0.f;
    const bool has_zones=zones!=std::array<float,3>{};
    if(auto_tone.active()||has_zones) {
        std::vector<float> peaks(jobs);
        hdr_parallel_pixels(pixels,jobs,[&](size_t begin,size_t end,unsigned job) {
            float peak=0.f;
            for(size_t i=begin*4;i<end*4;i+=4) {
                if(!std::isfinite(sdr[i+3])||sdr[i+3]<=0.f)continue;
                for(unsigned c=0;c<3;++c) {
                    if(!std::isfinite(sdr[i+c]))sdr[i+c]=0.f;
                    if(mapping!=jxrc::SdrColorMapping::ictcp)sdr[i+c]=std::max(sdr[i+c],0.f);
                }
                if(auto_tone.active())apply_auto_sdr_tone(sdr[i],sdr[i+1],sdr[i+2],auto_tone,luma);
                if(has_zones)apply_tonal_zones(sdr[i],sdr[i+1],sdr[i+2],zones[0],zones[1],zones[2],luma);
                peak=std::max({peak,sdr[i],sdr[i+1],sdr[i+2]});
            }
            peaks[job]=peak;
        });
        const float peak=*std::max_element(peaks.begin(),peaks.end());
        range_nits=auto_range?peak*203.f:std::min(range_nits,peak*203.f);
    }
    const float range=std::max(1.f,range_nits/203.f);
    if(range<=1.f) {
        hdr_parallel_pixels(pixels,jobs,[&](size_t begin,size_t end,unsigned) {
            for(size_t i=begin*4;i<end*4;i+=4)for(unsigned c=0;c<3;++c)
                sdr[i+c]=std::clamp(std::isfinite(sdr[i+c])?sdr[i+c]:0.f,0.f,1.f);
        });
        return;
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
    const float log_scale_f=float(log_scale);
    const float inv_log_scale_f=1.f/log_scale_f;
    const float headroom=1.f-shoulder;
    const float inv_headroom=1.f/headroom;
    const float ext_k=headroom/(span*span);
    const float reinhard_k=headroom*((span+knee)/span);
    const auto curve=[&](float peak) {
        if(peak<=shoulder)return peak;
        const float above=std::min(peak,range)-shoulder;
        if(mapper==jxrc::SdrToneMapper::logarithmic) {
            return shoulder+log_scale_f*std::log1p(above*inv_log_scale_f);
        } else if(mapper==jxrc::SdrToneMapper::extended_reinhard) {
            return shoulder+above*(1.f+above*ext_k)/(1.f+above*inv_headroom);
        } else {
            return shoulder+reinhard_k*(above/(above+knee));
        }
    };
    if(mapping==jxrc::SdrColorMapping::ictcp) {
        const skiv::IctcpSdrMapping color(gamut,shoulder);
        hdr_parallel_pixels(pixels,jobs,[&](size_t begin,size_t end,unsigned) {
            for(size_t i=begin*4;i<end*4;i+=4)color.map(&sdr[i],[&](double intensity){return curve(float(intensity));});
        });
        return;
    }
    hdr_parallel_pixels(pixels,jobs,[&](size_t begin,size_t end,unsigned) {
        for(size_t i=begin*4;i<end*4;i+=4) {
            std::array<float,3> rgb;
            for(unsigned c=0;c<3;++c)rgb[c]=std::max(std::isfinite(sdr[i+c])?sdr[i+c]:0.f,0.f);
            if(mapping==jxrc::SdrColorMapping::per_channel) {
                for(unsigned c=0;c<3;++c)sdr[i+c]=std::clamp(curve(rgb[c]),0.f,1.f);
            } else {
                const float metric=mapping==jxrc::SdrColorMapping::luminance?
                    rgb[0]*luma[0]+rgb[1]*luma[1]+rgb[2]*luma[2]:std::max({rgb[0],rgb[1],rgb[2]});
                const float scale=metric>0.f?curve(metric)/metric:0.f;
                for(unsigned c=0;c<3;++c)sdr[i+c]=std::clamp(rgb[c]*scale,0.f,1.f);
            }
        }
    });
}

inline std::vector<float> tone_map_sdr_base(const std::vector<float>& hdr,
                                            float shoulder, float range_nits,
                                            jxrc::SdrToneMapper mapper = jxrc::SdrToneMapper::logarithmic,
                                            jxrc::SdrColorMapping mapping = jxrc::SdrColorMapping::max_rgb,
                                            std::array<float,3> luma = {.2126f,.7152f,.0722f},
                                            std::array<float,3> zones = {}, bool auto_range = false, bool auto_curve = false,
                                            int gamut = 0) {
    std::vector<float> sdr=hdr;
    tone_map_sdr_inplace(sdr,shoulder,range_nits,mapper,mapping,luma,zones,auto_range,auto_curve,gamut);
    return sdr;
}
