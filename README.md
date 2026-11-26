<h1 align="center">
<img src="https://raw.githubusercontent.com/zmz125000/LocalViewer-art/refs/heads/master/JXRConverter.svg" width="200" alt="JXRConverter">
<br>JXRConverter<br>
  
</h1>
<p align="center">
  <a href="https://github.com/zmz125000/JXRConverter/actions/workflows/release.yml">
    <img src="https://github.com/zmz125000/JXRConverter/actions/workflows/release.yml/badge.svg" alt="Github Actions">
  </a>
  <a href="#license">
    <img src="https://img.shields.io/badge/license-MIT%20%2B%20GPLv3-blue" alt="MIT and GPLv3 notices">
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

JXRConverter is a HDR image converter that  
renders accurate SDR base images from HDR sources,    
while preserving wide color gamut, with built-in color management  
then outputs to JPG/AVIF Gain Map Ultra HDR and more.   

Supported HDR formats: JPG, PNG, JXR, JXL, HEIF, AVIF, OpenEXR, Radiance HDR, PFM, TIFF and RAW.  

The Qt 6 GUI for Windows, Linux and macOS uses FFmpeg for video conversion and frame grabs. The Video tab selects H.264, HEVC or AV1 independently from the container and dynamic range. It offers encoder presets, rate control, bit depth, chroma subsampling and audio settings, with SDR tone mapping and PQ or HLG output for HEVC and AV1. Frames can be exported to 16-bit PNG, JXL and AVIF.

