# Release package audit

Inspected the three 0.1.5 desktop archives supplied in `samples/`.

| Package | Uncompressed | Finding |
|---|---:|---|
| Windows AMD64 | 100.2 MiB | VC installer 17.86 MiB; optional D3D/DXC compiler DLLs 19.55 MiB; codec code embedded in both executables |
| Linux x86_64 | 109.4 MiB | ICU data 30.55 MiB is required by Qt; codec code embedded in both executables |
| macOS arm64 | 80.1 MiB | Codec code embedded in both executables; Qt frameworks correctly separate |

The GUI and CLI are different executables, not duplicate files. Windows SHA-256:

- GUI: `670e701e9014b491a8110894f50386c2b13aa367533b21aa6024b73003d344df`
- CLI: `ad127a4e1682992ea769f1e96194cb214d0858701f28c617852ec747ce3940d5`

The supplied GUI is 18.60 MiB and CLI 16.46 MiB on Windows; source build linkage
shows both contain the same static codec engine. New builds use one shared
`jxrc` library by default. Qt remains dynamically linked. A static engine is
available for embedders with `JXRC_SHARED_ENGINE=OFF`.

New packaging removes the VC redistributable installer and deploys MSVC CRT DLLs
beside both executables instead. Their PE imports require `MSVCP140`, `_1`, `_2`
and `VCRUNTIME140`, `_1`; Windows provides the Universal CRT and `icuuc.dll`.
Optional shader compiler payloads are omitted for this Widgets application.
Qt image-format plugins are omitted because native codecs supply thumbnails
and conversion. Native desktop, font, input and offscreen plugins are retained;
TLS plugins are now required by the update button. Linux ICU libraries/data stay.

The supplied archives contain no SDK headers, static libraries, PDBs, build logs
or test executables. Dependency notices are retained. Packaging now also collects
lowercase license filenames (fixing JPEG XR's omitted `license.md`) and the Qt,
Qt Wayland and ICU runtime notices. Previous archives lacked these Qt/ICU texts.

On macOS the updater helper, licenses and an app-relative manifest are inside
the bundle. A moved `.app` updates independently; a complete extracted folder
also updates the CLI. The final bundle is ad-hoc signed after resources are installed.

The updater installs only package files, retains unrelated images/user files,
removes obsolete files recorded by the previous package manifest, and rolls back
replacement failures. It does not modify existing published release assets.

See [redistribution requirements](redistribution.md) for the source release flow.
Local Linux 0.1.6 build/package inspection passed: GUI 4.34 MiB, CLI 0.05 MiB,
one shared engine 20.01 MiB, complete runtime archive 96.15 MiB unpacked and
36.88 MiB compressed. Both installed executables resolve the packaged engine;
no missing dynamic libraries were reported. The manifest matches all 293 files
and symlinks. Qt notices and TLS are present; duplicate SBOM/build metadata stays
in source assets. Windows/macOS still require their CI builds; live self-update
replacement has not been exercised.
