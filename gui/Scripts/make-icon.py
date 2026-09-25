#!/usr/bin/env python3
"""Render the app icon from its SVG source.

    pip install cairosvg pillow
    python3 gui/Scripts/make-icon.py

Writes gui/Resources/AppIcon.icns (every size macOS asks for, each rendered
from the vector source rather than scaled from one bitmap) and
docs/assets/icon.png (README). Works on any OS; no iconutil needed.
"""
import io
from pathlib import Path

import cairosvg
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "gui/Resources/Icon/AppIcon.svg"


def render(size: int) -> Image.Image:
    png = cairosvg.svg2png(url=str(SOURCE), output_width=size, output_height=size)
    return Image.open(io.BytesIO(png)).convert("RGBA")


def main() -> None:
    images = {size: render(size) for size in (16, 32, 64, 128, 256, 512, 1024)}
    images[1024].save(ROOT / "gui/Resources/AppIcon.icns",
                      append_images=[images[s] for s in (16, 32, 64, 128, 256, 512)])
    images[512].save(ROOT / "docs/assets/icon.png", optimize=True)


if __name__ == "__main__":
    main()
