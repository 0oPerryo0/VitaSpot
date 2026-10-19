"""Generate placeholder LiveArea images (8-bit indexed PNGs, as the Vita
requires) and template.xml under sce_sys/. Standard library only.

    python tools/gen_livearea.py
"""
import os
import struct
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sce_sys")

BG = (18, 18, 18)
ACCENT = (30, 215, 96)
DARK = (0, 0, 0)
WHITE = (255, 255, 255)
PALETTE = [BG, ACCENT, DARK, WHITE, (28, 58, 42)]


def write_png(path, w, h, pixel):
    """pixel(x, y) -> palette index."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(pixel(x, y) for x in range(w))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    plte = b"".join(bytes(c) for c in PALETTE)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0))
    png += chunk(b"PLTE", plte)
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)


def in_rrect(x, y, x0, y0, w, h, r):
    if not (x0 <= x < x0 + w and y0 <= y < y0 + h):
        return False
    cx = min(max(x, x0 + r), x0 + w - r - 1)
    cy = min(max(y, y0 + r), y0 + h - r - 1)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def note(x, y, cx, cy, s):
    """Music note glyph centred on (cx, cy), size s."""
    u = s / 24.0
    hx, hy = cx + (8 - 12) * u, cy + (17 - 12) * u
    if (x - hx) ** 2 + (y - hy) ** 2 <= (3.4 * u) ** 2:
        return True
    sx = cx + (11 - 12) * u
    if sx - 0.9 * u <= x <= sx + 0.9 * u and cy + (4.5 - 12) * u <= y <= hy:
        return True
    # flag
    fy0 = cy + (4.5 - 12) * u
    if sx <= x <= cx + (18 - 12) * u:
        t = (x - sx) / ((18 - 11) * u)
        top = fy0 + t * 2 * u
        if top - 0.9 * u <= y <= top + 3.0 * u:
            return True
    return False


def badge(x, y, cx, cy, size):
    r = size * 0.28
    if in_rrect(x, y, cx - size / 2, cy - size / 2, size, size, r):
        return 2 if note(x, y, cx, cy, size * 0.65) else 1
    return None


def icon(x, y):
    b = badge(x, y, 64, 64, 128)
    return b if b is not None else 0


def bg(x, y):
    b = badge(x, y, 420, 230, 200)
    if b is not None:
        return b
    return 4 if y < 120 else 0


def startup(x, y):
    b = badge(x, y, 140, 79, 110)
    return b if b is not None else 0


TEMPLATE = """<?xml version="1.0" encoding="utf-8"?>
<livearea style="a1" format-ver="01.00" content-rev="1">
  <livearea-background>
    <image>bg.png</image>
  </livearea-background>
  <gate>
    <startup-image>startup.png</startup-image>
  </gate>
</livearea>
"""

if __name__ == "__main__":
    write_png(os.path.join(ROOT, "icon0.png"), 128, 128, icon)
    write_png(os.path.join(ROOT, "livearea", "contents", "bg.png"), 840, 500, bg)
    write_png(os.path.join(ROOT, "livearea", "contents", "startup.png"), 280, 158, startup)
    with open(os.path.join(ROOT, "livearea", "contents", "template.xml"), "w", encoding="utf-8") as f:
        f.write(TEMPLATE)
    print("wrote", os.path.normpath(ROOT))
