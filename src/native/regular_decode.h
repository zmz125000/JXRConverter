#pragma once
#include "codec_api.h"

// Transfer-encoded, straight-alpha samples. 16-bit samples use native byte order.
// This representation deliberately never passes through an OS bitmap or F16.
struct RegularImage {
    unsigned width = 0, height = 0, bits = 8, orientation = 1;
    std::string format;
    std::vector<uint8_t> pixels, icc, exif;
};
bool is_regular_image(const uint8_t* data, size_t size);
RegularImage decode_regular(const uint8_t* data, size_t size);
void regular_to_linear(const RegularImage&, std::vector<uint16_t>&, uhdr_color_gamut_t&);
void regular_to_linear(const RegularImage&, std::vector<float>&, uhdr_color_gamut_t&);
unsigned regular_orientation(const std::vector<uint8_t>& exif);
int convert_regular_source(const RegularImage&, int target, const char*, StillEncodeQuality, bool keep_exif, int* info);
