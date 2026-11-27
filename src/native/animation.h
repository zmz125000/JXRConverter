#pragma once

#include "jxrc/converter.hpp"

namespace jxrc {

enum class AnimationFormat { gif, webp, png, jxl, avif };
enum class AnimationTransfer { sdr, pq, hlg, linear, scrgb };
enum class AnimationStack { off, first, previous };
enum class AnimationResize { off, fit_up, fit_down, fill_up, fill_down };

struct AnimationPlacement {
    AnimationStack stack = AnimationStack::off;
    AnimationResize resize = AnimationResize::off;
    unsigned horizontal = 1, vertical = 1; // Start, center, end on each axis.
};

struct AnimationFrame {
    unsigned width = 0, height = 0;
    unsigned primaries = 1;
    std::vector<float> rgba; // Straight alpha, display-linear RGB with 203-nit white.
    std::vector<uint8_t> rgba8; // Direct sRGB frame, mutually exclusive with rgba.
};

struct AnimationEncoding;
struct AnimationImageRenderer {
    static JXRC_API AnimationFrame render(const std::filesystem::path&, const Options&, const AnimationEncoding&,
                                         const AnimationPlacement&, const std::function<bool()>& cancelled);
};

struct AnimationEncoding {
    AnimationFormat format;
    AnimationTransfer transfer;
    unsigned width, height, bits, primaries;
    unsigned delay_us;
    bool video = false;
};

// The optional stack stores premultiplied linear pixels in the output gamut.
JXRC_API std::vector<float> compose_animation_frame(AnimationFrame&&, const AnimationEncoding&,
                                                  const AnimationPlacement&, const Options&, std::vector<float>& stack);
JXRC_API std::vector<uint8_t> animation_rgba8(const std::vector<float>&);
JXRC_API std::vector<uint16_t> animation_rgba16(const std::vector<float>&);
JXRC_API std::vector<uint8_t> place_animation_rgba8(AnimationFrame&&, const AnimationEncoding&,
                                                 const AnimationPlacement&, const Options&);
JXRC_API std::vector<uint16_t> animation_bt709_rgba16(const std::vector<uint8_t>&);
JXRC_API void clear_gif_canvas_between_frames(const std::filesystem::path&, const std::function<bool()>& cancelled);

class JXRC_API AnimationEncoder {
public:
    AnimationEncoder(const std::filesystem::path&, const AnimationEncoding&, const Options&);
    ~AnimationEncoder();
    void add(const std::vector<float>&);
    void add(const std::vector<uint8_t>&);
    void finish(const std::function<bool()>& cancelled);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
