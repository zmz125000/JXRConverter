# What JXRConverter can do

| Input | Output | Processing |
|---|---|---|
| SDR photos and images (JPEG, PNG, WebP, TIFF, BMP, GIF, AVIF, HEIF, JXL) | SDR JPEG, AVIF or JXL | Preserve the source look, or edit pixels in linear light with float32 precision. |
| HDR gain-map photos (Ultra HDR JPEG, gain-map AVIF, Apple/ProXDR HEIC) | Gain-map JPEG or AVIF; SDR JPEG/AVIF; linear or PQ JXL/AVIF | Keep the original gain map directly when eligible, or render HDR pixels and create a new gain map. |
| HDR images (PQ/HLG AVIF, PNG or JXL) | SDR JPEG/AVIF/JXL; gain-map JPEG/AVIF; linear or PQ JXL/AVIF | Decode to float32 linear light for processing; convert to PQ or create a gain map as needed. |
| Linear HDR and scRGB (JXL, plus scRGB JPEG XR input) | Linear JXL, scRGB JXL, PQ JXL/AVIF, SDR or gain-map JPEG/AVIF | Preserve eligible scRGB JXL directly, or process linear pixels with exposure and gamut conversion. |
| Camera RAW (DNG and LibRaw-supported formats) | SDR JPEG/AVIF/JXL; gain-map JPEG/AVIF; linear/scRGB JXL; PQ JXL/AVIF | Demosaic with camera look, white balance and exposure controls; render sensor highlights for HDR output. |

**Processing notes:** Pixel edits use float32 linear-light data. Exposure and resizing are supported. HDR-to-SDR output clips to SDR; a general tone-mapping control is not currently available. JPEG is lossy, AVIF supports up to 12-bit output, and JXL supports linear HDR and scRGB.
