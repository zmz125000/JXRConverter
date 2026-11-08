# Conversion matrix

This matrix describes the native converter and desktop app. “Direct” means the
converter can carry the encoded image or gain map across without rendering the
pixels; codec or container conversion can still alter samples. Processed paths
decode to linear-light float32 pixels, except where a direct path applies.

## Input classes

| Input class | Common file formats | What the app reads |
|---|---|---|
| SDR raster | JPEG, PNG, WebP, TIFF, BMP, GIF, SDR AVIF/HEIF, SDR JXL | Ordinary SDR pixels and color profile/transfer metadata. Raster layouts are subject to the decoder limits in [port status](port-status.md). |
| Gain-map HDR | Ultra HDR JPEG, gain-map AVIF, Apple/ProXDR HEIC | SDR base plus auxiliary gain map. Plain JPEG/AVIF export can retain both directly when exposure is 0 EV and resizing is off. |
| PQ/HLG HDR | PQ/HLG AVIF/HEIC, HDR PNG, PQ/HLG JXL | HDR transfer and primaries; processing linearizes into float32. HDR PNG supports full-range cICP PQ/HLG in BT.709, BT.2020 or Display P3. |
| Linear HDR | OpenEXR (`.exr`), Radiance (`.hdr/.rgbe/.xyze/.pic`), PFM, floating-point/LogLuv TIFF, linear JXL/AVIF/HEIC; scRGB JPEG XR (JXR) | Linear-light samples, including extended-range values. JXR processing is supported; source-preserving JXR export is limited to linear JXL. |
| scRGB | scRGB JXL and scRGB JXR | Linear BT.709/scRGB values, including negative and above-white channels where present. scRGB output is JXL only. |
| Camera RAW | DNG and camera formats supported by LibRaw | Sensor data is demosaiced for conversion. RAW uses separate camera-look/white-balance/exposure and HDR rendering controls. |

EXR, Radiance, PFM and floating-point TIFF use Process pixels mode and share the
same prepared F32 render across outputs. Untagged linear RGB uses BT.709;
EXR chromaticities and Radiance PRIMARIES override that default (Radiance without
PRIMARIES uses its specified standard primaries). Relative linear white is 1 =
203 nits. TIFF STONITS, when supplied, converts absolute luminance into that
working convention. Scene-referred formats do not imply a universal exposure.

Formats overlap: JPEG, AVIF, HEIF and JXL may be SDR or HDR, so transfer/gain-map
metadata—not the extension alone—determines the input class.

Unedited CMYK/YCCK JPEG sources retain CMYK samples and their ICC profile in
JPEG output, and in JXL output when an embedded CMYK ICC profile is available.
CMYK JXL uses level 10 and a black extra channel; compatible JXL-to-JXL/JPEG
exports retain that channel. These paths avoid RGB/linear-light conversion.
JPEG is still re-encoded at the selected quality. PNG/AVIF and pixel edits need
RGB; CMYK ICC transforms apply there, with a conventional CMYK fallback for
untagged JPEG. Untagged CMYK-to-JXL also uses that RGB fallback.
Grayscale JPEG/PNG/TIFF retain grayscale in compatible unedited outputs.
JPEG coefficient transcoding does not support CMYK; disable that separate
option to use the native CMYK sample path.

## Output representations

| Output representation | File targets | Encoding and caveats |
|---|---|---|
| SDR | JPEG (`jpeg`), JXL (`jxl`), AVIF (`avif`) | Processed SDR output is clipped to SDR. JPEG is 8-bit and lossy; AVIF supports up to 12-bit; JXL supports integer and floating-point encodings depending on target/path. No general HDR-to-SDR tone mapper is selectable. |
| SDR base + HDR gain map | Ultra HDR JPEG (`jpeg-gainmap`), gain-map AVIF (`avif-gainmap`) | ISO/Ultra HDR gain-map output. A gain map can be transferred directly from compatible JPEG/AVIF/HEIF sources at 0 EV without resizing. Otherwise the app renders HDR and builds a new map; SDR inputs do not gain real highlight detail. |
| Linear HDR | Linear JXL (`jxl-linear`) | Float32 linear-light processing output; preserves extended-range values. JXL may encode float samples as float32 or half-float according to encoder settings. No AVIF linear target. |
| scRGB | scRGB JXL (`jxl-scrgb`) | Linear BT.709/scRGB with 1.0 representing 80-nit scRGB reference white in JXL signaling. Existing scRGB JXL can take a direct path when no pixel edits are requested. |
| PQ HDR | PQ JXL (`jxl-pq`), PQ AVIF (`avif-pq`) | Processed HDR rendered and encoded with PQ transfer signaling. PQ transfer conversion requires Process mode. AVIF uses selected 10/12-bit depth; JXL uses integer HDR codes. |

