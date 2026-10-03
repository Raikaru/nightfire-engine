"""Accessibility crosshairs (opt-in on the Settings screen; the default is the game's own crosshair).

python3 tools/art/access.py  ->  assets/ui/access.png + assets/ui/access.txt
White shapes with a 1-texel dark rim so they read on bright and dark scenes; the HUD tints them like the game's red
crosshair. Names follow ui::CrosshairStyle.
"""

import math

from PIL import Image

import style as s

WHITE = (255, 255, 255)


def shape(w, h, inside):
    img = Image.new("RGBA", (w, h), s.OUTLINE + (0,))
    s.fill_shape(img, (0, 0, w, h), inside, lambda u, v, x, y: WHITE)
    s.outline(img, 90, alpha=230)
    return img


def cross():
    c = 10.5
    def inside(px, py):
        dx, dy = abs(px - c), abs(py - c)
        return (dx <= 1.0 and 3.5 <= dy <= 9.5) or (dy <= 1.0 and 3.5 <= dx <= 9.5)
    return shape(21, 21, inside)


def dot():
    return shape(7, 7, s.circle_inside(3.5, 3.5, 2.0))


def ring():
    c = 9.5
    return shape(19, 19, lambda px, py: abs(math.hypot(px - c, py - c) - 7.0) <= 1.0 or math.hypot(px - c, py - c) <= 1.3)


def chevron():
    def inside(px, py):
        d = abs(px - 7.5)
        return 0 <= (py - 2.0) - d * 0.75 <= 2.0 and d <= 6.5
    return shape(15, 10, inside)


def main():
    sheet = s.Sheet("access", width=64)
    sheet.add("crosshair_cross", cross())
    sheet.add("crosshair_dot", dot())
    sheet.add("crosshair_ring", ring())
    sheet.add("crosshair_chevron", chevron())
    print(sheet.save())


if __name__ == "__main__":
    main()
