# Dependency releases

Release audit: 2026-10-31. Source dependencies use explicit stable release tags,
not moving branches. JPEG is fetched separately so an older Ultra HDR vendored
copy cannot override the selected release.

| Dependency | Release | Upstream |
|---|---|---|
| Ultra HDR | 2.0.2 | [Release](https://github.com/google/libultrahdr/releases/tag/v2.0.2) |
| libjpeg-turbo | 3.2.1 | [Release](https://github.com/libjpeg-turbo/libjpeg-turbo/releases/tag/3.2.1) |
| libjxl | 0.12.0 | [Release](https://github.com/libjxl/libjxl/releases/tag/v0.12.0) |
| libaom | 3.15.3 | [Tag](https://aomedia.googlesource.com/aom/+/refs/tags/v3.15.3) |
| libavif | 1.4.2 | [Release](https://github.com/AOMediaCodec/libavif/releases/tag/v1.4.2) |
| LibRaw | 0.22.3 | [Release](https://github.com/LibRaw/LibRaw/releases/tag/0.22.3) |
| JPEG XR | 0.3.1 | [Tag](https://github.com/bvibber/jpegxr/tree/0.3.1) |
| Exiv2 | 0.28.9 | [Release](https://github.com/Exiv2/exiv2/releases/tag/v0.28.9) |
| Qt (current-release GUI CI) | 6.12.0 | [Official release](https://download.qt.io/official_releases/qt/6.12/6.12.0/) |

JPEG XR has no newer release tag. The existing Ultra HDR, libjxl and libavif
pins were already current. libjxl's Brotli, Highway and skcms submodules retain
the revisions selected by its release. Exiv2 reuses that Brotli decoder instead
of building another copy. Exiv2's unused video/network/XMP/PNG/INI integrations
are disabled; EXIF, BMFF and compressed JXL metadata remain enabled.

The normal desktop build fetches Exiv2. `JXRC_USE_SYSTEM_EXIV2=ON` retains support
for distribution packages, including Ubuntu 24.04's Exiv2 0.27.6. Qt's supported
minimum remains 6.4; the current-release CI job installs Qt 6.12.0 and its ICU
runtime, while a separate job exercises Ubuntu's older Qt and Exiv2 packages.

GitHub Actions use checkout 7.0.1, msvc-dev-cmd 1.13.0 and install-qt-action
4.5.0. Build tools installed from the runners' package repositories use the
versions supplied by those repositories.

## Integration fixes

Ultra HDR's ordinary `core` target carries its JPEG and image-IO link
requirements transitively. Linking it avoids both its MSVC-specific wrapper
name and the nested JPEG archive rejected by the macOS linker. Windows builds
use a consistent DLL CRT, `LIBRAW_NODLL` for static LibRaw, and `NOMINMAX`.
The workflow explicitly exports NASM's installed directory to later steps.

The Ultra HDR CPU-budget adaptation still applies to a generated build-tree
copy; it does not modify downloaded sources. LocalViewer source caches are
reused only if their HEAD matches the requested release tag, so dependency
updates cannot silently keep an older cached release.