## Input-to-output support

Legend: **Direct** = eligible source/container path; **Render** = decode/process/encode;
**Render*** = available through processing, with the noted source-specific behavior;
**No** = unsupported or rejected.

| Input ↓ / Output → | SDR JPEG / AVIF / JXL | Gain-map JPEG / AVIF | Linear JXL | scRGB JXL | PQ JXL / AVIF |
|---|---|---|---|---|---|
| SDR raster / SDR JPEG, AVIF, HEIF, JXL | Direct where supported; otherwise render | Render* | Render | Render | Render |
| Gain-map JPEG / AVIF / HEIC | JPEG/AVIF: direct at 0 EV, no resize; otherwise render. JXL preservation rejects gain maps | JPEG/AVIF: direct at 0 EV, no resize; otherwise render | Render | Render | Render |
| PQ/HLG AVIF, PNG, JXL | Preserve source transfer where the selected preserve target allows it; processed SDR clips | Render | Render | Render | Render; PQ targets convert transfer to PQ |
| Linear JXL | Render or source-preserve to JXL | Render | JXL direct when eligible; otherwise render | Render | Render |
| scRGB JXL / JXR | Render | Render | Render (JXR preserve is not available) | scRGB JXL can be direct; JXR scRGB is rendered | Render |
| RAW | No in Preserve mode; render in Process mode | Render with paired SDR camera-look base | Render | Render | Render |

“Direct” depends on mode, source type, output target, exposure, resize, depth and
codec support. Preserve mode retains source encoding when possible and rejects
unsupported preservation instead of silently rendering. Process mode normally
uses float32 linear pixels; eligible SDR/gain-map and JXL paths may still bypass
rendering when there are no pixel edits.

## Processing paths and controls

| Path or control | Behavior |
|---|---|
| General pixel processing | Straight-alpha float32 linear-light RGB; exposure is applied in stops; resize filters premultiplied colors to avoid transparent-edge fringes. |
| Preserve source encoding | Avoids linearization and HDR reconstruction. Container/codec conversion may still quantize or convert RGB/YUV and bit depth. |
| SDR JPEG → JXL coefficient transcode | Optional `jpeg-transcoding` path for eligible SDR JPEG to JXL without exposure/resizing; stores JPEG coefficients. Gain-map input is rejected. |
| Gain-map → JPEG/AVIF direct | Retains the SDR base and map at 0 EV without resizing, including Process mode. Exposure or resizing invokes float32 HDR rendering and creates a new output map if a gain-map target is selected. |
| scRGB JXL → scRGB JXL direct | Keeps the encoded scRGB samples when eligible and no pixel edits are requested. Processed scRGB can convert gamut to BT.2020 by default for wider viewer compatibility; this is configurable. |
| RAW → SDR | Demosaic and render with camera look, exposure and white balance; processed SDR clips to SDR. |
| RAW → HDR | Demosaic and render using source sensor headroom (up to 10,000 nits), boost mode (default target 1,500 nits), or custom controls. Linear HDR and camera-look rendering are selectable. Gain-map output retains a separate SDR camera-look base. |
| Gamut conversion | Processing uses source primaries and can convert scRGB to BT.2020; extended BT.2020 handling is enabled by default for JXL decoding. |
| Metadata | EXIF can be retained for supported photographic inputs and JPEG/JXL/AVIF outputs. Source orientation/dimensions are updated to match exported pixels. Preservation can retain color profiles; processing uses supported color transforms. |

For exact decoder layouts, preservation restrictions and RAW notes, see
[native port status](port-status.md). The CLI target names and options are listed
in the [README](../README.md#convert).
