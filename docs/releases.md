# Versions, CI downloads and releases

To build a desktop archive locally (Qt 6.10+ required for runtime deployment):

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DJXRC_BUILD_GUI=ON -DJXRC_DEPLOY_QT=ON -DCMAKE_INSTALL_LIBDIR=lib
cmake --build build-release --target jxrconverter jxrconvert -j 4
(cd build-release && cpack -C Release)
python3 tools/smoke_package.py build-release
```

After committing the version and changes, push a matching release tag:

```sh
git tag v0.1.1
git push origin v0.1.1
```

The output panel can create format subfolders (`avif/`, `jxl/`, `jpg/`), save
beside each original image, and rename conflicting outputs with a number.
Settings are stored in `config/settings.ini` and recent-file/folder history in
`config/history.json` beside the executable (beside the `.app` on macOS).
Keep the app folder writable; updates preserve this folder.
These settings are persistent. Rename disables replacement of existing files.
RAW outputs rendered with Boost highlights or Custom include `_hdr_boost` or
`_hdr_custom` before the target suffix. RAW thumbnails prefer the camera's
embedded JPEG, falling back to a reduced-size demosaic when it is unavailable.

Pixel processing uses float32 through exposure, orientation, resizing and JXL
encoding. AVIF quantizes directly to its selected bit depth; JPEG composites
alpha onto black in linear light before encoding. RAW gain-map SDR bases retain
float precision until encoding. JPEG gain-map decoding and encoding use F16 only
at the Ultra HDR API boundary, and LibRaw supplies 16-bit demosaiced samples.

Release packages use one shared `jxrc` codec engine for the GUI and CLI. Keep the
whole extracted folder together when using the CLI, including its libraries.
On macOS the GUI app can be moved separately; its updater and licenses are inside
the app bundle. Windows packages carry
the required MSVC runtime DLLs directly and need no bundled VC installer.

The **Update** button checks stable releases in `zmz125000/JXRConverter`, downloads
the matching OS/CPU desktop archive, verifies its SHA-256 checksum, extracts it,
and restarts after installation. Updates require a writable extracted release
folder and the system `tar` tool (built into current Windows/macOS and standard
Linux installations). Conversion must finish before updating. Replacement errors
restore the prior files and are reported on relaunch. The optional package manifest tracks
obsolete program files. Older installations can update without it; unrelated
files are retained when no inventory is available. User images and settings are
preserved. Forks can set
`JXRC_UPDATE_REPOSITORY=owner/repository` at configure time.

Before `cpack`, run `python tools/prepare_release_sources.py build` to collect
runtime licenses and source references. Releases publish the desktop packages and
`SHA256SUMS.txt`. See [redistribution](redistribution.md) and the
[package audit](release-audit.md).

Multiple outputs share one float32 render per source in Process pixels mode.
The queue runs different source files in parallel and encodes each source's targets
in sequence, keeping prepared-image memory bounded by the worker count. RAW SDR,
HDR and HDR with a separate SDR base require different renders; targets within
each family share their render. The native `ConversionSession` API exposes the
same reuse to library callers and snapshots a source for the session's lifetime.
Changed processing controls invalidate the prepared pixels.

Processing remains float32 until encoding. PQ JXL stores 16-bit integer codes.
The **JXL · scRGB HDR** output target (CLI `--target jxl-scrgb`) uses
linear BT.709 primaries and the scRGB 80-nit unit. Unchanged scRGB sources take
the direct path; other sources and edited pixels convert to scRGB in float32,
retaining signed values and HDR headroom. It can be selected alongside regular
linear HDR JXL; the outputs use distinct `_scrgb.jxl` and `_linear.jxl` names.
The scRGB-to-BT.2020 toggle defaults to on and still controls other outputs.
For scRGB JXR or scRGB JXL to linear HDR JXL, zero exposure, original size and
"Convert scRGB gamut to BT.2020" disabled select a direct sample-preserving path. Half-float
JXR stays half-float, float32 stays float32, and fixed-point expands exactly to
float32. The path retains signed/extended RGB, alpha and the scRGB 80-nit unit,
without BT.2020 conversion or HDR normalization. All presets (Lossless, High,
Medium, Low and Custom) use this path. Select Lossless for exact samples; the other
presets apply their JXL compression distance to the same scRGB input. Reader support for extended scRGB
JXL varies. Exposure, resizing and PQ select float32 processing; processed linear JXL stays
float32. "Convert scRGB gamut to BT.2020" is enabled by default in Process pixels
mode, converting scRGB to BT.2020 before encoding. Disable it (CLI
`--no-convert-gamut`) to keep scRGB for untouched linear JXL and BT.709 primaries
for other outputs. Preserve mode retains source primaries independently of this
processing control.
The scRGB JXR direct path also supports Preserve mode with JXL output.
AVIF depth is a maximum: 8-bit sources remain 8-bit, with higher-depth sources
capped by the preset or custom 10/12-bit choice. AVIF supports 8/10/12-bit files;
its uint16 RGB encoder input buffer does not imply a 16-bit AVIF file.

Existing output files are skipped before decoding or encoding when replacement
is disabled. The queue reports skipped outputs separately. Rename-if-conflict
chooses a new name as before; replacement still requires the overwrite toggle.

SDR JXL uses integer samples: untouched SDR JPEG, regular raster, AVIF and integer
JXL inputs use the source-preserving integer path even in Process pixels mode.
When exposure, resizing or HDR-to-SDR conversion requires processing, the working
buffer stays float32 and the encoder writes sRGB-transfer integer pixels: 8-bit
for sources of 8 bits or less, otherwise 16-bit. Primaries match the encoded RGB.
Linear HDR/scRGB and PQ targets retain their separate encoding contracts.

**JXL lossless JPEG transcoding** is available in both conversion modes for SDR
JXL without exposure or resizing. Eligible JPEGs bypass RGB decoding and the JXL
quality preset. Other source formats use normal conversion. The toggle is
persistent; temporarily disabling it for pixel edits retains the saved choice.
Keep EXIF enables reconstruction of the original JPEG bytes.
