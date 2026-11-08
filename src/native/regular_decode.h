#pragma once
#include "codec_api.h"
#include <array>

// Straight-alpha raster, except decode_jpeg(native_color=true), which reports
// its native channel count and CMYK flag. Encoded integer samples use pixels (native-endian 16-bit);
// linear floating-point sources use floating_pixels.
// This representation deliberately never passes through an OS bitmap or F16.
struct RegularImage {
    unsigned width = 0, height = 0, bits = 8, orientation = 1;
    unsigned channels = 4;
    bool cmyk = false; // Native JPEG source path only; K is never alpha.
    bool gray = false;
    std::string format;
    unsigned primaries = 1, transfer = 13;
    bool hdr() const { return transfer == 8 || transfer == 16 || transfer == 18; }
    std::vector<uint8_t> pixels, icc, exif;
    // Linear HDR stays F32, including negative and above-white samples.
    std::vector<float> floating_pixels;
    std::array<float,8> chromaticities{.64f,.33f,.30f,.60f,.15f,.06f,.3127f,.3290f};
};
bool is_regular_image(const uint8_t* data, size_t size);
RegularImage decode_regular(const uint8_t* data, size_t size);
// JPEG uses the general libjpeg decoder; Ultra HDR's helper only accepts
// grayscale/RGB images up to 8192 pixels per side.
RegularImage decode_jpeg(const uint8_t*, size_t, bool metadata_only=false, bool native_color=false);
int encode_cmyk_jpeg(unsigned,unsigned,const uint8_t*,const StillSourceColor&,const char*,StillEncodeQuality);
jxrc::ImageInfo inspect_regular(const RegularImage& image);
jxrc::ImageInfo inspect_regular(const uint8_t* data, size_t size);
void regular_to_linear(const RegularImage&, std::vector<uint16_t>&, uhdr_color_gamut_t&);
void regular_to_linear(const RegularImage&, std::vector<float>&, uhdr_color_gamut_t&);
unsigned regular_orientation(const std::vector<uint8_t>& exif);
int convert_regular_source(const RegularImage&, int target, const char*, StillEncodeQuality, bool keep_exif, int* info);

bool is_linear_image(const uint8_t*, size_t);
RegularImage decode_linear_image(const uint8_t*, size_t, bool metadata_only=false);
