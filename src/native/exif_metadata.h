#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
// Standalone TIFF EXIF, excluding source raster offsets and maker notes.
std::vector<uint8_t> portable_source_exif(const uint8_t*,size_t,unsigned,unsigned,unsigned orientation=1);
void prepare_export_exif(std::vector<uint8_t>&,unsigned,unsigned);
