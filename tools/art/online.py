"""Online UI art: server-browser icons, ruleset badges, the busy spinner and the chat indicator.

python3 tools/art/online.py  ->  assets/ui/online.png + assets/ui/online.txt
Sizes are canvas units (drawn 1:1 on the 640x448 UI canvas, next to 14-unit font 2 text).
"""

import math

from PIL import Image

import style as s


def icon(w, h):
    return Image.new("RGBA", (w, h), s.OUTLINE + (0,))


def ping(level):
    """Four rising bars (2 wide, 1 apart); lit bars in the label colour, a single bar in the item colour."""
    img = icon(13, 11)
    lit = s.LABEL if level >= 2 else s.ITEM
    for i in range(4):
        h = 3 + 2 * i
        x = 1 + 3 * i
        for y in range(10 - h, 10):
            for dx in range(2):
                if i < level:
                    s.put(img, x + dx, y, s.lerp(lit, (255, 255, 255), 0.25) if y == 10 - h else lit)
                else:
                    s.put(img, x + dx, y, s.PLATE_EDGE_DARK, 200)
    s.outline(img, 100, alpha=170)
    return img


def lock():
    img = icon(11, 13)
    # Shackle: a 2-texel ring segment over the body.
    s.fill_shape(img, (0, 0, 11, 8),
                 lambda px, py: 2.2 <= math.hypot(px - 5.5, py - 5.5) <= 4.0 and py <= 5.6,
                 lambda u, v, x, y: s.lerp(s.PLATE_EDGE_LIGHT, s.PLATE_EDGE_DARK, v))
    s.fill_shape(img, (1, 5, 9, 7), s.round_rect_inside(9, 7, 1.5),
                 lambda u, v, x, y: s.lerp(s.GOLD_LIGHT, s.GOLD_DARK, v * 0.9))
    s.put(img, 5, 7, s.OUTLINE)
    s.put(img, 5, 8, s.OUTLINE)
    s.put(img, 5, 9, s.OUTLINE, 160)
    s.outline(img, 100, alpha=170)
    return img


