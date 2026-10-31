#!/usr/bin/env python3
"""Regenerate native icons from the supplied SVG (pip install cairosvg pillow)."""
from io import BytesIO
from pathlib import Path

import cairosvg
from PIL import Image

directory = Path(__file__).resolve().parents[1] / 'assets' / 'icons'
png = cairosvg.svg2png(url=str(directory / 'jxrconverter.svg'),
                      output_width=1024, output_height=1024)
image = Image.open(BytesIO(png)).convert('RGBA')
image.resize((512, 512), Image.Resampling.LANCZOS).save(directory / 'jxrconverter.png')
image.save(directory / 'jxrconverter.ico',
           sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
image.save(directory / 'jxrconverter.icns')
