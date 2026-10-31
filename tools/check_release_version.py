#!/usr/bin/env python3
"""Reject a release tag that disagrees with CMake's generated version header."""
import re
import sys
from pathlib import Path

tag, header = sys.argv[1:]
version = re.search(r'^#define JXRC_VERSION "([^"]+)"', Path(header).read_text(), re.M)[1]
if tag != f'v{version}':
    sys.exit(f'Release tag {tag!r} must match CMake project version v{version}')
print(f'Release version verified: {version}')
