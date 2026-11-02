# Redistribution and corresponding source

JXRConverter is distributed under GPL version 3; it contains GPL code derived
from LocalViewer/EhViewer and uses Exiv2. Moving a GPL library into a separate
DLL does not remove the combined application's GPL obligations.

The codec dependencies can remain linked into the shared JXRConverter engine:

| Dependency | Selected license / notices |
|---|---|
| Exiv2 | GPL v2 or later, compatible with this GPLv3 application |
| libheif, libde265 | LGPL v3 or later; corresponding sources and build scripts available for rebuilding/relinking |
| LibRaw | LGPL v2.1 option, with the complete application/library sources and build scripts supplied for rebuilding/relinking; the CDDL alternative is not selected |
| Ultra HDR | Apache-2.0/MIT and retained third-party notices |
| libjpeg-turbo | IJG, BSD and zlib notices |
| libjxl, skcms, Brotli, Highway | BSD/Apache and their included notices/patent terms |
| libavif, libaom, libwebp | BSD and applicable patent notices |
| JPEG XR | Microsoft's BSD license (`license.md`) |
| libpng, zlib, libtiff, stb_image | Their permissive licenses, retained in the package |
| Qt runtime | Dynamically linked; GPL/LGPL licenses and bundled third-party notices retained |
| ICU runtime | Unicode/ICU notices retained |

The license review found no codec that must become a separate DLL solely for
this project's GPLv3 distribution. The shared engine removes duplicated binary
code. This is a source/license compatibility review; patent grants and conditions
remain in the shipped notices, rather than a claim of universal patent clearance.

Release jobs publish desktop runtime packages and `SHA256SUMS.txt`. They do not
create or upload separate source or Qt build-info archives.

Runtime packages retain the application and dependency licenses. `SOURCE-ACCESS.txt`
links to the application source revision and names `cmake/Dependencies.cmake` for
the native dependency repositories and pinned releases. The bundled
`upstream-source-manifest.json` lists the Qt/ICU source URLs and SHA-256 digests.

Before CPack, run `python tools/prepare_release_sources.py build` to collect those
notices and references. The workflow does this automatically. Source references
do not replace the obligations in the licenses linked below; distributors are
responsible for the source access arrangements required for their distribution.

References: [GPLv3 section 6](https://www.gnu.org/licenses/gpl-3.0.html#section6),
[GNU license FAQ](https://www.gnu.org/licenses/gpl-faq.html),
[Qt licensing](https://doc.qt.io/qt-6/licensing.html),
[Qt LGPL obligations](https://www.qt.io/development/open-source-lgpl-obligations),
[Microsoft C++ runtime deployment](https://learn.microsoft.com/en-us/cpp/windows/deployment-in-visual-cpp).
