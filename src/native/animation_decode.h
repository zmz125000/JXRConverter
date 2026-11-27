#pragma once

#include "regular_decode.h"
#include <optional>

struct DecodedAnimationFrame {
    RegularImage image;
    // When set, floating_pixels already contains display-linear working RGB.
    std::optional<uhdr_color_gamut_t> linear_gamut;
};

using AnimationFrameReader = std::function<void(DecodedAnimationFrame&&)>;

bool decode_raster_animation(const uint8_t*, size_t, const AnimationFrameReader&, bool first_only,
                             const std::function<bool()>&);
int decode_jxl_animation(const uint8_t*, size_t, bool bt2020, const AnimationFrameReader&,
                         const std::function<bool()>&);
int decode_avif_animation(const uint8_t*, size_t, const AnimationFrameReader&,
                          const std::function<bool()>&);
jxrc::ImageInfo inspect_gif(const uint8_t*, size_t);
RegularImage decode_gif_patch(const uint8_t*, size_t);
RegularImage webp_metadata(const uint8_t*, size_t);
