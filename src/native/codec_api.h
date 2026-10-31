#pragma once
#include "hdr_encode.h"
#include "jxrc/converter.hpp"

// Private interfaces between the portable pipeline and extracted codecs.
int convert_source(const uint8_t*,size_t,int,int,const char*,StillEncodeQuality,bool,int*);
int decode_jxl_to_linear_f16(const uint8_t*,size_t,std::vector<uint16_t>&,unsigned&,unsigned&,uhdr_color_gamut_t*,bool*,int*,bool,unsigned,bool,std::vector<uint8_t>*,int*);
int decode_avif_to_linear_f16(const uint8_t*,size_t,std::vector<uint16_t>&,unsigned&,unsigned&,int*,int*,uhdr_color_gamut_t*,unsigned,std::vector<uint8_t>*);
int decode_jxr_memory(const uint8_t*,size_t,std::vector<uint16_t>&,unsigned&,unsigned&,uhdr_color_gamut_t*);
int encode_still_export(const uint8_t*,size_t,unsigned,unsigned,bool,int,int,bool,const char*,size_t,StillEncodeQuality,bool);
int decode_raw(const uint8_t*,size_t,const jxrc::RawControls&,bool,unsigned,std::vector<uint8_t>&,unsigned&,unsigned&,int&);
int transcode_jpeg(const uint8_t*,size_t,const char*,bool);
std::vector<uint8_t> source_jpeg_icc(const uint8_t*,size_t);
int source_orientation(const uint8_t*,size_t,int,std::vector<uint8_t>*);
void normalize_exif(std::vector<uint8_t>&);
void scale_rgba_f16_max_edge(std::vector<uint16_t>&,unsigned&,unsigned&,unsigned);

