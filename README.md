# JXRConverter

A C++17 image conversion library and CLI extracted from [LocalViewer](https://github.com/zmz125000/LocalViewer). 

Features:  
1. Convert JXR, JXL, RAW to JPG/AVIF gain map (Ultra HDR/ISO 21496-1) and AVIF/JXL PQ HDR.
2. Convert/Compress JPEG/HEIF/AVIF HDR photos to JPEG/AVIF gain map.
3. Support auto color management, keeping source wide color gamut and color tag.
4. JXR HDR images with scRGB 80 nits SDR white are rebased to 203 nits for SDR output.
5. RAW images are processed and output as Linear HDR or PQ HDR with BT.2020 color.

See [port status](docs/port-status.md) for the supported scope and remaining work.

## Build

Requires CMake 3.22+, a C/C++17 compiler, Git, and NASM on x86. Ninja is recommended.
Dependencies are pinned to current stable releases and built from source; see
[dependency versions](docs/dependencies.md). An initial build needs network access:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target jxrconvert jxrc_tests -j 6
ctest --test-dir build --output-on-failure
```

## Desktop GUI

The optional desktop application uses Qt 6 Widgets and Exiv2. Exiv2 0.28.9 is
built from source by default. Qt 6.12.0 is covered by CI; installed Qt 6.4+
remains supported. On Ubuntu:

```sh
sudo apt-get update
sudo apt-get install -y qt6-base-dev libexiv2-dev ninja-build nasm
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJXRC_BUILD_GUI=ON
cmake --build build --target jxrconverter -j 6
./build/jxrconverter
```

You can also pass files or folders on the command line:

```sh
./build/jxrconverter /path/to/photos picture.avif
```

To use your distribution's Exiv2 instead, configure with
`-DJXRC_USE_SYSTEM_EXIV2=ON` (including Exiv2 0.27 compatibility).

On macOS, install `qt`, `cmake`, `ninja`, and `nasm` with Homebrew,
and provide `-DCMAKE_PREFIX_PATH="$(brew --prefix qt)"` if Qt is not discovered.
On Windows, provide Qt 6 Widgets with a matching compiler and architecture.
Exiv2 is built automatically unless the system-library option is selected. Platform packaging
and macOS/Windows GUI execution still need validation.

Native inputs include JPEG, AVIF, JXL, PNG, WebP, TIFF, BMP, GIF, JPEG XR and
LibRaw camera formats. Regular raster decoding is independent of OS/Qt decoders.
PNG/TIFF retain 16-bit integer samples for preservation; lossless JXL can preserve
those codes exactly. AVIF is limited to 12 bits and JPEG remains lossy. See
[supported raster layouts](docs/port-status.md) for restrictions.

GUI input formats follow the converter's current codec support; the GUI does not
add the Android-only formats listed in [port status](docs/port-status.md).

Pixel-processing exposure defaults to **0 EV** (unchanged exposure). **−3 EV**
multiplies linear RGB by 1/8; **+3 EV** multiplies it by 8. Processing still decodes
and re-encodes the image at 0 EV; use preservation mode to preserve source values.
For RAW gain-map exports, adjust the separate Custom RAW exposure instead.
**Extended BT.2020 · JXL decoding** converts floating-point linear-sRGB JXL's
extended colors into BT.2020 before clipping negative channels, preserving more
wide-gamut color. This switch affects JXL decoding, not the output transfer
function or other input formats.

## Convert

Preserve source transfer, color profiles, and gain-map metadata while converting
containers/codecs:

```sh
build/jxrconvert input-hdr.jpg output.avif --target avif-gainmap --preset high
build/jxrconvert input.avif output.jpg --target jpeg-gainmap --preset high
build/jxrconvert input-pq.jxl output.avif --target avif --preset lossless
build/jxrconvert input.jpg output.jxl --target jxl --jpeg-transcoding --keep-exif
```

Preservation avoids linearization, tone mapping, and HDR reconstruction. Codec
quantization, RGB/YUV conversion, and bit-depth conversion can still change
sample values. JPEG remains lossy even with the `lossless` preset. The explicit
JPEG transcoding option stores JPEG coefficients in JXL; retaining EXIF allows
reconstruction of the original JPEG bytes. Gain-map sources are rejected for
JXL preservation because that path cannot carry the gain map.

Decode and process linear-light pixels, then select the output representation:

```sh
build/jxrconvert input.avif output.jxl --mode process --target jxl-linear
build/jxrconvert input.jxl output.avif --mode process --target avif-pq --exposure -0.5
build/jxrconvert input.jxr output.jpg --mode process --target jpeg --max-edge 2048
build/jxrconvert camera.dng output.avif --mode process --target avif-gainmap \
  --raw-exposure 0.5 --raw-wb kelvin --raw-kelvin 6000 \
  --raw-highlight-stops 1 --raw-shoulder 0.6 --raw-target-nits 1500
```

Targets: `jpeg`, `jpeg-gainmap`, `jxl`, `jxl-linear`, `jxl-pq`, `avif`,
`avif-pq`, `avif-gainmap`. Generic targets in preservation mode retain the source
transfer, so `--target avif` can produce a PQ AVIF from a PQ source.
Processing SDR targets clip to SDR; there is currently no selectable general
HDR-to-SDR tone mapper. Processing gain-map outputs use the ported LocalViewer
encoder; an SDR source does not gain new highlight detail.

`--threads N` sets a per-conversion codec CPU budget (`0` keeps automatic defaults).

RAW source mode follows sensor headroom up to 10,000 nits. `--raw-mode boost`
opens highlights toward the default 1,500-nit target. RAW adjustment flags select
custom mode unless an explicit `--raw-mode` was supplied.

`--help` lists all controls. Presets are `lossless`, `high`, `medium`, `low`, and
`custom`. `--quality`, `--jxl-distance`, `--avif-depth`, and
`--downsize-gainmap` select the custom preset and conflict with an explicitly
selected non-custom preset. Existing output files require `--overwrite`.
Failed conversions do not publish partial destination files. Input and output
must refer to different files.

## Library

Embed the project with CMake and link `jxrc::jxrc`. The C++ API is in
[`include/jxrc/converter.hpp`](include/jxrc/converter.hpp):

```cpp
#include <jxrc/converter.hpp>

jxrc::Options options;
options.mode = jxrc::Mode::process;
options.target = jxrc::Target::avif_pq;
options.raw.hdr_mode = jxrc::RawHdrMode::custom;
options.raw.exposure = 0.5f;
auto result = jxrc::convert("camera.dng", "output.avif", options);
```

Options are per-call values, independent of global viewer settings. Decoders
hand off straight-alpha linear F16 pixels in the declared RGB primaries, with
1.0 corresponding to 203 nits. RAW HDR gain-map exports retain a separate SDR
camera-look render. Pixel-processing controls are rejected in preservation mode.
There is no silent fallback between those modes.

The CMake target handles codec linkage for embedding. A standalone installed SDK,
C ABI, and language bindings remain future packaging work. CLI exit codes are
0 for success, 2 for invalid arguments, and 1 for conversion/I/O failure.

## License

The port retains LocalViewer's GPLv3 license. See [COPYING](COPYING) and
[NOTICE](NOTICE). Codec dependencies retain their own licenses.

## Versions, CI downloads and releases

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
