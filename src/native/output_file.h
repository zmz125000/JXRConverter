#pragma once

#include <cstdio>
#ifdef _WIN32
#include <filesystem>
#endif

// The converter passes UTF-8 paths to the codec writers.
inline std::FILE* open_output_file(const char* path) {
#ifdef _WIN32
    return _wfopen(std::filesystem::u8path(path).c_str(), L"wb");
#else
    return std::fopen(path, "wb");
#endif
}
