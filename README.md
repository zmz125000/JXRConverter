<h1 align="center">
<img src="https://raw.githubusercontent.com/zmz125000/LocalViewer-art/refs/heads/master/JXRConverter.svg" width="200" alt="JXRConverter">
<br>JXRConverter<br>
  
</h1>
<p align="center">
  <a href="https://github.com/zmz125000/JXRConverter/actions/workflows/release.yml">
    <img src="https://github.com/zmz125000/JXRConverter/actions/workflows/release.yml/badge.svg" alt="Github Actions">
  </a>
  <a href="/LICENSE">
    <img src="https://img.shields.io/github/license/zmz125000/JXRConverter" alt="LICENSE">
  </a>
  <a href="https://github.com/zmz125000/JXRConverter/releases">
    <img src="https://img.shields.io/github/v/release/zmz125000/JXRConverter" alt="Release">
  </a>
  <a href="https://github.com/zmz125000/JXRConverter/issues">
    <img src="https://img.shields.io/github/issues/zmz125000/JXRConverter" alt="Issues">
  </a>
</p>
<p align="center">
  <a href="https://github.com/zmz125000/JXRConverter/releases/latest">
    <img src="https://img.shields.io/github/downloads/zmz125000/JXRConverter/latest/total?label=Latest%20Downloads&labelColor=27303D&color=0D1117&logo=github&logoColor=FFFFFF&style=flat" alt="Github Actions">
  </a>
  <a href="https://github.com/zmz125000/JXRConverter/releases">
    <img src="https://img.shields.io/github/downloads/zmz125000/JXRConverter/total?label=Total%20Downloads&labelColor=27303D&color=0D1117&logo=github&logoColor=FFFFFF&style=flat" alt="LICENSE">
  </a>
</p>
A C++17 image conversion library and CLI extracted from LocalViewer.  

https://github.com/zmz125000/LocalViewer  

JXRConverter is a RAW/JXR converter that  
renders accurate SDR base images,  
while preserving wide color gamut,  
then outputs to JPG/AVIF Gain Map HDR.   

It also supports scRGB and PQ outputs  
and comes with a Qt6 GUI for Windows, Linux and macOS.

Features:  
1. Convert JXR, JXL, RAW to JPG/AVIF gain map (Ultra HDR/ISO 21496-1) and AVIF/JXL PQ HDR.
2. Convert/Compress JPEG/HEIF/AVIF HDR photos to JPEG/AVIF gain map.
3. Support auto color management, keeping source wide color gamut and color tag.
4. JXR HDR images with scRGB 80 nits SDR white are rebased to 203 nits for UHDR SDR base image.
5. RAW images are processed and output as scRGB, Linear HDR or PQ HDR with BT.2020 color.
6. Float32 high quality image processing.
7. File listing with ultra fast thumbnails.
8. HDR boost rendering for RAW images.

See [port status](docs/port-status.md) for the supported scope and remaining work.
See the [conversion matrix](docs/conversion-matrix.md) for input classes, output formats and processing paths.

<a href="https://github.com/zmz125000/JXRConverter/releases">
<img alt="Get it on GitHub" src="https://github.com/zmz125000/LocalViewer-art/blob/master/get-it-on-github.svg" width="200px"/>
</a>

<img width="2232" height="1561" alt="JXRConverter screenshot" src="https://github.com/user-attachments/assets/a9cdc1e5-86de-4592-8fa6-4db33049ab23" />

## Build

Requires CMake 3.22+, a C/C++17 compiler, Git, and NASM on x86. Ninja is recommended. Dependencies are pinned and built from source; see [dependency versions](docs/dependencies.md). The initial build needs network access.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target jxrconvert -j 6
```

## CLI usage

Run `jxrconvert --help` for the complete option list. Basic conversion examples:

```sh
# Preserve source transfer, color profile, and gain-map metadata.
build/jxrconvert input-hdr.jpg output.avif --target avif-gainmap --preset high
build/jxrconvert input.avif output.jpg --target jpeg-gainmap --preset high
build/jxrconvert input-pq.jxl output.avif --target avif --preset lossless
build/jxrconvert input.jpg output.jxl --target jxl --jpeg-transcoding --keep-exif

# Decode and process linear-light pixels before encoding.
build/jxrconvert input.avif output.jxl --mode process --target jxl-linear
build/jxrconvert input.jxl output.avif --mode process --target avif-pq --exposure -0.5
build/jxrconvert input.jxr output.jpg --mode process --target jpeg --max-edge 2048

