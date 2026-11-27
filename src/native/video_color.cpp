#include "video_color.h"
#include "codec_api.h"
#include "render_analysis.h"
#include "sdr_mapping.h"
#include <array>
#include <cstring>

namespace jxrc {
namespace {
template<unsigned GammaTenths>
float power_oetf(float linear) {
    if(!(linear>0.f))return 0.f;
    if(linear>=1.f)return 1.f;
    static constexpr unsigned steps=4096;
    static constexpr float exponent=10.f/GammaTenths;
    static const auto lut=[] {
        std::array<float,steps+1> values{};
        for(unsigned i=0;i<=steps;++i) {
            const float x=float(i)/steps;
            values[i]=std::pow(x*x,exponent);
        }
        return values;
    }();
    const float position=std::sqrt(linear)*steps;
    const unsigned index=std::min(unsigned(position),steps-1);
    if(index<16)return std::pow(linear,exponent);
    return lut[index]+(lut[index+1]-lut[index])*(position-index);
}

skiv::Matrix rgb_to_xyz(unsigned primaries) {
    switch(primaries) {
    case 9:return skiv::rgb2020_xyz;
    case 12:return skiv::rgbp3_xyz;
    case 5:return {{{.43055381,.34154980,.17835231},{.22200431,.70665477,.07134092},{.02018221,.12955337,.93932217}}};
    case 6:return {{{.39352090,.36525808,.19167695},{.21237636,.70105986,.08656378},{.01873909,.11193393,.95838473}}};
    default:return skiv::rgb709_xyz;
    }
}

void render_video_pixels_inplace(const uint8_t* codes,size_t count,unsigned channels,VideoColor source,bool sdr_output,
                                 const Options& options,std::vector<float>& working) {
    uhdr_color_gamut_t gamut;
    cicp_rgb_to_linear(codes,count,16,source.primaries,source.transfer,working,gamut,channels);
    const float exposure=std::exp2(options.exposure);
    const unsigned jobs=hdr_pixel_jobs(count);
    if(exposure!=1.f)hdr_parallel_pixels(count,jobs,[&](size_t begin,size_t end,unsigned) {
        for(size_t i=begin*4;i<end*4;i+=4)for(unsigned c=0;c<3;++c)working[i+c]*=exposure;
    });

    const bool hdr_source=source.transfer==16||source.transfer==18;
    if(hdr_source&&sdr_output&&options.sdr_tone_map) {
        RenderAnalysis analysis;
        analysis.bind(working.data(),count,gamut);
        const float peak=analysis.peak();
        if(options.sdr_tone_mapper==SdrToneMapper::mpcvr) {
            if(peak>1.f)working=tone_map_mpcvr_base(std::move(working),gamut,double(peak)*kSdrWhiteNits);
        } else if(peak>1.f||options.sdr_shadows||options.sdr_midtones||options.sdr_highlights) {
            const float range=peak*kSdrWhiteNits;
            map_sdr_pixels_inplace(working,gamut==UHDR_CG_BT_2100?2:gamut==UHDR_CG_DISPLAY_P3?1:0,
                                   options.sdr_auto_hdr_range?range:std::min(range,float(options.sdr_hdr_range_nits)),options);
        }
    }
}
}

float sdr_video_oetf(float linear,SdrVideoGamma gamma) {
    switch(gamma) {
    case SdrVideoGamma::gamma22:return power_oetf<22>(linear);
    case SdrVideoGamma::gamma24:return power_oetf<24>(linear);
    case SdrVideoGamma::srgb:return srgb_oetf(linear);
    case SdrVideoGamma::automatic:
    case SdrVideoGamma::rec709:return bt709_oetf(linear);
    }
    return bt709_oetf(linear);
}

AnimationFrame render_animation_video_frame(const uint8_t* codes,unsigned width,unsigned height,
                                             VideoColor source,bool sdr,const Options& options) {
    JxrcThreadBudget budget(options.threads);
    AnimationFrame frame;
    frame.width=width;
    frame.height=height;
    frame.primaries=source.primaries;
    render_video_pixels_inplace(codes,size_t(width)*height,4,source,sdr,options,frame.rgba);
    if(source.primaries!=1&&source.primaries!=9&&source.primaries!=12) {
        const auto m=skiv::multiply(skiv::inverse(skiv::rgb709_xyz),rgb_to_xyz(source.primaries));
        const float m00=float(m[0][0]),m01=float(m[0][1]),m02=float(m[0][2]);
        const float m10=float(m[1][0]),m11=float(m[1][1]),m12=float(m[1][2]);
        const float m20=float(m[2][0]),m21=float(m[2][1]),m22=float(m[2][2]);
        const auto count=size_t(width)*height;
        hdr_parallel_pixels(count,hdr_pixel_jobs(count),[&](size_t begin,size_t end,unsigned) {
            for(size_t i=begin*4;i<end*4;i+=4) {
                auto* p=frame.rgba.data()+i;
                const float r=p[0],g=p[1],b=p[2];
                p[0]=m00*r+m01*g+m02*b;
                p[1]=m10*r+m11*g+m12*b;
                p[2]=m20*r+m21*g+m22*b;
            }
        });
        frame.primaries=1;
    }
    return frame;
}

void process_video_pixels(uint8_t* codes,size_t count,VideoColor source,VideoColor output,
                          const Options& options,std::vector<float>& working,unsigned output_bits,SdrVideoGamma gamma) {
    JxrcThreadBudget budget(options.threads);
    const bool sdr_output=output.transfer==1;
    render_video_pixels_inplace(codes,count,3,source,sdr_output,options,working);
    const float* linear=working.data();
    const unsigned jobs=hdr_pixel_jobs(count);
    const bool rematrix=source.primaries!=output.primaries;
    const auto m=rematrix?skiv::multiply(skiv::inverse(rgb_to_xyz(output.primaries)),rgb_to_xyz(source.primaries)):skiv::Matrix{};
    const float m00=float(m[0][0]),m01=float(m[0][1]),m02=float(m[0][2]);
    const float m10=float(m[1][0]),m11=float(m[1][1]),m12=float(m[1][2]);
    const float m20=float(m[2][0]),m21=float(m[2][1]),m22=float(m[2][2]);
    const auto output_gamut=output.primaries==9?UHDR_CG_BT_2100:UHDR_CG_BT_709;
    const bool out8=output_bits==8;
    hdr_parallel_pixels(count,jobs,[&](size_t begin,size_t end,unsigned) {
        if(out8) {
            uint8_t* dst=codes+begin*3;
            for(size_t pixel=begin;pixel<end;++pixel) {
                const size_t i=pixel*4;
                float r=linear[i],g=linear[i+1],b=linear[i+2];
                if(rematrix) {
                    const float nr=m00*r+m01*g+m02*b;
                    const float ng=m10*r+m11*g+m12*b;
                    const float nb=m20*r+m21*g+m22*b;
                    r=nr;g=ng;b=nb;
                }
                if(output.transfer==18)linear_to_hlg_rgb(r,g,b,output_gamut);
                const auto enc=[&](float value) -> uint8_t {
                    const float code=output.transfer==16?linear_to_pq_code(value):sdr_output?sdr_video_oetf(value,gamma):value;
                    return uint8_t(std::clamp(code,0.f,1.f)*255.f+0.5f);
                };
                dst[0]=enc(r);dst[1]=enc(g);dst[2]=enc(b);
                dst+=3;
            }
        } else {
            uint16_t* dst=reinterpret_cast<uint16_t*>(codes)+begin*3;
            for(size_t pixel=begin;pixel<end;++pixel) {
                const size_t i=pixel*4;
                float r=linear[i],g=linear[i+1],b=linear[i+2];
                if(rematrix) {
                    const float nr=m00*r+m01*g+m02*b;
                    const float ng=m10*r+m11*g+m12*b;
                    const float nb=m20*r+m21*g+m22*b;
                    r=nr;g=ng;b=nb;
                }
                if(output.transfer==18)linear_to_hlg_rgb(r,g,b,output_gamut);
                const auto enc=[&](float value) -> uint16_t {
                    const float code=output.transfer==16?linear_to_pq_code(value):sdr_output?sdr_video_oetf(value,gamma):value;
                    return uint16_t(std::clamp(code,0.f,1.f)*65535.f+0.5f);
                };
                dst[0]=enc(r);dst[1]=enc(g);dst[2]=enc(b);
                dst+=3;
            }
        }
    });
}
}
