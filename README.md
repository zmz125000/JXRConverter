# JXRConverter

A C++17 image conversion library and CLI extracted from LocalViewer. The library
owns conversion policy and controls; both the CLI and Qt desktop GUI use it.
No Android SDK or JVM is required. The library and CLI work without a display.

This is the first native port, with Linux integration coverage. Windows and macOS
builds still need validation. See [port status](docs/port-status.md) for the
supported scope and remaining work.

## Build

Requires CMake 3.22+, a C/C++17 compiler, Git, and NASM on x86. Ninja is recommended.
Dependencies are pinned to current stable releases and built from source; see
[dependency versions](docs/dependencies.md). An initial build needs network access:

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

The desktop supports:

- Drag-and-drop files or folders, folder opening, optional recursive import, and
  canonical-path deduplication. Import and metadata discovery run in the background.
- Grid, list, and sortable detail views with a shared selection and size slider.
  Like LocalViewer, the grid slider sets minimum cell width (50–200 dp, in 10 dp
  steps), and cells expand to fill each row as the window resizes.
  Drag from blank space to box-select; Ctrl/Cmd-click and Shift-click extend selection.
- Lazy asynchronous thumbnails with a bounded 64 MiB cache. JPEG thumbnails show
  the SDR base; native JXL/AVIF/JXR/RAW decoders provide fallback previews. Preview
  display rendering is separate from conversion and does not change exported pixels.
- Instant case-insensitive text filtering, with a toggle for the full metadata
  controls: EXIF tags/values, format, SDR/HDR/RAW, minimum longest edge and
  megapixels (both must match), and capture date. The format input accepts comma-
  or space-separated formats/extensions, for example `jpg, avif, raw`. Capture
  date defaults to yesterday through today. Hidden metadata
  controls do not restrict simple search. Today filters by capture date, falling
  back to modification date when capture date is absent, and overrides the custom
  date range. Detail columns sort numerically for size,
  ISO, aperture, shutter, focal length, bit depth, and pixel count. Missing capture
  dates are excluded when a date filter is active.
- Quality presets/custom controls and pixel/RAW controls mapped to the library's
  `Options`. Unsupported transfer targets are disabled in preservation mode.
- Check multiple output formats to convert every input to each selected target.
  All outputs share the configured parallel job limit. Per-file completion waits
  for every output, retaining failures even if another output succeeds. A draggable
  divider separates conversion settings from the independently scrollable output
  and queue controls.
- A toolbar toggle enables wheel changes to dropdowns and numeric inputs; it is
  off by default to avoid changing settings while scrolling. Open output folder
  opens the destination in the system file manager, creating it if needed.
- Separate **Convert selected** and **Convert visible files** actions. Each batch
  captures its options; changing controls during a run affects the next batch.
- Parallel jobs default to CPU count / 4, rounded down with a minimum of one, and a
  per-job codec thread budget. Ultra HDR's pinned source gets a build-tree-only CPU
  budget adaptation; downloaded source files are not modified. Reduce jobs for
  very large images.
- Per-file queue/error states, collision-safe output names within a batch, existing
  output protection, and stopping pending work while active conversions finish.

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

The current version is **0.1.1**. Change `project(JXRConverter VERSION ...)` in
`CMakeLists.txt` to update the library's `jxrc::version()`, CLI help/`--version`,
GUI title/`--version`, Windows file properties, macOS bundle metadata and package
filenames together. The generated public header is `jxrc/version.hpp`.

The **Native library, CLI and GUI** Actions run uploads three downloadable
`JXRConverter-*` artifacts (14-day retention). Each contains a versioned archive
with both applications and Qt runtime dependencies: Windows x64 ZIP, macOS
Apple Silicon tar.gz, and Linux x64 tar.gz. Extract the complete archive before
running `./jxrconverter` (`jxrconverter.exe` on Windows) or the macOS
`jxrconverter.app`; the CLI is `./jxrconvert`. Portable packages place applications
at the extracted root, alongside runtime folders, with no `bin/` directory.
Linux packages target Ubuntu
24.04 or a compatible newer system and still require the system display/graphics
libraries. macOS/Windows packages are unsigned.

The application icon comes from the supplied SVG, stored in
`assets/icons/jxrconverter.svg`. PNG, Windows ICO and macOS ICNS derivatives are
checked in, so normal builds need no icon-generation tools. To regenerate them,
install CairoSVG and Pillow in a Python environment and run
`python3 tools/generate_icons.py`.

Package runners are macOS 26 and Windows Server 2025; macOS binaries retain a
14.0 deployment target. Linux packages retain Ubuntu 24.04 as their build
baseline, with distribution compatibility tests on both 24.04 and 26.04.
See [runner and installer details](docs/dependencies.md#runner-images-and-qt-installation).

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

The **Release** workflow checks the tag against CMake's version, builds and tests
all three desktop packages, tests extracted archives, and publishes a GitHub
Release with generated notes and `SHA256SUMS.txt`. A mismatched tag fails without
publishing. Like LocalViewer, publishing uses `softprops/action-gh-release` to
upload the versioned archives directly to the release's **Assets** section;
rerunning publication can update an existing release's assets. The reusable
`packages.yml` workflow supplies the same package checks
to CI and releases. Source distributions and dependency pins are available through
the tagged repository; bundled third-party license files accompany the binaries.

For noisy downloaded Actions logs, run the streaming deduplicator before reading:

```sh
python3 tools/summarize_build_logs.py .git/logs_RUN_ID
python3 tools/summarize_build_logs.py .git/logs_RUN_ID --details > warnings.txt
```

See [the warning audit](docs/build-warnings.md) for the inspected successful build.
