#pragma once

#include "skcms.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Shared BT.2020 profile for the linear working space and encoded SDR output.
inline bool make_bt2020_profile(skcms_ICCProfile* profile,const skcms_TransferFunction* transfer) {
    skcms_Init(profile);
    skcms_SetTransferFunction(profile,transfer);
    skcms_Matrix3x3 matrix{};
    if (!skcms_PrimariesToXYZD50(0.708f, 0.292f, 0.170f, 0.797f, 0.131f,
                                0.046f, 0.3127f, 0.3290f, &matrix)) return false;
    skcms_SetXYZD50(profile, &matrix);
    return skcms_MakeUsableAsDestination(profile);
}

inline bool make_linear_bt2020_profile(skcms_ICCProfile* profile) {
    return make_bt2020_profile(profile,skcms_Identity_TransferFunction());
}

inline bool icc_needs_wide_gamut(const uint8_t* data,size_t size) {
    if(!size)return false;
    skcms_ICCProfile source{};
    if(!skcms_Parse(data,size,&source))return false;
    if(source.data_color_space==skcms_Signature_Gray)return false;
    if(!source.has_toXYZD50||source.has_A2B)return true;
    skcms_Matrix3x3 from_xyz{};
    if(!skcms_Matrix3x3_invert(&skcms_sRGB_profile()->toXYZD50,&from_xyz))return true;
    const auto to_srgb=skcms_Matrix3x3_concat(&from_xyz,&source.toXYZD50);
    // The extrema of each linear channel over the source RGB cube are the
    // sum of its negative/positive coefficients. Allow ICC matrix rounding.
    for(const auto& row:to_srgb.vals) {
        float low=0,high=0;
        for(const auto coefficient:row) {
            low+=std::min(coefficient,0.f);
            high+=std::max(coefficient,0.f);
        }
        if(low<-.005f||high>1.005f)return true;
    }
    return false;
}
