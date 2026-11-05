#pragma once
#include "codec_api.h"
#include <avif/avif.h>
#include <memory>

// Decode just the gain image to normalized RGB F32 codes. These are gain data,
// not colors: neither CICP/ICC transfer functions nor gain gamma are applied.
// The native resolution is retained, including monochrome replicated to RGB.
bool decode_gain_map_f32(const avifGainMap*,std::vector<float>& rgb,unsigned& width,unsigned& height);

// Reconstruct the highest-headroom rendition directly in linear RGBA F32.
// 1.0 = 203 nits. Optional SDR receives the lowest-headroom rendition in the
// same output gamut. Resampling, gain gamma, offsets and color conversion are
// applied in float; there is no intermediate PQ encoding or half-float raster.
// gain_codes optionally supplies unquantized native-resolution ISO RGB codes
// (for Apple HEIF's nonlinear gain conversion), replacing the integer map.
bool reconstruct_gain_map_f32(const avifImage*,std::vector<float>& rgba,uhdr_color_gamut_t& gamut,
                              std::vector<float>* sdr=nullptr,const std::vector<float>* gain_codes=nullptr);

// Decode a JPEG Ultra HDR base and map into the same integer source container
// used by HEIF and AVIF. No HDR reconstruction or F16 conversion occurs here.
std::unique_ptr<avifImage,decltype(&avifImageDestroy)> decode_jpeg_gain_map(const uint8_t*,size_t);
