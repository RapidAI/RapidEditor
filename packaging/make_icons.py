#!/usr/bin/env python3
"""Generate RapidEditor's application icons (PNG / ICO / ICNS).

Run once from the repository root:

    python packaging/make_icons.py

The generated files are committed under assets/ so the CI jobs (Windows,
macOS, Linux) do not need Pillow installed.
"""
from __future__ import annotations

import struct
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets"
SIZE = 512


def rounded(size: int, radius: int) -> Image.Image:
    """A vertically shaded blue rounded square."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = img.load()
    top = (59, 130, 246)      # #3B82F6
    bottom = (29, 60, 170)    # #1D3CAA
    r = radius
    for y in range(size):
        t = y / max(1, size - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
        for x in range(size):
            inside = True
            # distance to the nearest rounded corner centre
            cx = min(max(x, r), size - 1 - r)
            cy = min(max(y, r), size - 1 - r)
            if (x - cx) ** 2 + (y - cy) ** 2 > r * r:
                inside = False
            if inside:
                px[x, y] = c
    return img


def draw_icon(size: int) -> Image.Image:
    s = size / 512.0
    def v(n: float) -> int:
        return int(round(n * s))

    img = rounded(size, v(112))
    d = ImageDraw.Draw(img)

    # Page
    x0, y0, x1, y1 = v(132), v(96), v(380), v(420)
    d.rounded_rectangle((x0, y0, x1, y1), radius=v(28), fill=(255, 255, 255, 255))

    # Folded corner (a blue triangle over the white page)
    fx = v(300)
    d.polygon([(fx, y0), (x1, y0 + v(80)), (x1, y0 + v(112)),
               (fx + v(66), y0 + v(112)), (fx, y0 + v(46))],
              fill=(37, 99, 235, 255))
    d.polygon([(fx, y0), (x1, y0 + v(80)), (fx + v(80), y0 + v(80))],
              fill=(147, 197, 253, 255))

    # Text lines
    line = (71, 85, 105, 255)
    for i, width in enumerate((176, 216, 150)):
        ly = v(206) + i * v(46)
        d.rounded_rectangle((v(166), ly, v(166) + width, ly + v(18)),
                            radius=v(9), fill=line)

    # Caret / "fast" chevron
    d.polygon([(v(282), v(320)), (v(346), v(365)), (v(282), v(410)),
               (v(282), v(386)), (v(318), v(365)), (v(282), v(344))],
              fill=(249, 115, 22, 255))
    return img


def save_icns(img: Image.Image, path: Path) -> None:
    """Write a minimal ICNS with a 512x512 PNG (ic09) and 256x256 PNG (ic08)."""
    chunks = []
    for tag, px in (("ic09", 512), ("ic08", 256)):
        buf = __import__("io").BytesIO()
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
