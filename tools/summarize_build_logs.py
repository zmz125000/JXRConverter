#!/usr/bin/env python3
"""Stream GitHub Actions logs and collapse repeated compiler/configure warnings."""
import argparse
from collections import Counter
from pathlib import Path
import re


def summarize(directory, details=False):
    # Downloaded logs include both a combined job log and individual step logs.
    # Prefer the combined files so each occurrence is counted only once.
    logs = sorted(directory.glob('*.txt')) or sorted(directory.rglob('*.txt'))
    for log in logs:
        warnings = Counter()
        examples = {}
        pending = None
        with log.open(errors='replace') as stream:
            for raw in stream:
                line = re.sub(r'^\s*\d{4}-\d\d-\d\dT\S+\s+', '', raw.strip())
                line = re.sub(r'\x1b\[[0-9;]*m', '', line)
                if re.search(r'(?:\bwarning(?: [A-Z]\d+)?\s*:|^CMake (?:Deprecation )?Warning)', line, re.I):
                    # Normalize checkout and build paths, retaining file and line.
                    line = line.replace('\\', '/')
                    line = re.sub(r'.*?JXRConverter/JXRConverter/', '', line)
                    line = re.sub(r'^.*?(/build[^/]*/)', 'build/', line)
                    warnings[line] += 1
                    examples.setdefault(line, [])
                    pending = line if line.startswith('CMake ') else None
                elif pending and line and len(examples[pending]) < 4:
                    examples[pending].append(line)
                elif not line:
                    pending = None
        print(f'\n## {log.name}: {sum(warnings.values())} warning lines, {len(warnings)} unique')
        grouped = Counter()
        for warning, count in warnings.items():
            dependency = re.search(r'_deps/([^/]+)', warning)
            code = re.search(r'warning ([A-Z]\d+)|\[(-W[^\]]+)\]', warning)
            key = (dependency[1] if dependency else 'project/toolchain',
                   (code[1] or code[2]) if code else warning.split(':', 1)[0])
            grouped[key] += count
        if not details:
            for (component, code), count in grouped.most_common():
                print(f'{count:6}x {component}: {code}')
            continue
        for warning, count in warnings.most_common():
            print(f'{count:6}x {warning}')
            for detail in examples[warning]:
                print(f'         {detail}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--details', action='store_true', help='Print every unique diagnostic and CMake context')
    args = parser.parse_args()
    summarize(args.directory, args.details)
