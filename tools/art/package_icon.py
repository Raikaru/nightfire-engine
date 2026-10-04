"""Generate the original, unbranded application icon used by packages.

Usage: python3 tools/art/package_icon.py [output.png]
Only the Python standard library is required.
"""

import math
import pathlib
import struct
import sys
import zlib

SIZE = 256
TRANSPARENT = (0, 0, 0, 0)
BACKGROUND = (18, 34, 49, 255)
FRAME = (60, 107, 119, 255)
GOLD = (229, 181, 83, 255)
LIGHT = (245, 238, 211, 255)


def pixel(x, y):
    edge = 18 <= x < SIZE - 18 and 18 <= y < SIZE - 18
    if edge:
        dx = max(0, 36 - x, x - (SIZE - 37))
        dy = max(0, 36 - y, y - (SIZE - 37))
        if dx * dx + dy * dy <= 18 * 18:
            border = x < 27 or x >= SIZE - 27 or y < 27 or y >= SIZE - 27
            color = FRAME if border else BACKGROUND
            cx, cy = 128, 119
            radius = math.hypot(x - cx, y - cy)
            if abs(radius - 72) <= 7:
                color = GOLD
            if 91 <= y <= 149 and 110 <= x <= 110 + 3 * min(y - 91, 149 - y) // 2:
                color = LIGHT
            return color
    return TRANSPARENT


def png_chunk(kind, payload):
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def generate(destination):
    rows = bytearray()
    for y in range(SIZE):
        rows.append(0)
        for x in range(SIZE):
            rows.extend(pixel(x, y))
    data = b"\x89PNG\r\n\x1a\n"
    data += png_chunk(b"IHDR", struct.pack(">2I5B", SIZE, SIZE, 8, 6, 0, 0, 0))
    data += png_chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    data += png_chunk(b"IEND", b"")
    pathlib.Path(destination).write_bytes(data)


if __name__ == "__main__":
    output = pathlib.Path(sys.argv[1]) if len(sys.argv) == 2 else pathlib.Path(__file__).resolve().parents[2] / "assets" / "nightfire.png"
    generate(output)
