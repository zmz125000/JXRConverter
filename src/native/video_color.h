#pragma once
#include "jxrc/converter.hpp"
#include <cstddef>

namespace jxrc {
struct VideoColor {
    unsigned primaries, transfer; // CICP identifiers
};

// Full-range, native-endian RGBA16 frames from the video decoder. Reuse the
// float buffer between frames, and write the encoder's RGB codes in place.
JXRC_API void process_video_pixels(uint8_t* codes,size_t count,VideoColor source,VideoColor output,
                                  const Options&,std::vector<float>& working);
}
