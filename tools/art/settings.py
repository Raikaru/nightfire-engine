"""Settings screen category icons: graphics, widescreen, texture packs, audio and accessibility.

python3 tools/art/settings.py  ->  assets/ui/settings.png + assets/ui/settings.txt
Each icon is a 22x22 charcoal disc lit from the top right (the face-button bodies of 0x03000075) carrying a light
symbol; the symbols are drawn as shapes so the output is reproducible.
"""

import math

from PIL import Image

import style as s

SIZE = 22
C = SIZE / 2


def disc():
    img = Image.new("RGBA", (SIZE, SIZE), s.OUTLINE + (0,))
    s.fill_shape(img, (0, 0, SIZE, SIZE), s.circle_inside(C, C, C - 0.6),
                 lambda u, v, x, y: s.body_shade(u, v, x, y, seed=7))
    s.fill_shape(img, (0, 0, SIZE, SIZE), lambda px, py: C - 1.6 <= math.hypot(px - C, py - C) <= C - 0.6,
                 lambda u, v, x, y: s.lerp(s.PLATE_EDGE_DARK, s.PLATE_EDGE_LIGHT, max(0.0, u - v + 0.5)))
    return img


def symbol(img, inside, rgb=s.LABEL):
    s.fill_shape(img, (0, 0, SIZE, SIZE), inside, lambda u, v, x, y: s.lerp(rgb, s.ITEM, v * 0.35))


def graphics():
    """A monitor on a stand."""
    img = disc()
    frame = lambda px, py: (5 <= px <= 17 and 5.5 <= py <= 13.5) and not (6.5 <= px <= 15.5 and 7 <= py <= 12)
    stand = lambda px, py: (10 <= px <= 12 and 13.5 <= py <= 15.5) or (7.5 <= px <= 14.5 and 15.5 <= py <= 16.8)
    symbol(img, lambda px, py: frame(px, py) or stand(px, py))
    # A small horizon inside the screen.
    symbol(img, lambda px, py: 6.5 <= px <= 15.5 and 7 <= py <= 12 and py >= 12 - (px - 6.5) * 0.45 - 0.5
           and py >= 9.2 + abs(px - 12) * 0.5, s.ITEM)
    return img


def widescreen():
    """A wide frame with arrows pointing outwards."""
    img = disc()
    frame = lambda px, py: (5.5 <= px <= 16.5 and 7 <= py <= 15) and not (7 <= px <= 15 and 8.5 <= py <= 13.5)
    def arrow(px, py, tip, direction):
        d = (px - tip) * direction
        return -2.8 <= d <= 0 and abs(py - C) <= 2.8 + d
    symbol(img, lambda px, py: frame(px, py) or arrow(px, py, 3.0, -1) or arrow(px, py, 19.0, 1))
    return img


def texture_packs():
    """Three stacked texture cards, the front one filled with a checker."""
    img = disc()
    def card_edge(px, py, o):
        inside = o <= px <= o + 8 and o <= py <= o + 8
        return inside and not (o + 1.2 <= px <= o + 6.8 and o + 1.2 <= py <= o + 6.8)
    def front(px, py):
        return 9.0 <= px <= 17.0 and 9.0 <= py <= 17.0
    def checker(px, py):
        return front(px, py) and (int((px - 9.0) // 2.67) + int((py - 9.0) // 2.67)) % 2 == 0
    symbol(img, lambda px, py: (card_edge(px, py, 4.0) and not (px >= 6.5 and py >= 6.5))
           or (card_edge(px, py, 6.5) and not front(px, py)) or checker(px, py))
    symbol(img, lambda px, py: front(px, py) and not checker(px, py), s.ITEM)
    return img


def audio():
    """A speaker with two sound waves."""
    img = disc()
    body = lambda px, py: (5 <= px <= 7.5 and 9 <= py <= 13) or (7.5 <= px <= 11 and abs(py - C) <= 1.9 + (px - 7.5))
    def wave(px, py, r):
        d = math.hypot(px - 10, py - C)
        return px > 12 and abs(d - r) <= 0.75 and abs(py - C) <= r * 0.75
    symbol(img, lambda px, py: body(px, py) or wave(px, py, 4.0) or wave(px, py, 6.5))
    return img


def accessibility():
    """An eye: visual options."""
    img = disc()
    def lid(px, py):
        dx = (px - C) / 7.0
        return abs(dx) <= 1 and abs(py - C) <= 4.2 * (1 - dx * dx)
    def lid_edge(px, py):
        return lid(px, py) and not ((abs((px - C) / 5.8) <= 1) and abs(py - C) <= 3.0 * (1 - ((px - C) / 5.8) ** 2))
    symbol(img, lambda px, py: lid_edge(px, py) or math.hypot(px - C, py - C) <= 2.3)
    return img


def main():
    sheet = s.Sheet("settings", width=128)
    sheet.add("graphics", graphics())
    sheet.add("widescreen", widescreen())
    sheet.add("texture_packs", texture_packs())
    sheet.add("audio", audio())
    sheet.add("accessibility", accessibility())
    print(sheet.save())


if __name__ == "__main__":
    main()
