#!/usr/bin/env python3
"""Render the guest's real VGA text buffer (dumped over serial) to a PNG."""
import sys
from PIL import Image, ImageDraw, ImageFont

CGA = [
    (0, 0, 0), (0, 0, 170), (0, 170, 0), (0, 170, 170),
    (170, 0, 0), (170, 0, 170), (170, 85, 0), (170, 170, 170),
    (85, 85, 85), (85, 85, 255), (85, 255, 85), (85, 255, 255),
    (255, 85, 85), (255, 85, 255), (255, 255, 85), (255, 255, 255),
]

COLS, ROWS = 80, 25
CW, CH = 12, 24

raw = open(sys.argv[1], "rb").read().decode("latin-1")
start = raw.index("--VGA--") + len("--VGA--\n")
end = raw.index("--END--")
lines = [l.strip() for l in raw[start:end].splitlines() if l.strip()]
assert len(lines) == ROWS, f"expected {ROWS} rows, got {len(lines)}"

img = Image.new("RGB", (COLS * CW, ROWS * CH), (0, 0, 0))
d = ImageDraw.Draw(img)
try:
    font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", 21)
except OSError:
    font = ImageFont.load_default()

for r, line in enumerate(lines):
    assert len(line) == COLS * 4, f"row {r}: {len(line)} chars"
    for c in range(COLS):
        v = int(line[c * 4:(c + 1) * 4], 16)
        ch = v >> 8
        attr = v & 0xFF
        fg, bg = attr & 0x0F, (attr >> 4) & 0x0F
        x, y = c * CW, r * CH
        if bg:
            d.rectangle([x, y, x + CW - 1, y + CH - 1], fill=CGA[bg])
        if 32 <= ch < 127:
            d.text((x + 1, y + 1), chr(ch), font=font, fill=CGA[fg])


img.save(sys.argv[2])
print(f"{sys.argv[2]} {img.width}x{img.height} ({len(lines)} rows rendered)")
