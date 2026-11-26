#pragma once

#include "jxrc/converter.hpp"

namespace jxrc {

enum class AnimationFormat { gif, webp, png, jxl, avif };
enum class AnimationTransfer { sdr, pq, hlg, linear, scrgb };

struct AnimationFrame {
    unsigned width = 0, height = 0;
    unsigned primaries = 1;
    std::vector<float> rgba; // Straight alpha, display-linear RGB with 203-nit white.
};

struct AnimationImageRenderer {
    static JXRC_API AnimationFrame render(const std::filesystem::path&, const Options&, bool hdr,
                                         const std::function<bool()>& cancelled);
};

struct AnimationEncoding {
    AnimationFormat format;
    AnimationTransfer transfer;
    unsigned width, height, bits, primaries;
    unsigned delay_us;
};

// Render into the output color space before padding. GIF uses this same
// operation, then sends its sRGB RGBA8 frames to FFmpeg for palette encoding.
JXRC_API std::vector<float> compose_animation_frame(AnimationFrame&&, const AnimationEncoding&,
                                                  unsigned horizontal, unsigned vertical, const Options&);
JXRC_API std::vector<uint8_t> animation_rgba8(const std::vector<float>&);

class JXRC_API AnimationEncoder {
public:
    AnimationEncoder(const std::filesystem::path&, const AnimationEncoding&, const Options&);
    ~AnimationEncoder();
    void add(const std::vector<float>&);
    void finish(const std::function<bool()>& cancelled);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
