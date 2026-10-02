"""Record per-logic-frame player state AND the pad input the game saw, from a running PCSX2
(ACTION.ELF) over PINE, optionally driving a scripted input scenario through vpad.py.

Usage: python3 trace.py out.jsonl [--frames 150] [--load-slot 2] [--script scenario.txt]

Frame protocol (measured, see docs/oracle.md): each logic update begins by incrementing
GameState+0x3C (the frame counter) and ends by incrementing GameState+0x30, so an update N is
complete exactly when GameState+0x30 == N-1 (+0x30 == N-2 while it is still running). A sample is
kept only if that holds both before and after the batched read, so records are never torn.

Each line: {"frame", "pos", "yaw", "rate", "pad": {"w", "s", ...}, "act", "flg", "obj", "bl", "cb", "vw"}
  rate     FRAME_RATE (60 / vsyncs per logic frame): the frame duration the physics used
  pad.w    Sony libpad word (tSlot[0]+0x122, active-high: Up=0x1000 Cross=0x40 ...)
  pad.s    [rx, ry, lx, ly] after psiInput_PollDevices' dead-zone compensation (tSlot+0x128)
  act      PlayerSetting[0].action floats (Input_Actionf), flg the per-action flag bytes
  obj/bl/cb/vw  raw hex of player obj_tag[0:0x100], BLData ranges, collbody[0:0xd0], viewer[0:0x100]
--anim additionally dumps the player's animation state each frame ("anim": sAnimObject +0x70.., the list of
sAnimScript records reached through sAnimObject+0x9C / script+0x48 (0xC0 bytes each), the AnimSet state
BLData+0x890 points at, and MPSettings+0x180), read right after the sample and kept only if the frame is
still the same afterwards.
Scenario script: lines "<frame offset> <vpad command...>", e.g. "30 axis LY -1", "90 release".
"""
import argparse
import json
import os
import socket
import struct
import time

from pine import Pine

GAMESTATE = 0x2A3768
DONE_COUNTER = GAMESTATE + 0x30   # ++ at the end of each logic update
FRAME_COUNTER = GAMESTATE + 0x3C  # ++ at the start of each logic update
GLB_PLAYERS = 0x2D88E0
GLB_VIEWER = 0x2D88B0
FRAME_RATE = 0x30D0D0   # float, GS_SetRefreshRate: 60 / vsyncs per logic frame
TSLOT0 = 0x245680
PLAYER_SETTING = 0x2A38C8
MPSETTINGS = 0x2A47A0   # symbol MPSettings (+0x180 = multiplayer flag)
BL_RANGES = {"bl0": (0x0, 0x80), "bl100": (0x100, 0x40), "bl740": (0x740, 0xC0), "bl8a0": (0x8A0, 0xD0),
             "bl930": (0x930, 0x40)}


def vpad(*words):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(os.path.join(os.environ["XDG_RUNTIME_DIR"], "nf-vpad.sock"))
    s.sendall((" ".join(map(str, words)) + "\n").encode())
    reply = s.recv(256)
    s.close()
    return reply


def read_pointers(pine):
    player = struct.unpack("<I", pine.read_block(GLB_PLAYERS, 4))[0]
    if not player:
        raise SystemExit("glb_players[0] is null: not in an ACTION.ELF level")
    obj = pine.read_block(player, 0x100)
    bl, cb = struct.unpack_from("<I", obj, 0xE0)[0], struct.unpack_from("<I", obj, 0xDC)[0]
    viewer = struct.unpack("<I", pine.read_block(GLB_VIEWER, 4))[0]
    return player, bl, cb, viewer


