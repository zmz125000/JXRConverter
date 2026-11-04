#pragma once
#include "hdr_encode.h"
#include "hdr_parallel.h"
#include <array>
#include <stdexcept>

// The decoder's immutable PQ LUT (linear RGB, 1 = 203 nits).
const std::vector<float>& pq_linear_lut();

struct ContentLightLevels { double peak=0.,average=0.; };

// One accumulator per encoding worker. A monotonic PQ transfer means that
// quantize(max(R,G,B)) equals max(quantize(R),quantize(G),quantize(B)).
// Reuse the already calculated PQ code to measure every supported output depth.
struct PqAnalysisAccumulator {
    static constexpr std::array<unsigned,4> depths={8,10,12,16};
    float raw_peak=0.f;
    size_t count=0;
    std::array<double,4> peaks{},sums{};
    void observe(float raw,float code) {
        raw_peak=std::max(raw_peak,raw);++count;
        const auto& lut=pq_linear_lut();
        for(size_t d=0;d<depths.size();++d) {
            const unsigned maximum=(1u<<depths[d])-1;
            const unsigned packed=unsigned(std::lround(code*maximum));
            const unsigned full=(packed*65535u+maximum/2)/maximum;
            const double nits=std::clamp(double(lut[full])*kSdrWhiteNits,0.,double(kMaxNits));
            peaks[d]=std::max(peaks[d],nits);sums[d]+=nits;
        }
    }
};

// Borrowed from one immutable prepared render, owned by ConversionSession.
// Only scalar metadata is retained; no additional full-frame pixel buffers.
class RenderAnalysis {
    const float* pixels_=nullptr;
    size_t count_=0;
    uhdr_color_gamut_t gamut_=UHDR_CG_BT_709;
    bool peak_ready_=false,pq_ready_=false;
    float peak_=0.f;
    std::array<ContentLightLevels,4> pq_{},hlg_{};
    std::array<bool,4> hlg_ready_{};
    unsigned pq_builds_=0;
    static size_t index(unsigned depth) {
        for(size_t i=0;i<PqAnalysisAccumulator::depths.size();++i)
            if(PqAnalysisAccumulator::depths[i]==depth)return i;
        throw std::invalid_argument("Unsupported content-light bit depth");
    }
public:
    void bind(const float* pixels,size_t count,uhdr_color_gamut_t gamut) {
        *this=RenderAnalysis{};pixels_=pixels;count_=count;gamut_=gamut;
    }
    bool matches(const float* pixels,size_t count,uhdr_color_gamut_t gamut) const {
        return pixels_==pixels&&count_==count&&gamut_==gamut;
    }
    float peak() {
        if(!peak_ready_) {
            std::array<float,8> peaks{};
            hdr_parallel_pixels(count_,hdr_pixel_jobs(count_),[&](size_t begin,size_t end,unsigned job){
                float found=0.f;
                for(size_t i=begin;i<end;++i)for(unsigned c=0;c<3;++c) {
                    const float v=pixels_[i*4+c];if(std::isfinite(v))found=std::max(found,v);
                }
                peaks[job]=found;
            });
            peak_=*std::max_element(peaks.begin(),peaks.end());
            peak_ready_=true;
        }
        return peak_;
    }
    bool pq_ready() const {return pq_ready_;}
    unsigned pq_builds() const {return pq_builds_;}
    void record_pq(const std::array<PqAnalysisAccumulator,8>& workers) {
        if(pq_ready_)return;
        size_t count=0;
        for(const auto& worker:workers) {
            peak_=std::max(peak_,worker.raw_peak);count+=worker.count;
            for(size_t d=0;d<pq_.size();++d) {
                pq_[d].peak=std::max(pq_[d].peak,worker.peaks[d]);pq_[d].average+=worker.sums[d];
            }
        }
        for(auto& light:pq_)light.average/=double(count);
        peak_ready_=pq_ready_=true;++pq_builds_;
    }
    const ContentLightLevels& pq_levels(unsigned depth) {
        if(!pq_ready_) {
            std::array<PqAnalysisAccumulator,8> workers{};
            hdr_parallel_pixels(count_,hdr_pixel_jobs(count_),[&](size_t begin,size_t end,unsigned job){
                for(size_t i=begin;i<end;++i) {
                    float raw=0.f;
                    for(unsigned c=0;c<3;++c)if(std::isfinite(pixels_[i*4+c]))raw=std::max(raw,pixels_[i*4+c]);
                    workers[job].observe(raw,linear_to_pq_code(std::min(raw,kMaxLinear)));
                }
            });
            record_pq(workers);
        }
        return pq_[index(depth)];
    }
    const ContentLightLevels* cached_light(unsigned transfer,unsigned depth) const {
        const auto d=index(depth);
        if(transfer==16)return pq_ready_?&pq_[d]:nullptr;
        return transfer==18&&hlg_ready_[d]?&hlg_[d]:nullptr;
    }
    void record_hlg(unsigned depth,ContentLightLevels light) {const auto d=index(depth);hlg_[d]=light;hlg_ready_[d]=true;}
};
