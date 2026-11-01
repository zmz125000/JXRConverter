#!/usr/bin/env python3
"""Collect runtime license texts and source references for release packages."""
import argparse
import shutil
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('build', type=Path)
parser.add_argument('--repository', default='zmz125000/JXRConverter')
args = parser.parse_args()
build = args.build.resolve()
repo = Path(__file__).resolve().parents[1]
cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
qt_dir = re.search(r'^Qt6_DIR:[^=]+=(.+)$', cache, re.M)
if not qt_dir:
    raise SystemExit('Configure the GUI before collecting release sources')
qt_root = Path(qt_dir[1].strip()).parents[2]
config = (qt_root / 'lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake').read_text(encoding='utf-8')
qt_version = re.search(r'set\(PACKAGE_VERSION "([0-9.]+)"\)', config)[1]
app_version = re.search(r'#define JXRC_VERSION "([0-9.]+)"', (build / 'generated/jxrc/version.hpp').read_text())[1]
upstream = build / 'upstream-sources'
upstream.mkdir(exist_ok=True)
licenses = build / 'runtime-licenses'
licenses.mkdir(exist_ok=True)
# Remove obsolete metadata from reused notice directories.
shutil.rmtree(licenses / 'qt-sbom', ignore_errors=True)
shutil.rmtree(licenses / 'qtbase-buildinfo', ignore_errors=True)
external = []

def download(name, url):
    path = upstream / name
    if not path.exists():
        temporary = path.with_suffix(path.suffix + '.part')
        print('Downloading', name, flush=True)
        request = urllib.request.Request(url, headers={'User-Agent': 'JXRConverter-release-sources'})
        with urllib.request.urlopen(request, timeout=120) as response, temporary.open('wb') as output:
            shutil.copyfileobj(response, output)
        temporary.replace(path)
    with path.open('rb') as file:
        digest = hashlib.file_digest(file, 'sha256').hexdigest()
    external.append({'file': name, 'url': url, 'sha256': digest})
    return path

def notices(archive, component):
    destination = licenses / component
    shutil.rmtree(destination, ignore_errors=True)
    destination.mkdir(exist_ok=True)
    with tarfile.open(archive) as source:
        for member in source:
            if not member.isfile():
                continue
            name = Path(member.name)
            if name.name != 'qt_attribution.json' and 'licenses' not in [part.lower() for part in name.parts] and not re.match(r'^(licen[cs]e|copying|copyright|notice)([._-].*)?$', name.name, re.I):
                continue
            # Preserve subcomponent paths, excluding the archive's top directory.
            relative = Path(*name.parts[1:])
            if relative.is_absolute() or '..' in relative.parts:
                raise RuntimeError('Unsafe source notice path')
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.extractfile(member) as input_file, target.open('wb') as output:
                shutil.copyfileobj(input_file, output)

minor = '.'.join(qt_version.split('.')[:2])
qt_components = ['qtbase']
if list((qt_root / 'lib').glob('libQt6WaylandClient*')):
    qt_components.append('qtwayland')
for component in qt_components:
    name = f'{component}-everywhere-src-{qt_version}.tar.xz'
    path = download(name, f'https://download.qt.io/official_releases/qt/{minor}/{qt_version}/submodules/{name}')
    notices(path, component)
icu = sorted((qt_root / 'lib').glob('libicuuc.so.*.*'))
if icu:
    match = re.search(r'\.so\.(\d+)\.(\d+)$', icu[0].resolve().name)
    if not match:
        raise SystemExit('Cannot determine bundled ICU source version')
    major, patch = match.groups()
    name = f'icu4c-{major}_{patch}-src.tgz'
    notices(download(name, f'https://github.com/unicode-org/icu/releases/download/release-{major}-{patch}/{name}'), 'icu')
    # ICU 73.2 calls its main license license.html; notices() includes it.

commit = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
(licenses / 'SOURCE-ACCESS.txt').write_text(
    f'JXRConverter {app_version} source revision:\n'
    f'https://github.com/{args.repository}/tree/{commit}\n'
    'Native dependency repositories and revisions are listed in cmake/Dependencies.cmake.\n'
    'Qt/ICU source locations and SHA-256 digests are in upstream-source-manifest.json.\n'
    'Qt is dynamically linked; compatible replacement Qt libraries are permitted.\n'
    'See docs/redistribution.md and the included license texts.\n', encoding='utf-8')
(licenses / 'upstream-source-manifest.json').write_text(json.dumps(external, indent=2) + '\n')
print('Runtime notices:', licenses)
