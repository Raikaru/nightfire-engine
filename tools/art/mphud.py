"""Extended multiplayer HUD art (16-slot rule set): radar markers, name-tag plate, kill-feed and scoreboard icons.

python3 tools/art/mphud.py  ->  assets/ui/mphud.png + assets/ui/mphud.txt
Markers are white with a dark rim so the HUD tints them with the team / participant colour; plates are the agent
panel's translucent black with the glyph bevel. The PS2 / GC-Xbox rule sets keep the game's own HUD.
"""

import math

from PIL import Image

import style as s

WHITE = (255, 255, 255)


def icon(w, h):
    return Image.new("RGBA", (w, h), s.OUTLINE + (0,))


def tinted(img, inside, w, h):
    s.fill_shape(img, (0, 0, w, h), inside, lambda u, v, x, y: s.lerp(WHITE, (190, 190, 190), v))
    s.outline(img, 100, alpha=220)
    return img


def marker_dot():
    return tinted(icon(7, 7), s.circle_inside(3.5, 3.5, 2.6), 7, 7)


def marker_triangle():
    """Phoenix team marker (shape keeps teams apart without colour)."""
    return tinted(icon(9, 8), lambda px, py: py >= 1 and py <= 7 and abs(px - 4.5) <= (py - 1) * 0.62, 9, 8)


def marker_square():
    """MI6 team marker."""
    return tinted(icon(7, 7), lambda px, py: 1 <= px <= 6 and 1 <= py <= 6, 7, 7)


def marker_self():
    """The viewer's own position: an open chevron."""
    def inside(px, py):
        d = abs(px - 4.5) * 0.9
        return 1.0 <= py - d <= 3.2 and py <= 7
    return tinted(icon(9, 8), inside, 9, 8)


def plate():
    """Name / kill-feed / scoreboard row plate: 9-slice source, 3-texel corners."""
    img = icon(16, 12)
    s.fill_shape(img, (0, 0, 16, 12), s.round_rect_inside(16, 12, 3.0),
                 lambda u, v, x, y: s.lerp((24, 20, 22), s.PANEL, v))
    px = img.load()
    for y in range(12):
        for x in range(16):
            r, g, b, a = px[x, y]
            px[x, y] = (r, g, b, min(a, 200))
    for x in range(3, 13):
        s.put(img, x, 0, s.PLATE_EDGE_LIGHT, 150)
        s.put(img, x, 11, s.OUTLINE, 220)
    return img


def kill_arrow():
    """Kill-feed separator: a fired round (casing + tip) pointing at the victim."""
    img = icon(13, 7)
    s.fill_shape(img, (0, 0, 13, 7), lambda px, py: (1 <= px <= 8 and 1.5 <= py <= 5.5)
                 or (8 <= px <= 12 and abs(py - 3.5) <= (12 - px) * 0.55),
                 lambda u, v, x, y: s.lerp(s.GOLD_LIGHT, s.GOLD_DARK, v))
    for y in range(2, 6):
        s.put(img, 3, y, s.GOLD_DARK)
    s.outline(img, 100, alpha=200)
    return img


def kill_self():
    """Suicide / environment death: a crossed-out circle."""
    img = icon(9, 9)
    def inside(px, py):
        r = math.hypot(px - 4.5, py - 4.5)
        return (2.6 <= r <= 3.9) or (r <= 3.9 and abs((px - 4.5) - (py - 4.5)) <= 0.8)
    s.fill_shape(img, (0, 0, 9, 9), inside, lambda u, v, x, y: s.lerp(s.LABEL, s.ITEM, v))
    s.outline(img, 100, alpha=200)
    return img


def bot_tag():
    """Scoreboard marker for bot participants."""
    img = icon(15, 9)
    s.fill_shape(img, (0, 0, 15, 9), s.round_rect_inside(15, 9, 2.0),
                 lambda u, v, x, y: s.lerp(s.PLATE_FACE, s.PLATE_BOTTOM, v))
    s.bevel(img, (0, 0, 15, 9))
    s.pixel_text(img, 2, 2, "BOT", s.INK)
    return img


def main():
    sheet = s.Sheet("mphud", width=128)
    sheet.add("marker_dot", marker_dot())
    sheet.add("marker_triangle", marker_triangle())
    sheet.add("marker_square", marker_square())
    sheet.add("marker_self", marker_self())
    sheet.add("plate", plate())
    sheet.add("kill_arrow", kill_arrow())
    sheet.add("kill_self", kill_self())
    sheet.add("bot_tag", bot_tag())
    print(sheet.save())


if __name__ == "__main__":
    main()