# Process a RAW file using custom HDR controls.
build/jxrconvert camera.dng output.avif --mode process --target avif-gainmap \
  --raw-exposure 0.5 --raw-wb kelvin --raw-kelvin 6000 \
  --raw-highlight-stops 1 --raw-shoulder 0.6 --raw-target-nits 1500
```

Targets are `jpeg`, `jpeg-gainmap`, `jxl`, `jxl-linear`, `jxl-scrgb`, `jxl-pq`, `jxl-hlg`, `avif`, `avif-pq`, `avif-hlg`, `avif-gainmap`, `png`, `png-hdr` (also `png-pq`), and `png-hlg`. Generic targets in preservation mode retain the source transfer, so `--target png` can retain a PQ or HLG source. Explicit PQ and HLG targets require `--mode process`.

PNG uses direct integer samples for preservation and untouched SDR exports, retaining source profiles and straight alpha. Pixel edits use the float32 path. PNG cannot preserve gain maps or extended floating HDR directly; use process mode to render those inputs. `--png-depth 8|16` sets PNG precision independently of quality presets. Auto bit depth defaults on (8 bits for 8-bit sources, 16 for deeper sources); use `--no-auto-bit-depth` to apply the selected depth directly.

HLG PNG, JXL and AVIF exports accept display-linear float32 RGB (1.0 = 203 nits). They apply the inverse BT.2100 OOTF followed by the HLG OETF, using the same 1000-nit reference display and gamma 1.2 as the decoders. Values outside the HLG encoding range are clipped. JXL records a 1000-nit intensity target; PNG and AVIF carry HLG CICP tags.

Preservation mode retains compatible source metadata and avoids linearization, tone mapping, and HDR reconstruction. Codec quantization and color/bit-depth conversion can still change sample values. JPEG is lossy, including with the `lossless` preset. `--jpeg-transcoding` passes compressed SDR JPEG data directly to libjxl when exporting SDR JXL without exposure or resizing. Gain-map sources are rejected on this path.

Process mode decodes and processes linear-light pixels. Processing SDR targets clip to SDR; no general HDR-to-SDR tone mapper is currently selectable. RAW source mode follows sensor headroom up to 10,000 nits. `--raw-mode boost` opens highlights toward the default 1,500-nit target. RAW adjustment flags select custom mode unless `--raw-mode` is explicitly supplied. Gain-map output from an SDR source does not add highlight detail.

`--threads N` sets the per-conversion codec CPU budget (`0` keeps automatic defaults). Presets are `lossless`, `high`, `medium`, `low`, and `custom`. `--quality`, `--jxl-distance`, `--avif-depth`, and `--downsize-gainmap` select the custom preset and conflict with an explicitly selected non-custom preset. Existing output files require `--overwrite`. Failed conversions do not publish partial destination files, and input and output must refer to different files.

For supported formats and conversion paths, see the [conversion matrix](docs/conversion-matrix.md). For GUI build and usage, see [Desktop GUI](docs/desktop-gui.md).

## Thanks

- [libultrahdr](https://github.com/google/libultrahdr)
- [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo)
- [libjxl](https://github.com/libjxl/libjxl) and [skcms](https://skia.org/docs/user/skcms/)
- [libavif](https://github.com/AOMediaCodec/libavif), [libaom](https://aomedia.googlesource.com/aom/), and [dav1d](https://code.videolan.org/videolan/dav1d)
- [libheif](https://github.com/strukturag/libheif) and [libde265](https://github.com/strukturag/libde265)
- [LibRaw](https://github.com/LibRaw/LibRaw)
- [JPEG XR](https://github.com/bvibber/jpegxr)
- [libpng](https://github.com/pnggroup/libpng), [libwebp](https://github.com/webmproject/libwebp), [libtiff](https://gitlab.com/libtiff/libtiff), and [zlib](https://github.com/madler/zlib)
- [stb_image](https://github.com/nothings/stb)
- [Exiv2](https://github.com/Exiv2/exiv2)
- [Qt](https://www.qt.io/)

## links
Those links are unrelated to the project.
- [Pictureflect Photo Viewer](https://pictureflect.com/)
- [Special K Image Viewer](https://github.com/SpecialKO/SKIV)

## License

The port retains LocalViewer's GPLv3 license.
