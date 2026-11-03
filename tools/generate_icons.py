#!/usr/bin/env python3
"""Regenerate native icons from the supplied SVG (pip install cairosvg pillow)."""
import argparse
import math
from io import BytesIO
from pathlib import Path

import cairosvg
from PIL import Image

root = Path(__file__).resolve().parents[1]
directory = root / 'assets' / 'icons'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mac-only', action='store_true')
args = parser.parse_args()
png = cairosvg.svg2png(url=str(directory / 'jxrconverter.svg'),
                      output_width=1024, output_height=1024)
image = Image.open(BytesIO(png)).convert('RGBA')
if not args.mac_only:
    image.resize((512, 512), Image.Resampling.LANCZOS).save(directory / 'jxrconverter.png')
    image.save(directory / 'jxrconverter.ico',
               sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
# macOS does not apply an icon mask to a legacy .icns. Supply the squircle
# explicitly, including optical padding; use it for both bundle and Dock icons.
points = []
for step in range(1024):
    angle = step * 2 * math.pi / 1024
    x, y = math.cos(angle), math.sin(angle)
    points.append(f'{512 + 412 * math.copysign(abs(x) ** .5, x):.3f},'
                  f'{512 + 412 * math.copysign(abs(y) ** .5, y):.3f}')
outline = 'M' + ' L'.join(points) + ' Z'
background = f'''<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024">
<defs><linearGradient id="tile" x2="0" y2="1">
<stop stop-color="#ffffff"/><stop offset="1" stop-color="#e8edef"/>
</linearGradient></defs><path d="{outline}" fill="url(#tile)" stroke="#d0d7db" stroke-width="2"/>
</svg>'''
mac_icon = Image.open(BytesIO(cairosvg.svg2png(bytestring=background.encode()))).convert('RGBA')
art = cairosvg.svg2png(url=str(root / 'samples' / 'launcher_icon-web0.5.svg'),
                      output_width=660, output_height=660)
mac_icon.alpha_composite(Image.open(BytesIO(art)).convert('RGBA'), (182, 182))
mac_icon.resize((512, 512), Image.Resampling.LANCZOS).save(directory / 'jxrconverter-macos.png')
mac_icon.save(directory / 'jxrconverter.icns')
