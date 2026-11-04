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

PNG SDR/source, PQ, and HLG outputs are available. PNG preservation and untouched SDR exports use source integer samples, profiles, and straight alpha; edited exports use float32 until final encoding. PNG depth is configured separately from quality presets; Auto bit depth defaults on and follows source precision. AVIF output depth choices include 8, 10, and 12 bits.

HLG output is available for PNG, JXL, and AVIF in Process pixels mode. The encoders use the inverse BT.2100 display transform at a 1000-nit reference peak (gamma 1.2) before encoding HLG. Out-of-range values are clipped; HLG output does not add highlight detail to SDR inputs.

PQ JXL pixel-processing exports record the measured maximum RGB channel level in nits as `intensity_target` (MaxCLL), bounded to 1–10,000 nits. PQ codes still use the absolute 10,000-nit reference; the metadata does not rescale pixel brightness. Linear JXL retains its own luminance normalization, and direct scRGB JXL retains its 80-nit reference.

GUI input formats follow the converter's current codec support; the GUI does not add the Android-only formats listed in [port status](port-status.md).

Pixel-processing exposure defaults to **0 EV** (unchanged exposure). **−3 EV** multiplies linear RGB by 1/8; **+3 EV** multiplies it by 8. Processing still decodes and re-encodes the image at 0 EV, with source-preserving fast paths where eligible. JPEG/AVIF gain-map photos and Apple/ProXDR HEIC photos exported to ordinary JPEG or AVIF keep the SDR base plus gain map when exposure is 0, resizing is disabled and sRGB clamp is off, including in Process pixels mode. Exposure, resizing or sRGB clamping requests use the float32 HDR render path instead. JPEG recompression remains lossy; this is base/map retention, not a promise of identical compressed bytes.

**Auto exposure** in RAW rendering is off by default and works in Source headroom,
Boost highlights and Custom modes. It meters the white-balanced linear image
before the camera look, tone sliders and HDR boost. A trimmed geometric mean
of up to 50,000 luminance samples targets 18% gray. A 99th-percentile RGB highlight
guard (**AE Protect highlights**, on by default) limits added brightening to keep diffuse highlights below camera white,
while retaining existing HDR headroom. Automatic correction is limited to ±6 EV. Black pixels are ignored; an entirely black image gets no
correction. In Custom mode, Exposure (EV) adds compensation to the metered result.
Metering does not depend on boost target or shoulder, and gain-map exports apply
the same exposure to their SDR base and HDR rendition. This adjusts brightness;
it cannot recover clipped sensor data. CLI/library: `--raw-auto-exposure` /
`RawControls::auto_exposure`. Disable the guard using `--raw-ae-no-highlight-protection`
or `RawControls::auto_exposure_protect_highlights = false`.

In Custom RAW settings, **Disable HDR boost** forces the effective HDR shoulder to 0 while retaining its saved value. This disables highlight lift and its accompanying color restoration; exposure, tone controls, Highlight stops compression and the target cap still apply. Unchecking restores the saved shoulder, and Reset RAW rendering clears the toggle.

RAW outputs bypass Pixel-processing Exposure (EV); use Custom RAW Exposure instead, before the camera look and HDR boost. This applies to every RAW output target, including gain maps. **Extended BT.2020 · JXL decoding** converts floating-point linear-sRGB JXL's extended colors into BT.2020 before clipping negative channels, preserving more wide-gamut color. This switch affects JXL decoding, not the output transfer function or other input formats.

**sRGB clamp** (Process pixels only, off by default) converts output colors to
sRGB/BT.709 primaries and clamps negative RGB channels. SDR targets use their
normal sRGB range; HDR targets retain their selected transfer and HDR brightness
range. It overrides both BT.2020 controls. Untouched scRGB → linear JXL keeps its
direct signed/extended path, just as with scRGB → BT.2020 disabled. The CLI
option is `--srgb-clamp`. GUI-generated filenames add `_srgb` before the extension
when enabled, for example `photo_sdr_srgb.avif`.

**Reset pixel processing** restores exposure, resizing, gamut conversion, and
sRGB clamp to their defaults. It leaves quality and RAW rendering settings alone.
**Load filters** on the top toolbar restores saved searches and image filters at
startup. It defaults off; display preferences and conversion settings still load.

**Wheel changes controls** applies to dropdowns, numeric inputs and sliders, including HDR boost target and shoulder. Scrolling over **Jobs** always switches jobs in menu order, independently of this toggle, and opens the Job view without opening the popup. Navigation stops at the first and last job.

The progress bar shows nonzero Pixel-processing exposure after sRGB clamp, for example `(Max Edge 2048, sRGB Clamp, EV +1)`. This EV applies to non-RAW inputs.
