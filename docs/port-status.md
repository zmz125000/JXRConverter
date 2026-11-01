# Native port status

The reference is LocalViewer commit `e74a70c2811a1e6a80841ea975ba171e219cf306`.

| LocalViewer feature | Native library / CLI |
|---|---|
| Convert to JPEG / Ultra HDR JPEG | Ported |
| Convert to SDR / linear HDR / PQ JXL | Ported |
| Convert to SDR / PQ / ISO gain-map AVIF | Ported |
| Source-encoded JPEG, AVIF, JXL conversion | Ported, with rejection of unsupported preservation |
| JPEG coefficient transcoding to JXL | Explicit option; gain-map sources rejected |
| Lossless/high/medium/low/custom quality | Ported |
| JXL distance, JPEG/AVIF quality, AVIF 10/12 bit | Ported |
| Gain-map downsize | Ported |
| Extended BT.2020 JXL processing | Ported |
| EXIF retention | Preserve paths; processed JPEG/AVIF/JXL |
| RAW demosaic and camera look | Ported via LibRaw |
| RAW exposure, WB presets, Kelvin | Ported |
| RAW highlight stops, shoulder, HDR ceiling | Ported |
| RAW shadows, midtones, highlights, linear HDR | Ported |
| Folder batches and Android save destinations | Desktop batch queue added; Android destinations are not used |
| PNG / WebP / TIFF / BMP / GIF decoders | Portable native decoders added; supported layouts below |
| File browser, thumbnails, metadata filtering/sorting | Qt desktop added |
| Grid/list/details, sizing, multi-selection | Qt desktop added |
| CPU-scaled parallel batch conversion | Added with per-call codec thread budgets |

Preserve mode supports JPEG, AVIF, JXL, PNG, WebP, TIFF, BMP and GIF input. Process mode adds JPEG XR and
LibRaw sensor formats (including DNG and the camera formats enabled by LibRaw).
JPEG ICC, JXL CMS, AVIF transfer/gain-map conversion, and working RGB primaries
use the native pipeline. JPEG/AVIF orientation is applied during processing;
JXL's decoder applies its orientation. AVIF clean-aperture crops are rejected
until portable crop support is added.

Remaining decoder scope: HEIF/HEIC, JPEG 2000, and the
other formats previously supplied by Android. Unsupported inputs fail; they do
not silently export a thumbnail or an SDR substitute. RAW thumbnails first try the embedded camera JPEG through LibRaw, matching
LocalViewer, then use a reduced-size demosaic when the preview is missing or
unreadable. Conversion always demosaics the sensor data; a failed demosaic is
an error. There is no AVIF
linear floating-point target in the reference exporter; JXL is the linear HDR
target. Arbitrary HDR-to-SDR tone-mapping controls are not in the initial port.

Regular raster decoding uses libpng, libwebp, libtiff and stb_image, independently
of Qt and OS bitmap APIs. PNG palette/grayscale/transparency expand to straight
RGBA without gamma correction or premultiplication. PNG/TIFF 16-bit samples stay
integer through preserve-mode JXL encoding; lossless JXL can retain their exact
sample codes, including transparent RGB. JPEG remains lossy, and AVIF quantizes
16-bit inputs to the selected 10/12-bit depth. Process mode now uses straight-alpha linear F32 for JPEG, AVIF, JXL, JXR,
regular rasters and LibRaw's rendered output. Exposure, orientation and resizing
retain that precision; resizing filters premultiplied colors and restores straight
alpha to avoid transparent-edge fringes. JXL receives F32, AVIF quantizes directly
to the selected 10/12-bit codes, and JPEG composites onto black in linear light
before quantizing to 8-bit sRGB codes. RAW's paired SDR base remains linear F32
until encoding, including deep-color AVIF gain-map bases.

