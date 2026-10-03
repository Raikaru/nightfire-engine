"""Shared look of the engine's original UI art (tools/art/*.py -> assets/ui/<sheet>.png + .txt).

Every sheet is drawn in code (no system fonts, no external images) so the output is byte-for-byte
reproducible: `python3 tools/art/<sheet>.py` rewrites assets/ui/<sheet>.png and its manifest.

The palette and the bevel/outline rules are measured from the game's own art:
  * menu text: label colour 0x7D6D59 and item colour 0x73330F on the GS 0x80 = 1.0 scale (x2 below);
  * button glyphs, texture 0x03000075 (specialchar): charcoal bodies lit from the top right, a dark
    bottom-left, a soft 1 px dark rim, light symbols with grey anti-aliasing, gold D-pad arms.
"""

import math
import os
import zlib

from PIL import Image

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT_DIR = os.path.join(ROOT, "assets", "ui")

# Menu palette (GS colour words doubled to 8-bit).
LABEL = (250, 218, 178)   # 0x7D6D59 x2: page labels
ITEM = (230, 102, 30)     # 0x73330F x2: unselected items
PANEL = (8, 4, 8)         # agent panel body
PANEL_ALPHA = 168
OUTLINE = (4, 2, 4)       # the glyph texture's clear/rim colour
TEAM_PHOENIX = (200, 74, 41)   # the circle glyph red, used for the Phoenix side
TEAM_MI6 = (122, 135, 170)     # the cross glyph blue-grey, used for MI6

# Button glyph palette (0x03000075).
BODY_DARK = (20, 20, 21)       # bottom-left of a face button
BODY_MID = (59, 60, 61)
BODY_LIGHT = (108, 102, 85)    # top-right of a face button
PLATE_FACE = (85, 85, 85)      # R1/L1 plate body
PLATE_SHADE = (71, 71, 72)
PLATE_EDGE_DARK = (44, 44, 45)
PLATE_EDGE_LIGHT = (133, 133, 138)
PLATE_BOTTOM = (51, 52, 54)
PLATE_SHADOW = (28, 29, 28)
INK = (245, 250, 245)          # plate lettering
INK_EDGE = (178, 180, 175)     # its anti-aliased left edge
GOLD_LIGHT = (240, 186, 95)
GOLD_MID = (166, 122, 27)
GOLD_DARK = (104, 75, 20)
SYMBOL_CROSS = (136, 146, 181)
SYMBOL_CIRCLE = (200, 74, 41)
SYMBOL_TRIANGLE = (52, 150, 140)
SYMBOL_SQUARE = (214, 104, 150)


def clamp8(v):
    return max(0, min(255, int(round(v))))


def lerp(a, b, t):
    return tuple(clamp8(x + (y - x) * t) for x, y in zip(a, b))


def noise(x, y, seed=0):
    """Deterministic per-texel grain in [-1, 1] (the original glyphs are slightly dithered)."""
    h = zlib.crc32(bytes([x & 255, y & 255, seed & 255, (x >> 8) & 255]))
    return ((h & 1023) / 511.5) - 1.0


def new_sheet(w, h):
    return Image.new("RGBA", (w, h), OUTLINE + (0,))


def put(img, x, y, rgb, a=255):
    """Alpha-composites one texel over what is there."""
    if not (0 <= x < img.width and 0 <= y < img.height) or a <= 0:
        return
    br, bg, bb, ba = img.getpixel((x, y))
    fa = a / 255.0
    oa = fa + (ba / 255.0) * (1 - fa)
    if oa <= 0:
        return
    mix = lambda f, b: (f * fa + b * (ba / 255.0) * (1 - fa)) / oa
    img.putpixel((x, y), (clamp8(mix(rgb[0], br)), clamp8(mix(rgb[1], bg)), clamp8(mix(rgb[2], bb)), clamp8(oa * 255)))


def coverage(inside, x, y, samples=4):
    """Fraction of texel (x, y) covered by the shape `inside(px, py)` (supersampled)."""
    hit = 0
    for j in range(samples):
        for i in range(samples):
            if inside(x + (i + 0.5) / samples, y + (j + 0.5) / samples):
                hit += 1
    return hit / (samples * samples)


def fill_shape(img, box, inside, shade, samples=4):
    """Fills `inside` (local coordinates) within box (x, y, w, h); shade(u, v) -> rgb with u, v in 0..1."""
    x0, y0, w, h = box
    for y in range(h):
        for x in range(w):
            c = coverage(inside, x, y, samples)
            if c > 0:
                put(img, x0 + x, y0 + y, shade((x + 0.5) / w, (y + 0.5) / h, x, y), int(round(c * 255)))


def bevel(img, box, light=PLATE_EDGE_LIGHT, dark=PLATE_EDGE_DARK):
    """1 px bevel of the plate glyphs: light top and right edges, dark left and bottom edges."""
    x0, y0, w, h = box
    for x in range(x0 + 1, x0 + w - 1):
        put(img, x, y0, light)
        put(img, x, y0 + h - 1, dark)
    for y in range(y0 + 1, y0 + h - 1):
        put(img, x0, y, dark)
        put(img, x0 + w - 1, y, light)


