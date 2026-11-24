#!/usr/bin/env python3
"""Render a /dev/vcsa dump (Hartwell OS proof mode) into a PNG.

Input format:
    <rows> <cols>
    then two hex digits per cell: attribute, character
"""
import sys
from PIL import Image, ImageDraw, ImageFont

CGA = [
    (0, 0, 0), (0, 0, 170), (0, 170, 0), (0, 170, 170),
    (170, 0, 0), (170, 0, 170), (170, 85, 0), (170, 170, 170),
    (85, 85, 85), (85, 85, 255), (85, 255, 85), (85, 255, 255),
    (255, 85, 85), (255, 85, 255), (255, 255, 85), (255, 255, 255),
]
CW, CH = 10, 20

raw = open(sys.argv[1]).read().split("\n")
rows, cols = (int(x) for x in raw[0].split())
cells = [l for l in raw[1:] if l.strip()]

img = Image.new("RGB", (cols * CW, rows * CH), (0, 0, 0))
d = ImageDraw.Draw(img)
try:
    font = ImageFont.truetype("/usr/share/fonts/liberation/LiberationMono-Bold.ttf", 16)
except OSError:
    font = ImageFont.load_default()

for r, line in enumerate(cells[:rows]):
    for c in range(min(cols, len(line) // 4)):
        attr = int(line[c * 4:c * 4 + 2], 16)
        ch = int(line[c * 4 + 2:c * 4 + 4], 16)
        glyph = bytes([ch]).decode("cp437")
        fg, bg = attr & 0x0F, (attr >> 4) & 0x0F
        x, y = c * CW, r * CH
        if bg:
            d.rectangle([x, y, x + CW - 1, y + CH - 1], fill=CGA[bg])
        if glyph.strip() and ch >= 32:
            d.text((x, y + 1), glyph, font=font, fill=CGA[fg])

img.save(sys.argv[2])
print(f"{sys.argv[2]} {img.width}x{img.height} ({rows}x{cols} cells)")
