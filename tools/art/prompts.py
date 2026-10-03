#!/usr/bin/env python3
"""Button-prompt glyphs for keyboard/mouse and Xbox-style pads: assets/ui/prompts.png + prompts.txt.

Drawn to sit beside the game's own `~X` glyphs (texture 0x03000075): face buttons are 19x19 charcoal
discs lit from the top right with a light symbol, shoulder plates and keycaps are 13 texels tall like the
L1/R1 plates (light top/right edge, dark left/bottom edge, a soft shadow row, bold white lettering with a
grey left edge), D-pad and mouse highlights use the D-pad gold. Xbox-style buttons are generic discs with
a coloured letter; no console logos or third-party glyph art are used.

Run: python3 tools/art/prompts.py   (deterministic; rewrites assets/ui/prompts.png and prompts.txt)
"""

import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
import pixel_font  # noqa: E402
from style import (BODY_DARK, BODY_LIGHT, GOLD_DARK, GOLD_LIGHT, GOLD_MID, INK, INK_EDGE, PLATE_BOTTOM,  # noqa: E402
                   PLATE_EDGE_DARK, PLATE_EDGE_LIGHT, PLATE_FACE, PLATE_SHADE, PLATE_SHADOW, Sheet, body_shade,
                   circle_inside, fill_shape, lerp, noise, put, round_rect_inside)

CAP_H = 13          # keycap / plate height (L1/R1 plates are 13 texels tall)
FACE = 19           # face buttons (the cross/circle/square/triangle discs are 19x19)

ARROWS = {  # 5x5 arrow symbols
    "up": ["00100", "01110", "11111", "00100", "00100"],
    "down": ["00100", "00100", "11111", "01110", "00100"],
    "left": ["00100", "01100", "11111", "01100", "00100"],
    "right": ["00100", "00110", "11111", "00110", "00100"],
}
MINI_ARROWS = {
    "up": ["010", "111", "000"],
    "down": ["000", "111", "010"],
    "left": ["010", "110", "010"],
    "right": ["010", "011", "010"],
}

# Xbox-style letters: generic coloured letters on the game's charcoal disc, 2-texel strokes like the
# original symbols (8x9 texels).
FACE_LETTERS = {"A": (124, 196, 92), "B": (214, 82, 58), "X": (88, 140, 222), "Y": (230, 190, 70)}
FACE_SHAPES = {
    "A": ["00111100", "01100110", "11000011", "11000011", "11111111", "11111111", "11000011", "11000011", "11000011"],
    "B": ["11111100", "11000110", "11000110", "11111100", "11000110", "11000011", "11000011", "11000110", "11111100"],
    "X": ["11000011", "11000011", "01100110", "00111100", "00011000", "00111100", "01100110", "11000011", "11000011"],
    "Y": ["11000011", "11000011", "01100110", "00111100", "00011000", "00011000", "00011000", "00011000", "00011000"],
}


def blank(w, h):
    return Image.new("RGBA", (w, h), (4, 2, 4, 0))


def bits(img, x, y, rows, rgb):
    for j, row in enumerate(rows):
        for i, b in enumerate(row):
            if b == "1":
                put(img, x + i, y + j, rgb)


# ---------------------------------------------------------------------------------------------- keycaps

def cap_body(img, x0, y0, w, h, radius=2.0, highlight=False):
    """A keycap: bevelled skirt, a slightly lighter dished top face, dark bottom row and a shadow row."""
    body_h = h - 1
    shape = round_rect_inside(w, body_h, radius)
    face_lo = (98, 98, 100) if not highlight else GOLD_MID
    face_hi = (106, 106, 110) if not highlight else GOLD_LIGHT

    def shade(u, v, x, y):
        g = noise(x0 + x, y0 + y, 7) * 3
        if y == 0:
            c = PLATE_EDGE_LIGHT
        elif y >= body_h - 2:
            c = PLATE_BOTTOM if y == body_h - 2 else PLATE_EDGE_DARK
        elif x == 0:
            c = PLATE_EDGE_DARK
        elif x == w - 1:
            c = PLATE_EDGE_LIGHT
        elif 2 <= x <= w - 3 and 1 <= y <= body_h - 3:
            c = lerp(face_lo, face_hi, (x / max(1, w - 1)) * 0.6 + (1 - y / body_h) * 0.4)
        else:
            c = PLATE_SHADE if x < w // 2 else PLATE_FACE
        return tuple(max(0, min(255, int(ch + g))) for ch in c)

    fill_shape(img, (x0, y0, w, body_h), shape, shade)
    # The soft shadow row under the plate (alpha 0xCE in the original).
    for x in range(1, w - 1):
        put(img, x0 + x, y0 + body_h, PLATE_SHADOW, 206)
    put(img, x0, y0 + body_h, PLATE_SHADOW, 120)
    put(img, x0 + w - 1, y0 + body_h, PLATE_SHADOW, 120)


