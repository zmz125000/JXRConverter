#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jxrc {
enum class Mode { preserve, process };
// AVIF linear is deliberately absent: LocalViewer exports SDR, PQ or ISO gain maps.
enum class Target { jpeg, jpeg_gainmap, jxl, jxl_linear, avif, avif_pq, avif_gainmap, jxl_pq };
enum class Preset { lossless, high, medium, low, custom };
struct Quality {
    Preset preset = Preset::high;
    float jxl_distance = 0.5f;
    int codec_quality = 90;
    int avif_depth = 10;
    bool downsize_gainmap = false;
};
enum class RawHdrMode { source, boost, custom };
struct RawControls {
    RawHdrMode hdr_mode = RawHdrMode::source;
    float exposure = 0;
    int white_balance = 0; // camera, auto, daylight, cloudy, shade, tungsten, fluorescent, flash, kelvin
    int kelvin = 5200;
    float highlight_stops = 0;
    float shoulder = 0.5f;
    int target_nits = 1500;
    float shadows = 0, midtones = 0, highlights = 0;
    bool camera_look = true;
    bool linear_hdr = false;
};
struct Options {
    Mode mode = Mode::preserve;
    Target target = Target::avif;
    Quality quality;
    RawControls raw;
    float exposure = 0; // Process-mode linear-light exposure, in stops.
    unsigned max_edge = 0;
    bool jpeg_transcoding = false; // JPEG coefficients to JXL; gain-map JPEG is rejected.
    bool keep_exif = false;
    bool overwrite = false;
    bool extended_bt2020 = true;
};
struct Result {
    unsigned width = 0, height = 0;
    bool gainmap = false;
    int transfer = 0;
    std::string encoding;
};
// Throws std::invalid_argument for invalid controls, std::runtime_error for codec/I/O errors.
// Options are captured per call. Output is staged beside the destination before publication.
Result convert(const std::filesystem::path& input, const std::filesystem::path& output, const Options& options = {});
void validate(const Options& options);
std::string version();
} // namespace jxrc
