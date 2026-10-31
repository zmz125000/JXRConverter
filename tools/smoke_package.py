#!/usr/bin/env python3
"""Extract a CPack archive and verify both applications without the Qt SDK path."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

build = Path(sys.argv[1]).resolve()
archives = sorted(build.glob('JXRConverter-*.zip')) + sorted(build.glob('JXRConverter-*.tar.gz'))
if not archives:
    sys.exit('No package archive found')
for archive in archives:
    with tempfile.TemporaryDirectory(prefix='jxrc-package-') as directory:
        shutil.unpack_archive(archive, directory)
        root = Path(directory)
        suffix = '.exe' if os.name == 'nt' else ''
        # Locate installed executables, not similarly named data directories
        # (share/jxrconverter can precede the app bundle on macOS).
        cli_matches = [p for p in root.glob('*/jxrconvert' + suffix) if p.is_file()]
        gui_pattern = ('*/jxrconverter.app/Contents/MacOS/jxrconverter'
                       if sys.platform == 'darwin' else '*/jxrconverter' + suffix)
        gui_matches = [p for p in root.glob(gui_pattern) if p.is_file()]
        if len(cli_matches) != 1 or len(gui_matches) != 1:
            sys.exit(f'Expected one installed CLI and GUI in {archive.name}; '
                     f'found CLI={cli_matches}, GUI={gui_matches}')
        cli, gui = cli_matches[0], gui_matches[0]
        env = os.environ.copy()
        qt_root = env.pop('QT_ROOT_DIR', '') or env.get('QTDIR', '')
        for name in ('QT_PLUGIN_PATH', 'QML2_IMPORT_PATH', 'QTDIR', 'LD_LIBRARY_PATH', 'DYLD_LIBRARY_PATH'):
            env.pop(name, None)
        if qt_root:
            env['PATH'] = os.pathsep.join(p for p in env['PATH'].split(os.pathsep)
                                          if not p.lower().startswith(qt_root.lower()))
        env['QT_QPA_PLATFORM'] = 'offscreen'
        subprocess.run([str(cli), '--version'], env=env, check=True)
        subprocess.run([str(cli), '--help'], env=env, check=True, stdout=subprocess.DEVNULL)
        fixture = build / 'cli-input.jpg'
        if fixture.is_file():
            output = root / 'smoke-output.avif'
            subprocess.run([str(cli), str(fixture), str(output), '--target', 'avif'], env=env, check=True)
            if not output.is_file() or not output.stat().st_size:
                sys.exit('Packaged CLI did not produce an image')
        subprocess.run([str(gui), '--version'], env=env, check=True)
        if sys.platform.startswith('linux'):
            dependencies = subprocess.check_output(['ldd', str(gui)], env=env, text=True)
            qt_libraries = [line for line in dependencies.splitlines() if 'libQt6' in line]
            if not qt_libraries or any(str(root) not in line for line in qt_libraries):
                sys.exit(f'Qt libraries must resolve inside the extracted package:\n{dependencies}')
        process = subprocess.Popen([str(gui)], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            output, errors = process.communicate(timeout=3)
            sys.exit(f'GUI exited before smoke test completed ({process.returncode}):\n{errors.decode(errors="replace")}')
        except subprocess.TimeoutExpired:
            process.terminate()
            process.communicate(timeout=10)
        print(f'Package smoke test passed: {archive.name}')
