#pragma once
#include "sdr_tone_map.h"
#include "skiv_tone_map.h"

inline void map_sdr_pixels_inplace(std::vector<float>& linear,int gamut,
                                   float range_nits,const jxrc::Options& options) {
    using jxrc::SdrToneMapper;
    const auto mapper=options.sdr_tone_mapper;
    const auto cap=options.sdr_auto_hdr_range?10000.f:float(options.sdr_hdr_range_nits);
    const std::array<float,3> luma=gamut==2?std::array<float,3>{.2627f,.6780f,.0593f}:
        gamut==1?std::array<float,3>{.2289746f,.6917385f,.0792869f}:std::array<float,3>{.2126f,.7152f,.0722f};
    const std::array<float,3> zones={options.sdr_shadows,options.sdr_midtones,options.sdr_highlights};
    if(mapper==SdrToneMapper::skiv_manual) {
        tone_map_skiv_manual_inplace(linear,gamut,options.sdr_shoulder,cap,options.sdr_color_mapping,luma,zones,options.sdr_auto_tone_curve);
        return;
    }
    if(skiv_tone_mapper(mapper)) {
        tone_map_skiv_inplace(linear,gamut,mapper==SdrToneMapper::skiv_custom,options.sdr_shoulder,cap);
        return;
    }
    if(!options.sdr_auto_hdr_range&&zones!=std::array<float,3>{})range_nits=cap;
    tone_map_sdr_inplace(linear,options.sdr_shoulder,range_nits,mapper,options.sdr_color_mapping,luma,zones,
                         options.sdr_auto_hdr_range,options.sdr_auto_tone_curve);
}

inline std::vector<float> map_sdr_pixels(const std::vector<float>& linear,int gamut,
                                         float range_nits,const jxrc::Options& options) {
    std::vector<float> out=linear;
    map_sdr_pixels_inplace(out,gamut,range_nits,options);
    return out;
}
