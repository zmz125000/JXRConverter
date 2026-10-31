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
| Folder batches and Android save destinations | Not ported; file-to-file API first |
| Android platform format decoders | Need portable replacements |

Preserve mode supports JPEG, AVIF, and JXL input. Process mode adds JPEG XR and
LibRaw sensor formats (including DNG and the camera formats enabled by LibRaw).
JPEG ICC, JXL CMS, AVIF transfer/gain-map conversion, and working RGB primaries
use the native pipeline. JPEG/AVIF orientation is applied during processing;
JXL's decoder applies its orientation. AVIF clean-aperture crops are rejected
until portable crop support is added.

Remaining decoder scope: PNG, WebP, TIFF raster, HEIF/HEIC, JPEG 2000, and the
other formats previously supplied by Android. Unsupported inputs fail; they do
not silently export a thumbnail or an SDR substitute. RAW has no embedded-preview
fallback in this library, so a failed demosaic is an error. There is no AVIF
linear floating-point target in the reference exporter; JXL is the linear HDR
target. Arbitrary HDR-to-SDR tone-mapping controls are not in the initial port.

Processed RAW/JXR EXIF retention is explicitly rejected. Preservation can retain
custom source profiles, but processing supports the color transforms in the
ported native code. File input is capped at 512 MiB and output pixels at 80M;
these are limits, not a strict peak-memory guarantee. Conversion is synchronous;
progress, cancellation, streaming I/O, and batch scheduling are future API work.

## Verification

The integration suite generates its own linear HDR and SDR fixtures and an
uncompressed Bayer DNG. It exercises the public file-to-file API, all eight
output targets, gain-map container round trips, PQ signaling, linear-light
values/gamut, JPEG byte reconstruction, resize/exposure, RAW controls, invalid
input/options, overwrite protection, destination cleanup, and a real CLI file conversion. Real-camera RAW
fixtures and visual comparisons remain necessary before claiming camera parity.
Linux is validated locally; macOS and Windows are design targets pending builds.

## Library-first next steps

1. Add portable decoders for the remaining platform formats, with ICC/EXIF and
   high-bit-depth fixtures rather than relying on 8-bit raster intermediates.
2. Validate real camera RAW files and difficult gain maps against LocalViewer.
3. Validate macOS and Windows builds, including Unicode paths and packaging.
4. Add cancellable jobs and structured codec diagnostics; expose a versioned C
   ABI if needed by GUI frameworks and other languages.
5. Freeze the conversion API and packaging before choosing a GUI framework.

A future GUI should consume the same `Options` model and conversion library.
GUI implementation and framework selection are deferred.
