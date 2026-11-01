# Redistribution and corresponding source

JXRConverter is distributed under GPL version 3; it contains GPL code derived
from LocalViewer/EhViewer and uses Exiv2. Moving a GPL library into a separate
DLL does not remove the combined application's GPL obligations.

The codec dependencies can remain linked into the shared JXRConverter engine:

| Dependency | Selected license / notices |
|---|---|
| Exiv2 | GPL v2 or later, compatible with this GPLv3 application |
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

Release jobs publish `JXRConverter-<version>-source.tar.gz` alongside binaries.
It contains the application/build scripts, the exact populated native dependency
sources (including libjxl's submodules), dependency revisions, and Qt/ICU source
archives matching the runtime. Qt/ICU source files are hosted in this project’s
release, rather than relying only on an upstream link. Binary packages carry a
source-access notice naming that same release.

To build native dependencies without refetching their repositories, extract the
source release and run `cmake --preset source-release`. The generated
`CMakeUserPresets.json` points FetchContent at each included `third_party/` tree.
Install a compatible Qt SDK first, or build Qt from `external_sources/` using its
upstream instructions and set `CMAKE_PREFIX_PATH` to that installation. Download the matching `JXRConverter-<version>-<OS>-<CPU>-qt-buildinfo.tar.gz`
companion for that platform's Qt configuration summaries and SBOM. Upstream source
digests are recorded in the source manifest. These build details are kept out of
the runtime archives. Standard compiler/toolchain and OS system libraries remain prerequisites.

Before CPack, run `python tools/prepare_release_sources.py build` to collect runtime
license texts; `--archive` additionally builds the corresponding-source package.
The workflow performs both steps automatically. Keep binary and source assets
available together when distributing or mirroring releases. Distributors still
need to review their own changes and distribution terms.

References: [GPLv3 section 6](https://www.gnu.org/licenses/gpl-3.0.html#section6),
[GNU license FAQ](https://www.gnu.org/licenses/gpl-faq.html),
[Qt licensing](https://doc.qt.io/qt-6/licensing.html),
[Qt LGPL obligations](https://www.qt.io/development/open-source-lgpl-obligations),
[Microsoft C++ runtime deployment](https://learn.microsoft.com/en-us/cpp/windows/deployment-in-visual-cpp).
