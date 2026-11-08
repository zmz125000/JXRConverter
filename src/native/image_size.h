#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

// The largest in-memory working representation is RGBA float32. This checks
// arithmetic and addressability, not an arbitrary image-resolution ceiling.
inline bool fits_working_image(uint64_t width,uint64_t height) {
    return width && height && width<=std::numeric_limits<size_t>::max()/16/height;
}
