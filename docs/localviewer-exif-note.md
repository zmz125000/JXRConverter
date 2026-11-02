# LocalViewer HDR JPEG EXIF finding

While comparing EXIF retention, the local LocalViewer source showed that
`app/src/main/cpp/hdr_convert.cpp` receives EXIF in `StillEncodeQuality`,
but `encode_linear_rgba_f16_to_uhdr` does not pass it to
`uhdr_enc_set_exif_data`. Its SDR JPEG fallback calls `write_export_exif`,
so the omission specifically affects the actual HDR gain-map encoding path.
The JNI export accepts the EXIF payload and forwards it to the quality structure;
this is a missing encoder connection rather than an unavailable input tag.

JXRConverter now supplies an `Exif\0\0`-prefixed metadata block to Ultra HDR
and tests photographic tags in a real HDR JPEG gain-map output. LocalViewer
was not modified. This finding comes from source inspection, not an Android
runtime test.
