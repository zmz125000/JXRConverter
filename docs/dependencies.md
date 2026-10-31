# Dependency releases

Release audit: 2026-10-31. Source dependencies use explicit stable release tags,
not moving branches. JPEG is fetched separately so an older Ultra HDR vendored
copy cannot override the selected release.

| Dependency | Release | Upstream |
|---|---|---|
| Ultra HDR | 2.0.2 | [Release](https://github.com/google/libultrahdr/releases/tag/v2.0.2) |
| libjpeg-turbo | 3.2.1 | [Release](https://github.com/libjpeg-turbo/libjpeg-turbo/releases/tag/3.2.1) |
| libjxl | 0.12.0 | [Release](https://github.com/libjxl/libjxl/releases/tag/v0.12.0) |
| libaom | 3.15.3 | [Tag](https://aomedia.googlesource.com/aom/+/refs/tags/v3.15.3) |
| libavif | 1.4.2 | [Release](https://github.com/AOMediaCodec/libavif/releases/tag/v1.4.2) |
| LibRaw | 0.22.3 | [Release](https://github.com/LibRaw/LibRaw/releases/tag/0.22.3) |
| JPEG XR | 0.3.1 | [Tag](https://github.com/bvibber/jpegxr/tree/0.3.1) |
| Exiv2 | 0.28.9 | [Release](https://github.com/Exiv2/exiv2/releases/tag/v0.28.9) |
| Qt (current-release GUI CI) | 6.12.0 | [Official release](https://download.qt.io/official_releases/qt/6.12/6.12.0/) |

JPEG XR has no newer release tag. The existing Ultra HDR, libjxl and libavif
pins were already current. libjxl's Brotli, Highway and skcms submodules retain
the revisions selected by its release. Exiv2 reuses that Brotli decoder instead
of building another copy. Exiv2's unused video/network/XMP/PNG/INI integrations
are disabled; EXIF, BMFF and compressed JXL metadata remain enabled.

The normal desktop build fetches Exiv2. `JXRC_USE_SYSTEM_EXIV2=ON` retains support
for distribution packages, including Ubuntu 24.04's Exiv2 0.27.6. Qt's supported
minimum remains 6.4; the current-release CI job installs Qt 6.12.0 and its ICU
runtime, while a separate job exercises Ubuntu's older Qt and Exiv2 packages.

## Runner images and Qt installation

Desktop packages use `macos-26` (Apple Silicon), `windows-2025` (x64), and
`ubuntu-24.04` (x64). macOS 14 runners are deprecated; the macOS package still
sets `CMAKE_OSX_DEPLOYMENT_TARGET=14.0`. Windows uses the installed MSVC 14.44
toolset to match Qt's MSVC 2022 binaries. GitHub's standard Windows 11 runners
are ARM64, so they would require a separate ARM64 build rather than replacing
the x64 runner. See [GitHub's image catalog](https://github.com/actions/runner-images#available-images).

Linux release packages remain built on Ubuntu 24.04 to retain that libc
baseline. Distribution Qt/Exiv2 tests run on both Ubuntu 24.04 and 26.04.
The Linux runner is Ubuntu, not a Debian runner or a `.deb` package build.

For Qt 6.12, the Linux installer architecture is **`linux_gcc_64`**; `gcc_64`
is the installation directory name and an obsolete installer architecture for
this Qt version. The latter causes the misleading `qt_base` package-not-found
error. The Qt action's [architecture documentation](https://github.com/jurplel/install-qt-action#arch)
describes this distinction.

PyPI's latest aqtinstall release, 3.3.0, also lacks the
[Qt 6.11+ Windows repository-layout fix](https://github.com/miurahr/aqtinstall/pull/1000).
CI uses upstream commit `076e1659807d0b362a3ed684d54c2e9c775eb9c7`
(`3.3.1.dev166`) until a stable release includes it. This avoids the invalid
duplicated `qt6_6120/qt6_6120` Windows metadata URL. py7zr is pinned to 1.1.4,
compatible with that revision's requirement of at least 1.1.3; the Qt action's
default 1.1.0 predates it. Failed Qt setup uploads `aqtinstall.log` as a diagnostic
artifact. These errors do not require downgrading Python 3.15.

GitHub Actions use checkout 7.0.1, msvc-dev-cmd 1.13.0, install-qt-action
4.5.0, upload-artifact 7.0.2 and download-artifact 8.0.2. Build tools installed
from the runners' package repositories use the versions supplied by those
repositories.

## Desktop package contents

The supplied 0.1.1 Linux and Windows Actions downloads in `samples/` each
contain one versioned release archive. Publish those inner `.tar.gz`/`.zip`
archives, rather than the outer Actions download ZIPs. The release workflow
downloads and uploads the inner archives automatically.

Archive inspection found no SDK headers, static/import libraries, PDBs, CMake
files, build logs or test executables. Linux's largest runtime dependency is
ICU data (about 31 MiB); Windows includes the Visual C++ redistributable
installer (about 18 MiB) and Qt's graphics support DLLs. These are retained for
runtime compatibility. If the Visual C++ runtime is missing on Windows, run
`vc_redist.x64.exe` before launching either application (older 0.1.1 samples place
it under `bin/`; new portable packages place executables at the extracted root).

Deployment now excludes optional networking/TLS, generic embedded input and
EGL framebuffer plugins, plus embedded platform plugins, using Qt's
[deployment selection options](https://doc.qt.io/qt-6/qt-generate-deploy-app-script.html).
Desktop display backends, image codecs, input methods, ICU and dependency
licenses remain included. The samples predate these exclusions; new package
sizes have not been measured.

## Integration fixes

Ultra HDR's ordinary `core` target carries its JPEG and image-IO link
requirements transitively. Linking it avoids both its MSVC-specific wrapper
name and the nested JPEG archive rejected by the macOS linker. Windows builds
use a consistent DLL CRT, `LIBRAW_NODLL` for static LibRaw, and `NOMINMAX`.
The workflow explicitly exports NASM's installed directory to later steps.

The Ultra HDR CPU-budget adaptation still applies to a generated build-tree
copy; it does not modify downloaded sources. LocalViewer source caches are
reused only if their HEAD matches the requested release tag, so dependency
updates cannot silently keep an older cached release.
