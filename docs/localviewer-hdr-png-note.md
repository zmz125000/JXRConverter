Finding while porting LocalViewer HDR PNG
=======================================

Scope: commit `bbbeba5db374936c624951e491cc7b828397d673`; this is a finding
from the port, not a general LocalViewer review. LocalViewer was not modified.

The new `png_hdr_linear_rgba()` in `app/src/main/cpp/avif_hdr.cpp:1137`
uses `avif_linear_lut(HLG)` directly. That LUT produces `12 * scene RGB`.
The existing AVIF path at lines 607–619 additionally applies the luminance-based
BT.2100 OOTF and the 1000/203 reference-display scale. The PNG path skips both,
and `pack_linear_f16_for_direct()` does not supply them later.
Consequently, the same HLG pixels render differently in PNG and AVIF.
For opaque neutral full-scale white, PNG supplies 12 in the working space,
whereas AVIF supplies approximately 4.926 (1000/203), a factor of 2.436.
The PNG regression test currently expects 12, so it enshrines this inconsistency.

Suggested LocalViewer fix: apply the same source-gamut-dependent HLG OOTF as AVIF
before converting PNG linear samples to F16; update the HLG test expectations,
including a colored sample because the OOTF depends on luminance and primaries.
JXRConverter shares that OOTF between the PNG and AVIF decoders and keeps export
processing in float32.

References:

- [PNG conversion](https://github.com/zmz125000/LocalViewer/blob/bbbeba5db374936c624951e491cc7b828397d673/app/src/main/cpp/avif_hdr.cpp#L1137)
- [Existing AVIF HLG conversion](https://github.com/zmz125000/LocalViewer/blob/bbbeba5db374936c624951e491cc7b828397d673/app/src/main/cpp/avif_hdr.cpp#L607)
- [LocalViewer PNG test](https://github.com/zmz125000/LocalViewer/blob/bbbeba5db374936c624951e491cc7b828397d673/benchmark/avif/png_hdr_test.cpp#L23)
- [PNG cICP precedence](https://www.w3.org/TR/png-3/#cICP-chunk)
