#pragma once
#include "regular_decode.h"
#include <avif/avif.h>
#include <memory>

struct HeifSource {
    RegularImage base, gain;
    float max_boost = 1, display_ratio = 1;
    const uint8_t* encoded_gain = nullptr; // Borrowed from the input buffer.
    size_t encoded_gain_size = 0;
};
bool is_heif_input(const uint8_t*, size_t);
jxrc::ImageInfo inspect_heif(const uint8_t*, size_t);
HeifSource decode_heif(const uint8_t*, size_t, unsigned preview_edge = 0);
// Lossless RGB packing for the shared source-export and float32 render paths.
std::unique_ptr<avifImage,decltype(&avifImageDestroy)> decode_heif_avif(const uint8_t*,size_t,unsigned preview_edge = 0, const uint8_t** original_map = nullptr, size_t* original_map_size = nullptr);
float apple_gainmap_headroom(const std::vector<uint8_t>& tiff);
