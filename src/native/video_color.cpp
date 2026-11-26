#include "video_color.h"
#include "codec_api.h"
#include "render_analysis.h"
#include "sdr_mapping.h"
#include <cstring>

namespace jxrc {
namespace {
skiv::Matrix rgb_to_xyz(unsigned primaries) {
    switch(primaries) {
    case 9:return skiv::rgb2020_xyz;
    case 12:return skiv::rgbp3_xyz;
    case 5:return {{{.43055381,.34154980,.17835231},{.22200431,.70665477,.07134092},{.02018221,.12955337,.93932217}}};
    case 6:return {{{.39352090,.36525808,.19167695},{.21237636,.70105986,.08656378},{.01873909,.11193393,.95838473}}};
    default:return skiv::rgb709_xyz;
    }
}

float bt709_code(float value) {
    const float linear=std::clamp(value,0.f,1.f);
    return linear<.0180539685f?4.5f*linear:1.0992968268f*std::pow(linear,.45f)-.0992968268f;
}
}

void process_video_pixels(uint8_t* codes,size_t count,VideoColor source,VideoColor output,
                          const Options& options,std::vector<float>& working) {
    JxrcThreadBudget budget(options.threads);
    uhdr_color_gamut_t gamut;
    cicp_rgb_to_linear(codes,count,16,source.primaries,source.transfer,working,gamut);
    const float exposure=std::exp2(options.exposure);
    const unsigned jobs=hdr_pixel_jobs(count);
    if(exposure!=1.f)hdr_parallel_pixels(count,jobs,[&](size_t begin,size_t end,unsigned) {
        for(size_t i=begin*4;i<end*4;i+=4)for(unsigned c=0;c<3;++c)working[i+c]*=exposure;
    });

    const bool hdr_source=source.transfer==16||source.transfer==18;
    const bool sdr_output=output.transfer==1;
    std::vector<float> mapped;
    if(hdr_source&&sdr_output&&options.sdr_tone_map) {
        RenderAnalysis analysis;
        analysis.bind(working.data(),count,gamut);
        const float peak=analysis.peak();
        if(options.sdr_tone_mapper==SdrToneMapper::mpcvr) {
            if(peak>1.f)mapped=tone_map_mpcvr_base(working,gamut,double(peak)*kSdrWhiteNits);
        } else if(peak>1.f||options.sdr_shadows||options.sdr_midtones||options.sdr_highlights) {
            const float range=peak*kSdrWhiteNits;
            mapped=map_sdr_pixels(working,gamut==UHDR_CG_BT_2100?2:gamut==UHDR_CG_DISPLAY_P3?1:0,
                                  options.sdr_auto_hdr_range?range:std::min(range,float(options.sdr_hdr_range_nits)),options);
        }
    }
    const auto& linear=mapped.empty()?working:mapped;
    const auto to_xyz=rgb_to_xyz(source.primaries);
    const auto from_xyz=skiv::inverse(rgb_to_xyz(output.primaries));
    const auto output_gamut=output.primaries==9?UHDR_CG_BT_2100:UHDR_CG_BT_709;
    hdr_parallel_pixels(count,jobs,[&](size_t begin,size_t end,unsigned) {
        for(size_t pixel=begin;pixel<end;++pixel) {
            const size_t i=pixel*4;
            skiv::RGB rgb={linear[i],linear[i+1],linear[i+2]};
            if(source.primaries!=output.primaries)rgb=skiv::transform(from_xyz,skiv::transform(to_xyz,rgb));
            float r=float(rgb[0]),g=float(rgb[1]),b=float(rgb[2]);
            if(output.transfer==18)linear_to_hlg_rgb(r,g,b,output_gamut);
            const auto encode=[&](float value) {
                const float code=output.transfer==16?linear_to_pq_code(value):sdr_output?bt709_code(value):value;
                return uint16_t(std::lround(std::clamp(code,0.f,1.f)*65535.f));
            };
            const uint16_t pixel_codes[]={encode(r),encode(g),encode(b),65535};
            std::memcpy(codes+pixel*8,pixel_codes,sizeof(pixel_codes));
        }
    });
}
}
