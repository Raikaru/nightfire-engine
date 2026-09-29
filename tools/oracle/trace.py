"""Record per-game-frame player state from a running PCSX2 (ACTION.ELF) over PINE.

Addresses come from ACTION.ELF's symbol table (USA, SLUS-20579):
  GameState      0x2a3768; +0x3c = game-logic frame counter (30 Hz logic)
  glb_players    0x2d88e0; obj_tag*[4]
Player obj_tag fields observed so far (see docs/oracle.md):
  +0x30 vec3 position, +0x40 vec3 position (copy), +0x54 f32 yaw (radians, forward = (sin, 0, cos)),
  +0x70 vec3 eye/camera base, +0x90 3x4 rotation matrix.

Usage: python3 trace.py out.jsonl [seconds] [--raw 0x200]
Each line: {"frame": n, "pos": [...], "yaw": f, "raw": "<hex of obj_tag[0:raw]>"}
Frames are sampled by polling the counter; a frame is logged once when first seen.
"""
import argparse
import json
import struct
import time

from pine import Pine

GAMESTATE = 0x2A3768
FRAME_COUNTER = GAMESTATE + 0x3C
GLB_PLAYERS = 0x2D88E0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("seconds", type=float, nargs="?", default=5.0)
    ap.add_argument("--raw", type=lambda s: int(s, 0), default=0x200, help="bytes of obj_tag to dump per frame")
    args = ap.parse_args()

    pine = Pine()
    player = struct.unpack("<I", pine.read_block(GLB_PLAYERS, 4))[0]
    if not player:
        raise SystemExit("glb_players[0] is null: not in an ACTION.ELF level")

    last = None
    missed = 0
    end = time.monotonic() + args.seconds
    with open(args.out, "w") as f:
        while time.monotonic() < end:
            frame = pine.read32(FRAME_COUNTER)
            if frame == last:
                continue
            raw = pine.read_block(player, args.raw)
            after = pine.read32(FRAME_COUNTER)
            if after != frame:
                # State changed under us mid-read; skip rather than log a torn sample.
                missed += 1
                continue
            if last is not None and frame != last + 1:
                missed += frame - last - 1
            last = frame
            pos = struct.unpack_from("<3f", raw, 0x30)
            yaw = struct.unpack_from("<f", raw, 0x54)[0]
            f.write(json.dumps({"frame": frame, "pos": pos, "yaw": yaw, "raw": raw.hex()}) + "\n")
    print(f"player obj_tag @ {player:#x}; missed/torn frames: {missed}")


if __name__ == "__main__":
    main()
