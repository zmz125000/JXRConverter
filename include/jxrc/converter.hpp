#pragma once
#include "export.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <memory>
#include <vector>

namespace jxrc {
enum class Mode { preserve, process };
enum class GainMapReconstruction { off, automatic, full_image };
// Append targets to retain saved GUI target identifiers.
enum class Target { jpeg, jpeg_gainmap, jxl, jxl_linear, avif, avif_pq, avif_gainmap, jxl_pq, jxl_scrgb, png, png_hdr, png_hlg, jxl_hlg, avif_hlg };
enum class Preset { lossless, high, medium, low, custom };
struct Quality {
    Preset preset = Preset::high;
    float jxl_distance = 0.5f;
    int codec_quality = 90;
    int avif_depth = 10; // Custom maximum depth; source cap excludes full gain-map reconstruction.
    int png_depth = 16; // PNG controls are independent of preset.
    bool auto_bit_depth = true; // PNG: 8-bit sources use 8; deeper/reconstructed sources use 16.
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
    bool auto_exposure = false; // Meter scene-linear pixels before the camera look; exposure adds compensation.
    bool auto_exposure_protect_highlights = true; // Limit automatic brightening using RGB highlights.
};
struct Options {
    Mode mode = Mode::preserve;
    Target target = Target::avif;
    Quality quality;
    RawControls raw;
    float exposure = 0; // Non-RAW process-mode linear-light exposure, in stops. RAW uses raw.exposure.
    unsigned max_edge = 0;
    unsigned threads = 0; // Per-conversion codec budget; 0 uses automatic defaults.
    bool jpeg_transcoding = false; // Eligible SDR JPEG coefficients to JXL, without exposure/resizing.
    bool keep_exif = false;
    bool overwrite = false;
    bool srgb_clamp = false; // Process mode: constrain output gamut to sRGB/BT.709; retain HDR transfer/headroom.
    bool extended_bt2020 = true;
    bool convert_gamut = true; // Process mode: convert scRGB to BT.2020 for wider viewer compatibility.
    // Gain-map inputs only: transfer source, repair source, or reconstruct F32.
    // Ordinary SDR/HDR sources keep their normal conversion behavior.
    GainMapReconstruction gain_map_reconstruction = GainMapReconstruction::automatic;
};
struct Result {
    unsigned width = 0, height = 0;
    bool gainmap = false;
    int transfer = 0;
    std::string encoding;
    bool skipped = false; // Existing output with overwrite disabled; no image work performed.
};
// Throws std::invalid_argument for invalid controls, std::runtime_error for codec/I/O errors.
// Options are captured per call. Output is staged beside the destination before publication.
JXRC_API Result convert(const std::filesystem::path& input, const std::filesystem::path& output, const Options& options = {});
// A batch-scoped, thread-confined source. Compatible outputs share immutable
// float32 pixels; changed processing controls invalidate the prepared render.
class JXRC_API ConversionSession {
public:
    explicit ConversionSession(const std::filesystem::path& input);
    ~ConversionSession();
    ConversionSession(const ConversionSession&) = delete;
    ConversionSession& operator=(const ConversionSession&) = delete;
    Result convert(const std::filesystem::path& output, const Options& options = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct ImageInfo {
    unsigned width = 0, height = 0, bits = 0;
    bool raw = false, gainmap = false, hdr = false;
    std::string format, transfer, primaries;
};
struct Preview {
    unsigned width = 0, height = 0;
    std::vector<uint8_t> rgba; // sRGB display thumbnail, straight alpha; not an export intermediate.
};
JXRC_API ImageInfo inspect(const std::filesystem::path& input);
// Returning true cancels before opening the file or between 256 KiB reads.
JXRC_API ImageInfo inspect(const std::filesystem::path& input, const std::function<bool()>& cancelled);
JXRC_API ImageInfo inspect_bytes(const std::vector<uint8_t>& input);
// Display previews are capped at 768 px even when a larger edge is requested.
JXRC_API Preview preview(const std::filesystem::path& input, unsigned max_edge = 384);
// As above; codec calls already running finish before cancellation is checked.
JXRC_API Preview preview(const std::filesystem::path& input, unsigned max_edge, const std::function<bool()>& cancelled);
// Share already loaded bytes with metadata readers. source_info describes the
// original image, before thumbnail resizing, and is optional.
JXRC_API Preview preview_bytes(const std::vector<uint8_t>& input, unsigned max_edge = 384,
                              ImageInfo* source_info = nullptr, const std::function<bool()>& cancelled = {});
JXRC_API void validate(const Options& options);
JXRC_API std::string version();
} // namespace jxrc
