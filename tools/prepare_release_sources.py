#!/usr/bin/env python3
"""Collect runtime license texts and optionally publish corresponding sources."""
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
parser.add_argument('--archive', action='store_true')
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
# Build metadata belongs in the source asset, not the runtime archive.
shutil.rmtree(licenses / 'qt-sbom', ignore_errors=True)
shutil.rmtree(licenses / 'qtbase-buildinfo', ignore_errors=True)
buildinfo = build / 'upstream-buildinfo'
buildinfo.mkdir(exist_ok=True)
for name in ['config_qtbase.opt', 'config_qtbase.summary']:
    if (qt_root / name).is_file():
        folder = buildinfo / 'qtbase'
        folder.mkdir(exist_ok=True)
        shutil.copyfile(qt_root / name, folder / name)
if (qt_root / 'sbom').is_dir():
    shutil.rmtree(buildinfo / 'qt-sbom', ignore_errors=True)
    shutil.copytree(qt_root / 'sbom', buildinfo / 'qt-sbom')
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

# Publish per-platform Qt build configurations separately from runtime packages.
# The shared source asset contains the upstream sources; these companions retain
# the SDK configuration and SBOM for each platform's exact Qt binary build.
system_files = list((build / 'CMakeFiles').glob('*/CMakeSystem.cmake'))
if not system_files:
    raise SystemExit('Cannot identify the configured release platform')
system_config = system_files[0].read_text(encoding='utf-8')
system = re.search(r'set\(CMAKE_SYSTEM_NAME "([^"]+)"\)', system_config)[1]
cpu = re.search(r'set\(CMAKE_SYSTEM_PROCESSOR "([^"]+)"\)', system_config)[1]
info_archive = build / f'JXRConverter-{app_version}-{system}-{cpu}-qt-buildinfo.tar.gz'
with tarfile.open(info_archive, 'w:gz') as output:
    output.add(buildinfo, arcname=f'Qt-{qt_version}-{system}-{cpu}-buildinfo')

(licenses / 'SOURCE-ACCESS.txt').write_text(
    f'JXRConverter {app_version} corresponding source and Qt/ICU source archives:\n'
    f'https://github.com/{args.repository}/releases/tag/v{app_version}\n'
    'Download JXRConverter-<version>-source.tar.gz and the matching qt-buildinfo archive.\n'
    'Qt is dynamically linked; compatible replacement Qt libraries are permitted.\n'
    'See docs/redistribution.md and the included license texts.\n', encoding='utf-8')
(licenses / 'upstream-source-manifest.json').write_text(json.dumps(external, indent=2) + '\n')
print('Runtime notices:', licenses)
if not args.archive:
    raise SystemExit(0)

if subprocess.check_output(['git', '-C', str(repo), 'status', '--porcelain', '--untracked-files=no'], text=True).strip():
    raise SystemExit('Corresponding-source archives require a clean release checkout. Commit the release changes first.')

sources = []
for line in (build / 'dependency-sources.tsv').read_text().splitlines():
    name, directory, url, revision = line.split('\t')
    directory = Path(directory)
    commit = subprocess.check_output(['git', '-C', str(directory), 'rev-parse', 'HEAD'], text=True).strip()
    sources.append({'name': name, 'directory': str(directory), 'url': url, 'revision': revision, 'commit': commit})
archive = build / f'JXRConverter-{app_version}-source.tar.gz'
top = f'JXRConverter-{app_version}-source'

def filter_git(info):
    if '.git' in Path(info.name).parts:
        return None
    return info

with tarfile.open(archive, 'w:gz') as output:
    tracked = subprocess.check_output(['git', '-C', str(repo), 'ls-files', '-z']).decode().split('\0')
    for name in tracked:
        if name and (repo / name).is_file():
            output.add(repo / name, arcname=f'{top}/{name}', recursive=False)
    for source in sources:
        output.add(source['directory'], arcname=f"{top}/third_party/{source['name']}", filter=filter_git)
    for source in external:
        output.add(upstream / source['file'], arcname=f"{top}/external_sources/{source['file']}")
    manifest = {'application_commit': subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
                'dependencies': [{key: value for key, value in source.items() if key != 'directory'} for source in sources],
                'external_sources': external}
    generated = build / 'source-manifest.json'
    generated.write_text(json.dumps(manifest, indent=2) + '\n')
    output.add(generated, arcname=f'{top}/source-manifest.json')
    presets = {'version': 3, 'configurePresets': [{'name': 'source-release', 'generator': 'Ninja',
        'binaryDir': '${sourceDir}/build', 'cacheVariables': {
            'CMAKE_BUILD_TYPE': 'Release', 'JXRC_BUILD_GUI': 'ON',
            **{f"FETCHCONTENT_SOURCE_DIR_{s['name'].upper()}": '${sourceDir}/third_party/' + s['name'] for s in sources}}}]}
    generated = build / 'source-presets.json'
    generated.write_text(json.dumps(presets, indent=2) + '\n')
    output.add(generated, arcname=f'{top}/CMakeUserPresets.json')
print('Corresponding source:', archive)