special thanks to  [MPCVR](https://github.com/Aleksoid1978/VideoRenderer/) and [SKIV](https://github.com/SpecialKO/SKIV) for the best HDR to SDR tone mappers.

#### The app uses piecewise tone mapping (customized skiv) for HDR to SDR conversion, the results are much better than tools like Xbox Game Bar built-in PNG.  

<img width="800" alt="download" src="https://github.com/user-attachments/assets/f8fcd4e1-6401-4010-afe7-5232cf5da598" />

## Features:  
1. Convert OpenEXR, Radiance HDR/RGBE/XYZE, PFM, floating-point/LogLuv TIFF, JXR, JXL, RAW to JPG/AVIF gain map (Ultra HDR/ISO 21496-1) and AVIF/JXL scRGB, HLG and PQ HDR.
2. Creates accurate SDR JPGs from HDR sources by preserving the "untouched" SDR range (0-203 nits) from HDR sources to sRGB SDR.
3. Convert/Compress JPEG/HEIF/AVIF HDR photos to JPEG/AVIF gain map.
4. Generic image conversion/compression.
5. Support auto color management, keeping source wide color gamut and color tag.
6. JXR HDR images with scRGB 80 nits SDR white are rebased to 203 nits for UHDR SDR base image.
7. RAW images are processed and output as scRGB, Linear HDR or PQ HDR with BT.2020 color.
8. Gain map reconstruction mode, reverse gain map to F32 for Linear and PQ outputs.
9. Float32 high quality image processing.
10. File listing with ultra fast thumbnails.
11. HDR boost rendering for RAW images.

See [port status](docs/port-status.md) for the supported scope and remaining work.
See the [conversion matrix](docs/conversion-matrix.md) for input classes, output formats and processing paths.

<a href="https://github.com/zmz125000/JXRConverter/releases">
<img alt="Get it on GitHub" src="https://github.com/zmz125000/LocalViewer-art/blob/master/get-it-on-github.svg" width="200px"/>
</a>

<img width="2587" height="1726" alt="Screenshot 2026-11-18 200403_result" src="https://github.com/user-attachments/assets/1387bd0d-26b7-4b2d-b5b9-9dfd3db69a77" />
<img width="1920" height="1080" alt="JXRConverter JXR JPG HDR sample" src="https://github.com/zmz125000/LocalViewer-art/blob/master/Screenshots/PRAGMATA%204_21_2026%2010_24_50%20PM_low_gainmap.jpg?raw=true" />
<img width="1920" height="1080" alt="JXRConverter JXR JPG HDR sample" src="https://github.com/zmz125000/LocalViewer-art/blob/master/Screenshots/S.T.A.L.K.E.R.%202_%20Heart%20of%20Chornobyl%20%20%2011_23_2025%2010_04_32%20AM_low_gainmap.jpg?raw=true" />

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

Targets are `jpeg`, `jpeg-gainmap`, `jxl`, `jxl-linear`, `jxl-scrgb`, `jxl-pq`, `jxl-hlg`, `avif`, `avif-pq`, `avif-hlg`, `avif-gainmap`, `png`, `png-hdr` (also `png-pq`), `png-hlg`, and `webp`. WebP is 8-bit SDR with lossy and lossless presets, alpha, ICC profiles, and optional EXIF. Use `--mode process` to render HDR to WebP. Other generic targets in preservation mode retain the source transfer, so `--target png` can retain a PQ or HLG source. Explicit PQ and HLG targets require `--mode process`.

PNG uses direct integer samples for preservation and untouched SDR exports, retaining source profiles and straight alpha. Pixel edits use the float32 path. PNG cannot preserve gain maps or extended floating HDR directly; use process mode to render those inputs. `--png-depth 8|16` sets PNG precision independently of quality presets. Auto bit depth defaults on (8 bits for 8-bit sources, 16 for deeper sources); use `--no-auto-bit-depth` to apply the selected depth directly.

HLG PNG, JXL and AVIF exports accept display-linear float32 RGB (1.0 = 203 nits). They apply the inverse BT.2100 OOTF followed by the HLG OETF, using the same 1000-nit reference display and gamma 1.2 as the decoders. Values outside the HLG encoding range are clipped. JXL records a 1000-nit intensity target; PNG and AVIF carry HLG CICP tags.

Preservation mode retains compatible source metadata and avoids linearization, tone mapping, and HDR reconstruction. Codec quantization and color/bit-depth conversion can still change sample values. JPEG is lossy, including with the `lossless` preset. `--jpeg-transcoding` passes compressed SDR JPEG data directly to libjxl when exporting SDR JXL without exposure or resizing. Gain-map sources are rejected on this path.

Process mode decodes and processes linear-light pixels. SDR targets clip above 203 nits by default. `--sdr-tone-map` enables highlight compression. Its default mapper is `skiv-custom`: SKIV’s ICtCp intensity/chroma curve anchored at the manual SDR shoulder and SDR white. Pixels below the shoulder stay unchanged. `--sdr-tone-mapper skiv` uses SKIV’s original export settings (120-nit PQ target, 10000-nit cap, 1.18 intensity exponent, and 99.94th-percentile luminance). Stock SKIV and SKIV (custom) ignore Auto tone curve, color mapping, and tonal-zone adjustments; only SKIV (custom) uses our shoulder and HDR range controls. `--sdr-tone-mapper skiv-manual` adds Shadows, Midtones, and Highlights before the SKIV (custom) curve, keeping the HDR range until SKIV compresses it. Its color mapping replaces SKIV’s internal ICtCp mapping: Max RGB and Luminance scale RGB together using their chosen metric; Per channel applies the SKIV curve independently to each channel. Auto tone curve is enabled by default for mappers that support it. It automatically dims bright HDR midtones in bright daylight scenes (keeping shadows, peak highlights, and dark/SDR scenes neutral) while Shadows and Highlights remain manual. Use `--no-sdr-auto-tone-curve` to set midtones manually. The other choices are `logarithmic`, `extended-reinhard`, and `reinhard`; `--sdr-hdr-range` caps the range, using the actual image peak when it is lower. `--sdr-auto-hdr-range` uses the image peak without a cap. The same SDR mapping supplies new JPEG/AVIF gain-map bases. RAW source mode follows sensor headroom up to 10,000 nits. `--raw-mode boost` opens highlights toward the default 1,500-nit target. RAW adjustment flags select custom mode unless `--raw-mode` is explicitly supplied. Gain-map output from an SDR source does not add highlight detail.

MPCVR is available via `--mode process --sdr-tone-map --sdr-tone-mapper mpcvr`. It uses the measured image peak, a fixed 203-nit SDR target, and its own spline and color mapping. The destination encoder applies its regular SDR transfer function and color profile.

`--threads N` sets the per-conversion codec CPU budget (`0` keeps automatic defaults). Presets are `lossless`, `high`, `medium`, `low`, and `custom`. `--quality`, `--jxl-distance`, `--avif-depth`, and `--downsize-gainmap` select the custom preset and conflict with an explicitly selected non-custom preset. Existing output files require `--overwrite`. Failed conversions do not publish partial destination files, and input and output must refer to different files.

For supported formats and conversion paths, see the [conversion matrix](docs/conversion-matrix.md). For GUI build and usage, see [Desktop GUI](docs/desktop-gui.md).

**Transparency background color**, below Gain map reconstruction, offers White, Black, White (remove alpha), and Black (remove alpha). White / Black fill JPEG exports while other formats retain alpha. The remove-alpha choices flatten every format in linear light; transparent gain-map sources require Full reconstruction. CLI: `--transparency-background white|black|white-remove-alpha|black-remove-alpha` (default: black).

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
- [SKIV](https://github.com/SpecialKO/SKIV)

## links
Those links are unrelated to the project.
- [Pictureflect Photo Viewer](https://pictureflect.com/)
- [Special K Image Viewer](https://github.com/SpecialKO/SKIV)
- [HDRFrame](https://apps.microsoft.com/detail/9p4ngn61cr4t)
- [jxr2uhdr](https://github.com/tfx2001/jxr2uhdr/)
- [HDR2gainmapApp](https://github.com/vastunghia/HDR2gainmapApp)
- [Win11 HDR sRGB fix](https://github.com/HiBluey/Fuck-sRGB)

## License

JXRConverter’s original code is licensed under [MIT](LICENSE). The MPCVR tone mapper is licensed under GPLv3. Distributions of the combined engine and apps are covered by [GPLv3](COPYING). [Third-party notices](NOTICE)
