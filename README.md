# JXRConverter

A C++17 image conversion library and CLI extracted from LocalViewer. The library
owns conversion policy and controls; the CLI is its first client. A GUI can use
the same API later. No Android SDK, JVM, or display is required.

This is the first native port, with Linux integration coverage. Windows and macOS
builds still need validation. See [port status](docs/port-status.md) for the
supported scope and remaining work.

## Build

Requires CMake 3.22+, a C/C++17 compiler, Git, and NASM on x86. Ninja is recommended.
Dependencies are pinned to the codec versions used by LocalViewer and built from
source. An initial build needs network access:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target jxrconvert jxrc_tests -j 6
ctest --test-dir build --output-on-failure
```

To reuse an existing LocalViewer native source cache, supply its `_deps` directory:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DJXRC_DEPS_DIR=/path/to/LocalViewer/app/.cxx/RelWithDebInfo/build-id/x86_64/_deps
```

The source snapshot in `LocalViewer/` is a reference, not a build dependency.
`src/native/` contains the extracted implementation. The build never invokes
Gradle or compiles Android entry points. Build only the named targets above to
avoid unrelated upstream developer tools.

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