def source_lan():
    """Three linked stations: the local network."""
    img = icon(13, 12)
    for x0, y0 in ((0, 0), (8, 0), (4, 7)):
        s.fill_shape(img, (x0, y0, 5, 5), s.round_rect_inside(5, 5, 1),
                     lambda u, v, x, y: s.lerp(s.LABEL, s.ITEM, v * 0.6))
    for x in range(5, 8):
        s.put(img, x, 2, s.LABEL)
    for i in range(3):
        s.put(img, 3 + i // 2, 5 + i, s.LABEL)
        s.put(img, 9 - i // 2, 5 + i, s.LABEL)
    s.outline(img, 100, alpha=170)
    return img


def source_master():
    """A globe: the master server list."""
    img = icon(12, 12)
    s.fill_shape(img, (0, 0, 12, 12), s.circle_inside(6, 6, 5.6),
                 lambda u, v, x, y: s.lerp(s.LABEL, s.ITEM, (u + v) * 0.35))
    for y in range(1, 11):
        s.put(img, 6, y, s.OUTLINE, 150)
    for x in range(1, 11):
        s.put(img, x, 6, s.OUTLINE, 150)
    s.fill_shape(img, (0, 0, 12, 12), lambda px, py: abs(math.hypot((px - 6) * 2.0, py - 6) - 5.0) < 0.5,
                 lambda u, v, x, y: s.OUTLINE)
    s.outline(img, 100, alpha=170)
    return img


def source_favourite():
    img = icon(13, 12)

    def star(px, py):
        a = math.atan2(py - 6.6, px - 6.5) + math.pi / 2
        r = math.hypot(px - 6.5, py - 6.6)
        k = (math.cos(5 * a) + 1) * 0.5
        return r <= 2.6 + 3.6 * k ** 2
    s.fill_shape(img, (0, 0, 13, 12), star, lambda u, v, x, y: s.lerp(s.GOLD_LIGHT, s.GOLD_DARK, v))
    s.outline(img, 100, alpha=170)
    return img


def badge(text, face, edge):
    """Rule-set badge: an R1-style plate (0x03000075) with the name in the 4x7 font."""
    w = s.pixel_text_width(text) + 6
    img = icon(w, 11)
    s.fill_shape(img, (0, 0, w, 11), s.round_rect_inside(w, 11, 2.0),
                 lambda u, v, x, y: s.lerp(face, edge, v * 0.8))
    s.bevel(img, (0, 0, w, 11))
    s.pixel_text(img, 3, 2, text, s.INK)
    return img


def modified():
    """A gear: the server changed rules from the ruleset defaults."""
    img = icon(11, 11)

    def gear(px, py):
        a = math.atan2(py - 5.5, px - 5.5)
        r = math.hypot(px - 5.5, py - 5.5)
        teeth = 4.9 if math.cos(6 * a) > 0.2 else 3.9
        return 1.5 <= r <= teeth
    s.fill_shape(img, (0, 0, 11, 11), gear, lambda u, v, x, y: s.lerp(s.LABEL, s.ITEM, v))
    s.outline(img, 100, alpha=170)
    return img


def spinner(frame, frames=12):
    """Twelve radial ticks; the bright head moves one tick per frame and drags a fading tail."""
    img = icon(24, 24)
    for k in range(frames):
        age = (frame - k) % frames
        t = age / (frames - 1)
        rgb = s.lerp(s.LABEL, s.ITEM, min(1.0, t * 1.6))
        a = int(255 * max(0.18, 1.0 - t))
        ang = 2 * math.pi * k / frames - math.pi / 2
        ca, sa = math.cos(ang), math.sin(ang)

        def tick(px, py, ca=ca, sa=sa):
            dx, dy = px - 12, py - 12
            along = dx * ca + dy * sa
            across = -dx * sa + dy * ca
            return 6.0 <= along <= 11.0 and abs(across) <= 1.25
        for y in range(24):
            for x in range(24):
                c = s.coverage(tick, x, y)
                if c > 0:
                    s.put(img, x, y, rgb, int(a * c))
    s.outline(img, 60, alpha=120)
    return img


def chat():
    img = icon(15, 13)
    s.fill_shape(img, (0, 0, 15, 10), s.round_rect_inside(15, 10, 3.0),
                 lambda u, v, x, y: s.lerp(s.LABEL, s.lerp(s.LABEL, s.ITEM, 0.5), v))
    s.fill_shape(img, (2, 8, 5, 5), lambda px, py: px + py <= 5 and px >= 0.5,
                 lambda u, v, x, y: s.lerp(s.LABEL, s.ITEM, 0.5))
    for x in (4, 7, 10):
        s.put(img, x, 4, s.OUTLINE)
        s.put(img, x + 1, 4, s.OUTLINE)
        s.put(img, x, 5, s.OUTLINE)
        s.put(img, x + 1, 5, s.OUTLINE)
    s.outline(img, 100, alpha=170)
    return img


def main():
    sheet = s.Sheet("online")
    for level in range(5):
        sheet.add("ping_%d" % level, ping(level))
    sheet.add("lock", lock())
    sheet.add("source_lan", source_lan())
    sheet.add("source_master", source_master())
    sheet.add("source_favourite", source_favourite())
    sheet.add("badge_ps2", badge("PS2", s.PLATE_FACE, s.PLATE_BOTTOM))
    sheet.add("badge_gcxbox", badge("GC/XB", (70, 82, 120), (40, 46, 70)))
    sheet.add("badge_extended", badge("EXT", s.GOLD_MID, s.GOLD_DARK))
    sheet.add("modified", modified())
    sheet.add("chat", chat())
    for frame in range(12):
        sheet.add("spinner_%d" % frame, spinner(frame))
    print(sheet.save())


if __name__ == "__main__":
    main()
