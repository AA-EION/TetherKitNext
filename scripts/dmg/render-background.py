#!/usr/bin/env python3
"""Render the DMG window background from background.svg.

    pip install cairosvg
    python3 scripts/dmg/render-background.py

Needs the Inter font installed (and a CJK font for the Chinese line). The PNGs
are committed, so release builds do not run this.
"""
from pathlib import Path

import cairosvg

HERE = Path(__file__).resolve().parent
for scale, name in ((1, "background.png"), (2, "background@2x.png")):
    cairosvg.svg2png(url=str(HERE / "background.svg"), write_to=str(HERE / name),
                     output_width=660 * scale, output_height=400 * scale)