def outline(img, alpha_threshold=128, rgb=OUTLINE, alpha=150):
    """A 1 px dark ring around every texel whose alpha reaches the threshold."""
    src = img.copy()
    for y in range(img.height):
        for x in range(img.width):
            if src.getpixel((x, y))[3] >= alpha_threshold:
                continue
            near = any(
                0 <= x + dx < img.width and 0 <= y + dy < img.height and src.getpixel((x + dx, y + dy))[3] >= alpha_threshold
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            if near:
                put(img, x, y, rgb, alpha)


def body_shade(u, v, x, y, seed=0, dark=BODY_DARK, light=BODY_LIGHT):
    """Face-button body: dark bottom-left to lit top-right, with grain."""
    t = max(0.0, min(1.0, (u + (1.0 - v)) * 0.5))
    t = t * t * (3 - 2 * t)
    rgb = lerp(dark, light, t)
    g = noise(x, y, seed) * 5
    return tuple(clamp8(c + g) for c in rgb)


class Sheet:
    """Shelf packer: sprites are placed in insertion order, so the output is deterministic."""

    def __init__(self, name, width=256, pad=1):
        self.name, self.width, self.pad = name, width, pad
        self.items = []

    def add(self, name, img):
        if any(n == name for n, _ in self.items):
            raise ValueError("duplicate sprite " + name)
        self.items.append((name, img))

    def save(self):
        x = y = shelf = 0
        rects = []
        for name, img in self.items:
            if x + img.width + self.pad > self.width:
                x, y, shelf = 0, y + shelf + self.pad, 0
            rects.append((name, x + self.pad, y + self.pad, img.width, img.height))
            x += img.width + self.pad
            shelf = max(shelf, img.height + self.pad)
        height = 1
        while height < y + shelf + self.pad:
            height *= 2
        sheet = new_sheet(self.width, height)
        for (name, img), (_, rx, ry, _, _) in zip(self.items, rects):
            sheet.alpha_composite(img, (rx, ry))
        os.makedirs(OUT_DIR, exist_ok=True)
        png = os.path.join(OUT_DIR, self.name + ".png")
        sheet.save(png, optimize=False, compress_level=9)
        with open(os.path.join(OUT_DIR, self.name + ".txt"), "w", newline="\n") as f:
            f.write("# %s.png %dx%d, generated by tools/art/%s.py: name x y w h (texels)\n"
                    % (self.name, self.width, height, self.name))
            for name, rx, ry, w, h in rects:
                f.write("%s %d %d %d %d\n" % (name, rx, ry, w, h))
        return png


def circle_inside(cx, cy, r):
    return lambda px, py: (px - cx) ** 2 + (py - cy) ** 2 <= r * r


def round_rect_inside(w, h, r):
    def inside(px, py):
        qx = min(px, w - px)
        qy = min(py, h - py)
        if qx >= r or qy >= r:
            return 0 <= px <= w and 0 <= py <= h
        return (r - qx) ** 2 + (r - qy) ** 2 <= r * r
    return inside


def angle(dx, dy):
    return math.atan2(dy, dx)


# 4x7 capitals and digits for badges (drawn in code; rows top to bottom, '#' set).
PIXEL_FONT_4X7 = {
    "B": ("###.", "#..#", "#..#", "###.", "#..#", "#..#", "###."),
    "C": (".##.", "#..#", "#...", "#...", "#...", "#..#", ".##."),
    "E": ("####", "#...", "#...", "###.", "#...", "#...", "####"),
    "G": (".##.", "#..#", "#...", "#.##", "#..#", "#..#", ".###"),
    "O": (".##.", "#..#", "#..#", "#..#", "#..#", "#..#", ".##."),
    "P": ("###.", "#..#", "#..#", "###.", "#...", "#...", "#..."),
    "S": (".###", "#...", "#...", ".##.", "...#", "...#", "###."),
    "T": ("####", ".#..", ".#..", ".#..", ".#..", ".#..", ".#.."),
    "X": ("#..#", "#..#", ".##.", ".##.", ".##.", "#..#", "#..#"),
    "2": (".##.", "#..#", "...#", "..#.", ".#..", "#...", "####"),
    "/": ("...#", "...#", "..#.", ".##.", ".#..", "#...", "#..."),
    " ": ("....", "....", "....", "....", "....", "....", "...."),
}


def pixel_text_width(text):
    """Width of `text` in the 4x7 badge font, 1 texel between characters."""
    return 5 * len(text) - 1 if text else 0


def pixel_text(img, x, y, text, rgb, a=255):
    """Draws `text` in the 4x7 badge font with its top-left at (x, y)."""
    for i, ch in enumerate(text):
        for j, row in enumerate(PIXEL_FONT_4X7[ch]):
            for k, bit in enumerate(row):
                if bit == "#":
                    put(img, x + 5 * i + k, y + j, rgb, a)
