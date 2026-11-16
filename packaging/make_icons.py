#!/usr/bin/env python3
"""Generate RapidEditor's application icons (PNG / ICO / ICNS).

Run once from the repository root:

    python packaging/make_icons.py

Design (v1.1 "AI" theme): a white document card with code lines and a
purple four-point sparkle on a deep indigo-to-violet gradient tile. The
whole thing is drawn procedurally, so it is crisp at every icon size and
carries no watermark. Generated files are committed under assets/ so the
CI jobs (Windows, macOS, Linux) do not need Pillow installed.
"""
from __future__ import annotations

import math
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets"
SIZE = 512

# Palette -------------------------------------------------------------------
BG_TOP_LEFT = (24, 22, 62)      # deep indigo
BG_BOTTOM_RIGHT = (86, 30, 168)  # violet
GLOW = (139, 92, 246)           # violet glow
DOC = (255, 255, 255)
CODE = (109, 40, 217)           # violet code lines
TAB = (124, 58, 237)            # violet title tab
SPARKLE = (139, 92, 246)        # AI sparkle
SHADOW = (12, 8, 40, 130)


def _tile_mask(size: int, radius: int) -> Image.Image:
    """L mask of a rounded square."""
    m = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle((0, 0, size - 1, size - 1), radius=radius, fill=255)
    return m


def gradient_background(size: int, radius: int) -> Image.Image:
    """Diagonal indigo->violet gradient with two soft violet glows."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = img.load()
    mask = _tile_mask(size, radius).load()
    denom = max(1, 2 * (size - 1))
    g1x, g1y, g1r = 0.60 * size, 0.34 * size, 0.72 * size
    g2x, g2y, g2r = 0.50 * size, 1.02 * size, 0.70 * size
    for y in range(size):
        for x in range(size):
            if not mask[x, y]:
                continue
            t = (x + y) / denom
            base = [BG_TOP_LEFT[i] + (BG_BOTTOM_RIGHT[i] - BG_TOP_LEFT[i]) * t
                    for i in range(3)]
            d1 = math.hypot(x - g1x, y - g1y) / g1r
            d2 = math.hypot(x - g2x, y - g2y) / g2r
            a1 = max(0.0, 1.0 - d1) ** 2 * 0.40
            a2 = max(0.0, 1.0 - d2) ** 2 * 0.22
            for i in range(3):
                base[i] = min(255, base[i] + GLOW[i] * (a1 + a2))
            px[x, y] = (int(base[0]), int(base[1]), int(base[2]), 255)
    return img


def sparkle(draw: ImageDraw.ImageDraw, cx: float, cy: float,
            outer: float, inner: float, fill) -> None:
    """A four-point AI sparkle (alternating long/short radii)."""
    pts = []
    for i in range(8):
        ang = math.pi / 4 * i - math.pi / 2
        r = outer if i % 2 == 0 else inner
        pts.append((cx + r * math.cos(ang), cy + r * math.sin(ang)))
    draw.polygon(pts, fill=fill)


def draw_icon(size: int) -> Image.Image:
    s = size / 512.0

    def v(n: float) -> int:
        return int(round(n * s))

    img = gradient_background(size, v(112))
    radius = v(112)

    # Soft shadow beneath the document card.
    shadow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    sd.rounded_rectangle((v(138), v(108), v(374), v(432)), radius=v(30),
                         fill=SHADOW)
    shadow = shadow.filter(ImageFilter.GaussianBlur(v(18)))
    img.alpha_composite(shadow)

    # Document card.
    d = ImageDraw.Draw(img)
    x0, y0, x1, y1 = v(136), v(96), v(372), v(420)
    d.rounded_rectangle((x0, y0, x1, y1), radius=v(28), fill=DOC)

    # Title tab at the top of the card.
    d.rounded_rectangle((v(170), v(134), v(322), v(166)),
                        radius=v(10), fill=TAB)

    # Code lines.
    for i, width in enumerate((158, 128, 96)):
        ly = v(238) + i * v(48)
        d.rounded_rectangle((v(170), ly, v(170) + v(width), ly + v(16)),
                            radius=v(8), fill=CODE)

    # AI sparkle with a gentle violet halo, top-right of the card.
    halo = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    hd = ImageDraw.Draw(halo)
    sparkle(hd, v(336), v(214), v(44), v(12), (167, 139, 250, 160))
    halo = halo.filter(ImageFilter.GaussianBlur(v(10)))
    img.alpha_composite(halo)
    d = ImageDraw.Draw(img)
    sparkle(d, v(336), v(214), v(36), v(10), SPARKLE)

    # Clip back to the rounded tile so the blurred shadow never spills.
    img.putalpha(_tile_mask(size, radius))
    return img


def save_icns(img: Image.Image, path: Path) -> None:
    """Write an ICNS with 512x512 (ic09) and 256x256 (ic08) PNG entries."""
    import io

    chunks = []
    for tag, px in (("ic09", 512), ("ic08", 256)):
        buf = io.BytesIO()
        img.resize((px, px), Image.LANCZOS).save(buf, format="PNG")
        data = buf.getvalue()
        chunks.append(tag.encode("ascii") + struct.pack(">I", len(data) + 8) + data)
    body = b"".join(chunks)
    path.write_bytes(b"icns" + struct.pack(">I", len(body) + 8) + body)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    img = draw_icon(SIZE)
    img.save(OUT / "rapideditor.png", format="PNG")
    img.save(OUT / "rapideditor.ico", format="ICO",
             sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64),
                    (128, 128), (256, 256)])
    save_icns(img, OUT / "rapideditor.icns")
    for p in sorted(OUT.iterdir()):
        print(f"{p.name}: {p.stat().st_size} bytes")


if __name__ == "__main__":
    main()
