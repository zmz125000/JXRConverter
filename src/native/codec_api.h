#pragma once
#include "hdr_encode.h"
#include "jxrc/converter.hpp"

// Private interfaces between the portable pipeline and extracted codecs.
int convert_source(const uint8_t*,size_t,int,int,const char*,StillEncodeQuality,bool,int*);
int decode_jxl_to_linear_f16(const uint8_t*,size_t,std::vector<uint16_t>&,unsigned&,unsigned&,uhdr_color_gamut_t*,bool*,int*,bool,unsigned,bool,std::vector<uint8_t>*,int*);
int decode_avif_to_linear_f16(const uint8_t*,size_t,std::vector<uint16_t>&,unsigned&,unsigned&,int*,int*,uhdr_color_gamut_t*,unsigned,std::vector<uint8_t>*);
int decode_jxr_memory(const uint8_t*,size_t,std::vector<uint16_t>&,unsigned&,unsigned&,uhdr_color_gamut_t*,unsigned thumbnail_edge = 0);
int encode_still_export(const uint8_t*,size_t,unsigned,unsigned,bool,int,int,bool,const char*,size_t,StillEncodeQuality,bool);
bool raw_jpeg_thumbnail(const uint8_t*,size_t,std::vector<uint8_t>&);
int decode_raw(const uint8_t*,size_t,const jxrc::RawControls&,bool,unsigned,std::vector<uint8_t>&,unsigned&,unsigned&,int&);
int transcode_jpeg(const uint8_t*,size_t,const char*,bool);
std::vector<uint8_t> source_jpeg_icc(const uint8_t*,size_t);
int source_orientation(const uint8_t*,size_t,int,std::vector<uint8_t>*);
void normalize_exif(std::vector<uint8_t>&);
void scale_rgba_f16_max_edge(std::vector<uint16_t>&,unsigned&,unsigned&,unsigned);
void scale_rgba_f32_max_edge(std::vector<float>&,unsigned&,unsigned&,unsigned);
int encode_still_export_f32(const float*,unsigned,unsigned,int,int,const char*,StillEncodeQuality,const float* sdr_base = nullptr);
int encode_png_f32(unsigned,unsigned,const float*,uhdr_color_gamut_t,int,int,const char*,StillEncodeQuality);
struct avifImage;
struct avifRGBImage;
bool complete_avif_color_metadata(avifImage*,const avifRGBImage* = nullptr,bool preserve_hdr_metadata = false);


jxrc::ImageInfo inspect_jxr(const uint8_t*,size_t);

int decode_jxl_to_linear_f32(const uint8_t*,size_t,std::vector<float>&,unsigned&,unsigned&,uhdr_color_gamut_t*,bool*,int*,bool,std::vector<uint8_t>*);
int decode_avif_to_linear_f32(const uint8_t*,size_t,std::vector<float>&,unsigned&,unsigned&,int*,int*,uhdr_color_gamut_t*,std::vector<uint8_t>*);
int decode_jxr_f32(const uint8_t*,size_t,std::vector<float>&,unsigned&,unsigned&,uhdr_color_gamut_t*,bool to_bt2020 = false);
int decode_raw_f32(const uint8_t*,size_t,const jxrc::RawControls&,bool,unsigned,std::vector<float>&,std::vector<float>&,unsigned&,unsigned&,int&);

// Full-range, straight-alpha integer RGB to the same HDR working values as AVIF.
void cicp_rgb_to_linear(const uint8_t*,size_t,unsigned,unsigned,unsigned,std::vector<float>&,uhdr_color_gamut_t&);
void cicp_rgb_to_linear(const uint8_t*,size_t,unsigned,unsigned,unsigned,std::vector<uint16_t>&,uhdr_color_gamut_t&);

bool decode_jxr_scrgb(const uint8_t*,size_t,std::vector<float>&,unsigned&,unsigned&,bool&);
bool is_scrgb_jxl(const uint8_t*,size_t);

bool is_integer_jxl(const uint8_t*,size_t);

int decode_heif_to_linear_f32(const uint8_t*,size_t,std::vector<float>&,unsigned&,unsigned&,uhdr_color_gamut_t*,std::vector<uint8_t>*);
int decode_heif_to_linear_f16(const uint8_t*,size_t,std::vector<uint16_t>&,unsigned&,unsigned&,uhdr_color_gamut_t*,unsigned);
