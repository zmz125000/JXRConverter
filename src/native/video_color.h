#pragma once
#include "animation.h"
#include <cstddef>

namespace jxrc {
struct VideoColor {
    unsigned primaries, transfer; // CICP identifiers
};

JXRC_API AnimationFrame render_animation_video_frame(const uint8_t* codes, unsigned width, unsigned height,
                                                     VideoColor source, bool sdr, const Options&);

// Full-range, native-endian RGB16 frames from the video decoder. Reuse the
// float buffer between frames, and write the encoder's RGB codes in place.
JXRC_API void process_video_pixels(uint8_t* codes,size_t count,VideoColor source,VideoColor output,
                                  const Options&,std::vector<float>& working);
}
