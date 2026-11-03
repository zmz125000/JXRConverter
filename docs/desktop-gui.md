# Desktop GUI

The optional desktop application uses Qt 6 Widgets. The native converter and GUI use Exiv2 for photographic metadata. Exiv2 0.28.9 is built from source by default. Qt 6.12.0 is covered by CI; installed Qt 6.4+ remains supported.

On Ubuntu:

```sh
sudo apt-get update
sudo apt-get install -y qt6-base-dev libexiv2-dev ninja-build nasm
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJXRC_BUILD_GUI=ON
cmake --build build --target jxrconverter -j 6
./build/jxrconverter
```

You can pass files or folders on the command line:

```sh
./build/jxrconverter /path/to/photos picture.avif
```

To use your distribution's Exiv2, configure with `-DJXRC_USE_SYSTEM_EXIV2=ON` (including Exiv2 0.27 compatibility).

On macOS, install `qt`, `cmake`, `ninja`, and `nasm` with Homebrew, and provide `-DCMAKE_PREFIX_PATH="$(brew --prefix qt)"` if Qt is not discovered. On Windows, provide Qt 6 Widgets with a matching compiler and architecture. Exiv2 is built automatically unless the system-library option is selected. Platform packaging and macOS/Windows GUI execution still need validation.

## Input support and processing

Native inputs include JPEG, HEIC/HEIF, AVIF, JXL, PNG, WebP, TIFF, BMP, GIF, JPEG XR, and LibRaw camera formats. Regular raster decoding is independent of OS/Qt decoders. HDR PNG input supports full-range cICP PQ/HLG with BT.709, BT.2020, or Display P3 primaries, including 8/16-bit samples.

Processing uses float32. Source-preserving JXL/AVIF exports retain the transfer and primaries (AVIF quantizes to its selected bit depth). PNG/TIFF retain 16-bit integer samples for preservation; lossless JXL can preserve those codes exactly. AVIF is limited to 12 bits and JPEG remains lossy. See [supported raster layouts](port-status.md) for restrictions.

GUI input formats follow the converter's current codec support; the GUI does not add the Android-only formats listed in [port status](port-status.md).

Pixel-processing exposure defaults to **0 EV** (unchanged exposure). **−3 EV** multiplies linear RGB by 1/8; **+3 EV** multiplies it by 8. Processing still decodes and re-encodes the image at 0 EV, with source-preserving fast paths where eligible. JPEG/AVIF gain-map photos and Apple/ProXDR HEIC photos exported to ordinary JPEG or AVIF keep the SDR base plus gain map when exposure is 0 and resizing is disabled, including in Process pixels mode. Exposure or resizing requests use the float32 HDR render path instead. JPEG recompression remains lossy; this is base/map retention, not a promise of identical compressed bytes.

For RAW gain-map exports, adjust the separate Custom RAW exposure instead. **Extended BT.2020 · JXL decoding** converts floating-point linear-sRGB JXL's extended colors into BT.2020 before clipping negative channels, preserving more wide-gamut color. This switch affects JXL decoding, not the output transfer function or other input formats.