def sample(pine, ptrs):
    """One batched read; returns (frame, record) or None if the update was in flight."""
    player, bl, cb, viewer = ptrs
    ranges = [(DONE_COUNTER, 4), (FRAME_COUNTER, 4), (player, 0x100), (cb, 0xD0), (viewer, 0x100),
              (TSLOT0 + 0x120, 0x30), (PLAYER_SETTING + 0x14, 0xA0), (PLAYER_SETTING + 0x104, 0x28),
              (FRAME_RATE, 4)]
    ranges += [(bl + a, n) for a, n in BL_RANGES.values()]
    ranges += [(DONE_COUNTER, 4), (FRAME_COUNTER, 4)]
    d = pine.read_ranges(ranges)
    done0, frame0, done1, frame1 = (struct.unpack("<I", d[i])[0] for i in (0, 1, -2, -1))
    if frame0 != frame1 or done0 != done1 or done0 != frame0 - 1:
        return None
    obj, cbd, vw, pad, act, flg, rate = d[2:9]
    pos = struct.unpack_from("<3f", obj, 0x30)
    yaw = struct.unpack_from("<f", obj, 0x54)[0]
    rec = {
        "frame": frame0,
        "pos": pos,
        "yaw": yaw,
        "rate": struct.unpack("<f", rate)[0],
        "pad": {"w": struct.unpack_from("<H", pad, 2)[0], "s": list(pad[8:12])},
        "act": struct.unpack("<40f", act),
        "flg": flg.hex(),
        "obj": obj.hex(),
        "cb": cbd.hex(),
        "vw": vw.hex(),
    }
    for name, chunk in zip(BL_RANGES, d[9:-2]):
        rec[name] = chunk.hex()
    return frame0, rec


def read_anim(pine, ptrs):
    """sAnimScript list of the player plus the AnimSet state; None if it changed under us (caller checks)."""
    player, bl, cb, viewer = ptrs
    head, animset, mp = struct.unpack("<3I", b"".join(pine.read_ranges([(cb + 0x9C, 4), (bl + 0x890, 4), (MPSETTINGS + 0x180, 4)])))
    scripts, addr = [], head
    while addr and len(scripts) < 32:
        blob, = pine.read_ranges([(addr, 0xC0)])
        scripts.append({"addr": addr, "raw": blob.hex()})
        addr = struct.unpack_from("<I", blob, 0x48)[0]
    state = pine.read_ranges([(animset, 0x40)])[0].hex() if animset else ""
    return {"mp": mp, "animset": animset, "animset_raw": state, "scripts": scripts}


def load_script(path):
    steps = []
    for line in open(path):
        line = line.split("#")[0].split()
        if line:
            steps.append((int(line[0]), line[1:]))
    return sorted(steps, key=lambda s: s[0])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--frames", type=int, default=150)
    ap.add_argument("--load-slot", type=int)
    ap.add_argument("--script")
    ap.add_argument("--anim", action="store_true", help="also dump the animation script list per frame")
    ap.add_argument("--timeout", type=float, default=120.0, help="wall-clock limit in seconds")
    args = ap.parse_args()

    pine = Pine()
    steps = load_script(args.script) if args.script else []
    vpad("release")
    if args.load_slot is not None:
        before = pine.read32(FRAME_COUNTER)
        pine.load_state(args.load_slot)
        # The load is asynchronous: wait for the frame counter to jump to the saved value (or, if the
        # saved value is close to the current one, give it a fixed grace period).
        start = time.monotonic()
        while abs(pine.read32(FRAME_COUNTER) - before) <= 20 and time.monotonic() - start < 4.0:
            time.sleep(0.05)
        time.sleep(1.0)
    ptrs = read_pointers(pine)

    first = last = None
    records, missed = [], 0
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        s = sample(pine, ptrs)
        if s is None:
            continue
        frame, rec = s
        if frame == last:
            continue
        if args.anim:
            rec["anim"] = read_anim(pine, ptrs)
            if pine.read32(FRAME_COUNTER) != frame or pine.read32(DONE_COUNTER) != frame - 1:
                continue   # the next update began while we read: retry on the next poll
        if first is None:
            first = frame
        elif frame != last + 1:
            missed += frame - last - 1
        last = frame
        records.append(rec)
        rel = frame - first
        while steps and steps[0][0] <= rel:
            vpad(*steps.pop(0)[1])
        if rel >= args.frames:
            break
    vpad("release")
    with open(args.out, "w") as f:
        for rec in records:
            f.write(json.dumps(rec) + "\n")
    print(f"{len(records)} frames {first}..{last}, missed {missed}, player @ {ptrs[0]:#x} -> {args.out}")


if __name__ == "__main__":
    main()
