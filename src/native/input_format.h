#pragma once
#include <avif/avif.h>
#include <cstddef>
#include <cstdint>

// AVIF, HEIF and Canon CR3 all use ISO BMFF's ftyp box. Check AVIF's
// compatible brands instead of routing every BMFF container to libavif.
inline bool is_avif_input(const uint8_t* data, size_t size) {
    const avifROData input{data,size};
    return avifPeekCompatibleFileType(&input)==AVIF_TRUE;
}