def keycap(label, small=None, min_w=CAP_H):
    if small is None:
        small = len(label) > 3
    tw, th = pixel_font.text_size(label, small)
    w = max(min_w, tw + (8 if small else 7))
    img = blank(w, CAP_H)
    cap_body(img, 0, 0, w, CAP_H)
    ty = 3 if small else 2
    pixel_font.draw_text(img, (w - tw) // 2 + (1 if small else 0), ty, label, INK, INK_EDGE, small, put=put)
    return img


def arrow_cap(direction):
    img = blank(CAP_H, CAP_H)
    cap_body(img, 0, 0, CAP_H, CAP_H)
    bits(img, 4, 3, ARROWS[direction], INK)
    return img


def pair(a, b):
    img = blank(a.width + b.width + 1, max(a.height, b.height))
    img.alpha_composite(a, (0, 0))
    img.alpha_composite(b, (a.width + 1, 0))
    return img


def mini_cap(img, x, y, symbol=None, letter=None):
    cap_body(img, x, y, 9, 8, radius=1.5)
    if symbol:
        bits(img, x + 3, y + 2, MINI_ARROWS[symbol], INK)
    if letter:
        pixel_font.draw_text(img, x + 3, y + 1, letter, INK, INK_EDGE, small=True, put=put)


def cluster(top, bottom, letters=False):
    img = blank(29, 16)
    if letters:
        mini_cap(img, 10, 0, letter=top)
        for i, l in enumerate(bottom):
            mini_cap(img, i * 10, 8, letter=l)
    else:
        mini_cap(img, 10, 0, symbol=top)
        for i, s in enumerate(bottom):
            mini_cap(img, i * 10, 8, symbol=s)
    return img


# ---------------------------------------------------------------------------------------------- pads

def disc(size=FACE, seed=1):
    img = blank(size, size)
    r = size / 2.0 - 0.3
    c = size / 2.0
    inside = circle_inside(c, c, r)
    rim = circle_inside(c, c, r - 1.1)

    def shade(u, v, x, y):
        rgb = body_shade(u, v, x, y, seed)
        if not rim(x + 0.5, y + 0.5):  # soft dark rim, lighter on the lit top-right
            rgb = lerp(rgb, (12, 12, 13) if u + (1 - v) < 1.0 else (90, 86, 74), 0.55)
        return rgb

    fill_shape(img, (0, 0, size, size), inside, shade)
    return img


def face(letter):
    """A charcoal disc with a 2-texel coloured letter, shaded darker along its lower-left edges."""
    img = disc(seed=ord(letter))
    rgb = FACE_LETTERS[letter]
    shape = FACE_SHAPES[letter]
    dark = lerp(rgb, (10, 10, 12), 0.45)
    ox, oy = 6, 5
    for j, row in enumerate(shape):
        for i, b in enumerate(row):
            if b != "1":
                continue
            below = j + 1 < len(shape) and shape[j + 1][i] == "1"
            left = i > 0 and row[i - 1] == "1"
            put(img, ox + i, oy + j, rgb if (below and left) or j == 0 else lerp(rgb, dark, 0.5))
            if j + 1 >= len(shape) or shape[j + 1][i] != "1":
                put(img, ox + i, oy + j + 1, (12, 12, 14), 150)
    return img


def plate(label, w=22, h=CAP_H, top_radius=2.0):
    """Shoulder plate like L1/R1 (bumpers) or the taller L2/R2 (triggers, rounded top)."""
    img = blank(w, h)
    body_h = h - 1
    base = round_rect_inside(w, body_h, 2.0)
    top = round_rect_inside(w, body_h * 2, top_radius)

    def inside(px, py):
        return base(px, py) and (py > top_radius or top(px, py))

    def shade(u, v, x, y):
        g = noise(x, y, 11) * 4
        if y == 0:
            c = PLATE_EDGE_LIGHT
        elif y == body_h - 1:
            c = PLATE_BOTTOM
        elif x == 0:
            c = PLATE_EDGE_DARK
        elif x >= w - 2:
            c = PLATE_EDGE_LIGHT
        else:
            c = lerp(PLATE_SHADE, PLATE_FACE, u)
        return tuple(max(0, min(255, int(ch + g))) for ch in c)

    fill_shape(img, (0, 0, w, body_h), inside, shade)
    for x in range(1, w - 1):
        put(img, x, body_h, PLATE_SHADOW, 206)
    tw, th = pixel_font.text_size(label)
    pixel_font.draw_text(img, (w - tw) // 2, (body_h - th) // 2 + (1 if h > CAP_H else 0), label, INK, INK_EDGE, put=put)
    return img


def small_plate(label):
    tw, _ = pixel_font.text_size(label, small=True)
    img = blank(tw + 8, CAP_H)
    cap_body(img, 0, 0, tw + 8, CAP_H, radius=4.0)
    pixel_font.draw_text(img, 4, 3, label, INK, INK_EDGE, small=True, put=put)
    return img


def stick(label):
    size = 18
    img = blank(size, size)
    c = size / 2.0
    inside = circle_inside(c, c, c - 0.3)

    def shade(u, v, x, y):  # a lit sphere: highlight towards the top right
        d = ((u - 0.68) ** 2 + (v - 0.3) ** 2) ** 0.5
        rgb = lerp((150, 150, 154), (36, 36, 38), min(1.0, d * 1.5))
        g = noise(x, y, 23) * 4
        return tuple(max(0, min(255, int(ch + g))) for ch in rgb)

    fill_shape(img, (0, 0, size, size), inside, shade)
    tw, th = pixel_font.text_size(label)
    pixel_font.draw_text(img, (size - tw) // 2, (size - th) // 2, label, INK, (60, 60, 64), put=put)
    return img


def dpad(lit):
    """A generic plus-shaped pad; the arms in `lit` are gold."""
    size = FACE
    img = blank(size, size)
    arm, span = 7, size
    lo, hi = (size - arm) / 2.0, (size + arm) / 2.0

    def plus(px, py):
        return (lo <= px <= hi and 0.3 <= py <= span - 0.3) or (lo <= py <= hi and 0.3 <= px <= span - 0.3)

    def which(x, y):
        if lo <= x + 0.5 <= hi and lo <= y + 0.5 <= hi:
            return "centre"
        if y + 0.5 < lo:
            return "up"
        if y + 0.5 > hi:
            return "down"
        return "left" if x + 0.5 < lo else "right"

    def shade(u, v, x, y):
        part = which(x, y)
        g = noise(x, y, 31) * 5
        t = max(0.0, min(1.0, (u + (1 - v)) * 0.5))
        if part in lit:
            rgb = lerp(GOLD_DARK, GOLD_LIGHT, t)
        elif part == "centre":
            rgb = (28, 28, 30)
        else:
            rgb = lerp(BODY_DARK, (96, 96, 100), t)
        return tuple(max(0, min(255, int(ch + g))) for ch in rgb)

    fill_shape(img, (0, 0, size, size), plus, shade)
    return img


# ---------------------------------------------------------------------------------------------- mouse

def mouse_body(img, x0, y0, w, h, lit=()):
    """Mouse seen from above: rounded body, two buttons split at the centre, a wheel; `lit` parts are gold."""
    split = int(h * 0.45)
    body = round_rect_inside(w, h, w / 2.0)
    mid = w / 2.0

    def part(x, y):
        if mid - 1 <= x + 0.5 <= mid + 1 and 2 <= y <= split - 2:
            return "wheel"
        if y < split:
            return "left" if x + 0.5 < mid else "right"
        return "body"

    def shade(u, v, x, y):
        p = part(x, y)
        g = noise(x0 + x, y0 + y, 41) * 4
        t = max(0.0, min(1.0, (u + (1 - v)) * 0.5))
        if p in lit:
            rgb = lerp(GOLD_DARK, GOLD_LIGHT, t)
        elif p == "wheel":
            rgb = (24, 24, 26)
        elif p in ("left", "right") and (y == split - 1 or abs(x + 0.5 - mid) < 0.6):
            rgb = PLATE_EDGE_DARK
        else:
            rgb = lerp((44, 44, 46), (122, 122, 128), t)
        return tuple(max(0, min(255, int(ch + g))) for ch in rgb)

    fill_shape(img, (x0, y0, w, h), body, shade)


def mouse(lit, marks=()):
    img = blank(13, FACE)
    mouse_body(img, 0, 0, 13, FACE, lit)
    for m in marks:  # wheel direction ticks
        bits(img, 5, 1 if m == "up" else 13, ["010", "111"] if m == "up" else ["111", "010"], GOLD_LIGHT)
    return img


def mouse_move():
    img = blank(FACE, FACE)
    mouse_body(img, 5, 3, 9, 13)
    for x, y, rows in ((8, 0, ["010", "111"]), (8, 17, ["111", "010"]), (0, 8, ["01", "11", "01"]),
                       (17, 8, ["10", "11", "10"])):
        bits(img, x, y, rows, GOLD_LIGHT)
    return img


def mouse_side(which):
    img = blank(13, FACE)
    mouse_body(img, 0, 0, 13, FACE)
    y = 9 if which == "x1" else 5
    for j in range(3):
        put(img, 0, y + j, GOLD_LIGHT)
        put(img, 1, y + j, GOLD_MID)
    return img


# ---------------------------------------------------------------------------------------------- sheet

def build():
    s = Sheet("prompts", width=256)
    # Xbox-style pad.
    for l in "ABXY":
        s.add("pad_" + l.lower(), face(l))
    s.add("pad_lb", plate("LB"))
    s.add("pad_rb", plate("RB"))
    s.add("pad_lt", plate("LT", h=17, top_radius=5.0))
    s.add("pad_rt", plate("RT", h=17, top_radius=5.0))
    s.add("pad_ls", stick("L"))
    s.add("pad_rs", stick("R"))
    s.add("pad_view", small_plate("VIEW"))
    s.add("pad_menu", small_plate("MENU"))
    for name, lit in (("up", {"up"}), ("down", {"down"}), ("left", {"left"}), ("right", {"right"}),
                      ("updown", {"up", "down"}), ("leftright", {"left", "right"}),
                      ("all", {"up", "down", "left", "right"})):
        s.add("dpad_" + name, dpad(lit))
    # Mouse.
    s.add("mouse_left", mouse({"left"}))
    s.add("mouse_right", mouse({"right"}))
    s.add("mouse_middle", mouse({"wheel"}))
    s.add("mouse_wheel", mouse({"wheel"}, ("up", "down")))
    s.add("mouse_wheel_up", mouse({"wheel"}, ("up",)))
    s.add("mouse_wheel_down", mouse({"wheel"}, ("down",)))
    s.add("mouse_x1", mouse_side("x1"))
    s.add("mouse_x2", mouse_side("x2"))
    s.add("mouse_move", mouse_move())
    # Keyboard clusters.
    s.add("key_arrows", cluster("up", ("left", "down", "right")))
    s.add("key_wasd", cluster("W", "ASD", letters=True))
    s.add("key_updown", pair(arrow_cap("up"), arrow_cap("down")))
    s.add("key_leftright", pair(arrow_cap("left"), arrow_cap("right")))
    for d in ("up", "down", "left", "right"):
        s.add("key_" + d, arrow_cap(d))
    # Single keys: letters, digits, function keys, punctuation, named keys.
    for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789":
        s.add("key_" + c.lower(), keycap(c))
    for n in range(1, 13):
        s.add("key_f%d" % n, keycap("F%d" % n))
    for name, label in (("minus", "-"), ("equals", "="), ("leftbracket", "["), ("rightbracket", "]"),
                        ("backslash", "\\"), ("semicolon", ";"), ("apostrophe", "'"), ("comma", ","),
                        ("period", "."), ("slash", "/"), ("grave", "`"), ("unknown", "?")):
        s.add("key_" + name, keycap(label))
    for name, label in (("return", "ENTER"), ("escape", "ESC"), ("space", "SPACE"), ("tab", "TAB"),
                        ("backspace", "BACKSPACE"), ("lshift", "SHIFT"), ("rshift", "SHIFT"), ("lctrl", "CTRL"),
                        ("rctrl", "CTRL"), ("lalt", "ALT"), ("ralt", "ALT"), ("capslock", "CAPS"),
                        ("insert", "INS"), ("delete", "DEL"), ("home", "HOME"), ("end", "END"),
                        ("pageup", "PGUP"), ("pagedown", "PGDN")):
        s.add("key_" + name, keycap(label, small=len(label) > 3, min_w=19 if name == "space" else CAP_H))
    for c, name in (("0", "0"), ("1", "1"), ("2", "2"), ("3", "3"), ("4", "4"), ("5", "5"), ("6", "6"), ("7", "7"),
                    ("8", "8"), ("9", "9"), ("+", "plus"), ("-", "minus"), ("*", "multiply"), ("/", "divide"),
                    (".", "period")):
        s.add("key_kp_" + name, keycap("KP" + c, small=True))
    s.add("key_kp_enter", keycap("KPENT", small=True))
    return s.save()


if __name__ == "__main__":
    print(build())