Ultra HDR's public codec API supports half-float linear pixels, so JPEG gain-map
decode/encode still crosses that external F16 boundary. LibRaw's demosaic produces
16-bit integer samples; those expand directly to float before camera-look work.
Finite signed and extended-range values remain available for exposure and gamut
conversion. SDR/PQ bounds are applied at output; linear HDR JXL keeps extended
range. These processing conversions are not promises of source-code identity. ICC profiles are retained for preservation
and used for processing; PNG gAMA/cHRM produce an equivalent matrix/TRC profile.
HDR PNG uses explicit cICP PQ/HLG with BT.709, BT.2020 or Display P3 primaries,
RGB matrix and full-range codes. cICP takes precedence over embedded ICC and older
PNG color chunks. Integer codes and straight alpha remain intact for preservation;
JXL can retain every 16-bit code and AVIF uses its selected 10/12-bit depth.
Processing decodes directly to float32 in the source gamut: PQ uses 203-nit white,
and HLG uses the same BT.2100 1000-nit reference-display OOTF as AVIF. Inspection,
filter metadata and native thumbnails recognize these HDR inputs.
PNG/WebP EXIF and raster orientation are supported. Untagged rasters assume sRGB.

TIFF supports unsigned 8/16-bit RGB and black/white grayscale, contiguous or planar,
strips or tiles, opaque or explicitly straight alpha. Associated alpha, palette,
YCbCr/CMYK, floating samples, multipage TIFF and TIFF photographic EXIF retention
are rejected. TIFF-based sensor files continue through LibRaw. Animated PNG/WebP/GIF
and unsupported PNG CICP encodings are rejected. Calibrated BMP requires an embedded
ICC profile; external linked profiles are rejected. Thumbnails use the same native
pipeline, with sRGB conversion only for display.

Processed RAW/JXR EXIF retention is explicitly rejected. Preservation can retain
custom source profiles, but processing supports the color transforms in the
ported native code. File input is capped at 512 MiB and output pixels at 80M;
these are limits, not a strict peak-memory guarantee. Conversion is synchronous;
the desktop now provides batch scheduling and cancellation of pending work.
Active codec operations still run to completion; streaming I/O and in-codec
progress/cancellation remain future API work.

## Verification

The integration suite generates its own linear HDR and SDR fixtures and an
uncompressed Bayer DNG. It exercises the public file-to-file API, all eight
output targets, gain-map container round trips, PQ signaling, linear-light
values/gamut, JPEG byte reconstruction, resize/exposure, RAW controls, invalid
input/options, overwrite protection, destination cleanup, and a real CLI file conversion. Real-camera RAW
fixtures and visual comparisons remain necessary before claiming camera parity.
Linux is validated locally; macOS and Windows are design targets pending builds.

## Remaining work

1. Extend portable decoders to the remaining platform formats, with ICC/EXIF and
   high-bit-depth fixtures rather than relying on 8-bit raster intermediates.
2. Validate real camera RAW files and difficult gain maps against LocalViewer.
3. Validate macOS and Windows builds, including Unicode paths and packaging.
4. Add cancellable jobs and structured codec diagnostics; expose a versioned C
   ABI if needed by GUI frameworks and other languages.
5. Refine the Qt desktop API and platform packaging.

The Qt 6 desktop consumes the same `Options` model and conversion library.
Its interaction tests cover import/deduplication, metadata filtering and numeric
sorting, lazy thumbnails, view switching, box selection, option snapshots,
parallelism, queue cancellation, errors, output collisions, and real conversions.

Canon CR3 is identified separately from AVIF by ISO BMFF file-type brands.
Both EOS R6 Mark III RAW and CRAW samples were opened, thumbnail-unpacked,
sensor-unpacked and processed successfully with the pinned LibRaw 0.22.3.
This checks decoder execution, not color accuracy or camera-look parity.

HDR PNG port validation
-----------------------

The `hdr_png` CTest covers 8/16-bit PQ/HLG in BT.709, BT.2020 and Display P3,
independent numeric transfer/OOTF checks, straight alpha, metadata priority,
inspection, thumbnails, exact lossless JXL samples, preserved JXL/AVIF color tags,
float32 processing, and invalid cICP/CRC rejection. The Linux native build and this
focused test passed.

The existing `conversion` test fails at input mode 0 / target 3 (JPEG to lossless
linear JXL), with encoder error `-67`. The identical failure was reproduced using
the previous 0.1.6 packaged engine and the unchanged pre-port test source; it is
not introduced by HDR PNG support. Windows/macOS were not run locally.

A LocalViewer HLG inconsistency found during the port is documented in
[the port finding](localviewer-hdr-png-note.md).
