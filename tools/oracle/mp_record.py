"""MP match recorder: per-logic-frame PINE batch reads of full match state to JSONL.

Usage: python3 mp_record.py out.jsonl [--load-slot N] [--frames N] [--script s.txt]
         [--full-every 30] [--timeout S]

Superset of tools/oracle/trace.py (single-player movement columns kept where
they overlap: frame/pos/yaw/rate/pad/act/flg) plus, per frame: RNG words,
MPSettings slice, MPGame slots + globals, per-participant pos/yaw/pitch/state/
health/armour/weapon/aim/foot, per-bot drone health + BOT_vars goals/caches/
clips/reserves, pickups, switch channels; objective ext blobs every --full-every
frames.

Speed design: ONE PINE transaction per frame attempt, with the torn-sample
counters (GS_DONE == GS_FRAME-1) at both ends of the same batch, exactly like
trace.py. Pointer-dependent addresses (BLData/collbody/Drone/PICKUPINFO) come
from the previous frame's cache; when an obj pointer or the pickup count
changes the frame is recorded core-only with "resync":1 and the caches are
re-resolved (respawns reallocate objects). Scenario script lines:
"<frame offset> <vpad command...>" (see trace.py).
"""

import argparse
import json
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mp_addrs as A
from pine import Pine


def vpad(*words):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "nf-vpad.sock"))
    s.sendall((" ".join(map(str, words)) + "\n").encode())
    reply = s.recv(256)
    s.close()
    return reply


def load_script(path):
    steps = []
    for line in open(path):
        line = line.split("#")[0].split()
        if line:
            steps.append((int(line[0]), line[1:]))
    return sorted(steps, key=lambda s: s[0])


FULL_BLOBS = [
    ("flags", A.FLAGS, 0x120), ("bases", A.BASES, 0x120),
    ("uplinks", A.UPLINKS, 0x480), ("demo", A.DEMOLITION, 0x90),
    ("prot", A.PROTECTION, 0x90), ("ge", A.GOLDENEYE, 0x240),
    ("bp", A.BLUEPRINT, 0x90), ("esp", A.ESPONAGE_BASE, 0x120),
    ("hill", A.HILL, 0x90),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--frames", type=int, default=9000)
    ap.add_argument("--load-slot", type=int)
    ap.add_argument("--script")
    ap.add_argument("--full-every", type=int, default=30)
    ap.add_argument("--timeout", type=float, default=1200.0)
    args = ap.parse_args()

    pine = Pine()
    steps = load_script(args.script) if args.script else []
    vpad("release")
    if args.load_slot is not None:
        before = pine.read32(A.GS_FRAME_START)
        pine.load_state(args.load_slot)
        start = time.monotonic()
        while abs(pine.read32(A.GS_FRAME_START) - before) <= 20 and time.monotonic() - start < 4.0:
            time.sleep(0.05)
        time.sleep(2.0)

    n_humans = struct.unpack("<I", pine.read_block(A.MPSETTINGS + A.MPS_HUMANS, 4))[0]
    out = open(args.out, "w", buffering=1)
    cache = {"objs": [0] * 8, "bl": {}, "cb": {}, "drone": {}, "pinfo": {}, "pkcount": -1}

    def resolve(pine):
        """(Re)resolve all pointer caches with unguarded reads; caller retries."""
        mg = pine.read_block(A.MPGAME, 0x1D0)
        objs = [struct.unpack_from("<I", mg, s * A.MP_SLOT_STRIDE + A.MPG_OBJ)[0] for s in range(8)]
        cache["objs"] = objs
        for s in range(n_humans):
            if objs[s]:
                od = pine.read_block(objs[s], 0x100)
                cache["bl"][s] = struct.unpack_from("<I", od, A.OBJ_BL)[0]
                cache["cb"][s] = struct.unpack_from("<I", od, A.OBJ_COLL)[0]
        for k in range(4):
            if objs[4 + k]:
                cache["drone"][k] = struct.unpack("<I", pine.read_block(
                    A.BOT_VARS + k * A.BOT_VARS_STRIDE + A.BOT_DRONE + 4, 4))[0]
        mpk = pine.read_block(A.MPPICKUPS, 64 * A.MPPICKUP_STRIDE)
        pinfo = {}
        for i in range(64):
            obj = struct.unpack_from("<I", mpk, i * A.MPPICKUP_STRIDE)[0]
            if obj:
                pos = struct.unpack_from("<4f", mpk, i * A.MPPICKUP_STRIDE + A.MPPICKUP_POS)
                info = struct.unpack("<I", pine.read_block(obj + A.PICKUPINFO_OFF, 4))[0]
                pinfo[obj] = (info, [round(v, 2) for v in pos[:3]])
        cache["pinfo"] = pinfo
        cache["pkcount"] = struct.unpack("<H", pine.read_block(A.MPSETTINGS + A.MPS_PICKUP_COUNT, 2))[0]
        return objs

    resolve(pine)
    first = last = None
    last_new = time.monotonic()
    records, missed, resyncs = [], 0, 0
    deadline = time.monotonic() + args.timeout
    stalled_warned = False

    while time.monotonic() < deadline:
        if time.monotonic() - last_new > 45.0 and not stalled_warned:
            # No new frame in 45 s: the match is over (states 4/5 freeze the
            # update counters) or a long load is running. Confirm and leave.
            st = pine.read32(A.MPGAME + A.MPG_STATE)
            if st in (4, 5):
                print(f"  ... match over (state {st}), exiting", flush=True)
                break
            print("  ... stalled 45 s, still waiting", flush=True)
            stalled_warned = True
        ranges, tags = [], []
        ranges += [(A.GS_DONE, 4), (A.GS_FRAME, 4)]
        tags += [("done", 0), ("frame", 0)]
        ranges += [(A.MPGAME, 0x1D0), (A.MPSETTINGS + A.MPS_MP_ACTIVE, 0x60),
                   (A.RNG_WORDS, 16), (A.SWITCH_FD - 1, 4), (A.FRAME_RATE, 4),
                   (A.TSLOT0 + 0x120, 0x30),
                   (A.PLAYER_SETTING + 0x14, 0xA0), (A.PLAYER_SETTING + 0x104, 0x28)]
        tags += [("mpg", 0), ("mps", 0), ("rng", 0), ("sw", 0), ("rate", 0),
                 ("pad", 0), ("act", 0), ("flg", 0)]
        for s, o in enumerate(cache["objs"]):
            if o:
                tags.append(("obj", s))
                ranges.append((o, 0x100))
        for s in range(n_humans):
            if s in cache["bl"] and cache["bl"][s]:
                tags.append(("bl", s))
                ranges.append((cache["bl"][s] + A.BL_HEALTH - 4, 0x40))
            if s in cache["cb"] and cache["cb"][s]:
                tags.append(("cb", s))
                ranges.append((cache["cb"][s] + 0x90, 0x50))
        for k in range(4):
            d = cache["drone"].get(k)
            if d:
                bv = A.BOT_VARS + k * A.BOT_VARS_STRIDE
                tags.append(("drone", k))
                ranges.append((d + A.DRONE_HEALTH - 4, 0x20))
                tags.append(("dmg", k))
                ranges.append((d + A.DRONE_LASTDMG, 4))
                tags.append(("bv", k))
                ranges.append((bv, 0xA0))
                tags.append(("bo", k))
                ranges.append((bv + A.BOT_OTHER, 0x80))
                tags.append(("bw", k))
                ranges.append((bv + A.BOT_WEAPONS, 0x55 * 0xC))
                tags.append(("br", k))
                ranges.append((bv + A.BOT_RESERVE, 0x80))
                tags.append(("bs", k))
                ranges.append((bv + A.BOT_DISTRACT, 0x4C))
        for obj, (info, pos) in cache["pinfo"].items():
            if info:
                tags.append(("pi", obj))
                ranges.append((info + A.PI_STATE, 0x14))
        # tentatively assume this frame number for the full blobs
        ranges += [(A.GS_DONE, 4), (A.GS_FRAME, 4)]
        tags += [("done1", 0), ("frame1", 0)]

        chunks = pine.read_ranges(ranges)
        bytag = {}
        for (kind, s), ch in zip(tags, chunks):
            bytag.setdefault((kind, s), ch)
        done0 = struct.unpack("<I", bytag[("done", 0)])[0]
        frame0 = struct.unpack("<I", bytag[("frame", 0)])[0]
        done1 = struct.unpack("<I", bytag[("done1", 0)])[0]
        frame1 = struct.unpack("<I", bytag[("frame1", 0)])[0]
        if frame0 != frame1 or done0 != done1 or done0 != frame0 - 1:
            time.sleep(0.005)
            continue
        if last is not None and frame0 == last:
            time.sleep(0.005)
            continue

        mpg = bytag[("mpg", 0)]
        objs = [struct.unpack_from("<I", mpg, s * A.MP_SLOT_STRIDE + A.MPG_OBJ)[0] for s in range(8)]
        pkcount = struct.unpack_from("<H", bytag[("mps", 0)], A.MPS_PICKUP_COUNT - A.MPS_MP_ACTIVE)[0]
        if objs != cache["objs"] or pkcount != cache["pkcount"]:
            # Object set changed (respawn/re-register): core-only frame, re-resolve.
            resyncs += 1
            rec = {"frame": frame0, "resync": 1, "mpg": mpg.hex(),
                   "mps": bytag[("mps", 0)].hex(), "rng": bytag[("rng", 0)].hex()}
            if first is None:
                first = frame0
            elif frame0 != last + 1:
                missed += frame0 - last - 1
            last = frame0
            last_new = time.monotonic()
            stalled_warned = False
            records.append(rec)
            out.write(json.dumps(rec) + "\n")
            resolve(pine)
            continue
        rec = {"frame": frame0}
        rec["rate"] = struct.unpack("<f", bytag[("rate", 0)])[0]
        pad = bytag[("pad", 0)]
        rec["pad"] = {"w": struct.unpack_from("<H", pad, 2)[0], "s": list(pad[8:12])}
        rec["act"] = struct.unpack("<40f", bytag[("act", 0)])
        rec["flg"] = bytag[("flg", 0)].hex()
        rec["rng"] = bytag[("rng", 0)].hex()
        sw = bytag[("sw", 0)]
        rec["sw"] = {"fd": sw[1], "fe": sw[2]}
        rec["mps"] = bytag[("mps", 0)].hex()
        rec["mpg"] = mpg.hex()

        parts = []
        for s, o in enumerate(objs):
            if not o or ("obj", s) not in bytag:
                parts.append(None)
                continue
            och = bytag[("obj", s)]
            pos = struct.unpack_from("<3f", och, A.OBJ_POS)
            entry = {
                "pos": [round(v, 4) for v in pos],
                "yaw": round(struct.unpack_from("<f", och, A.OBJ_YAW)[0], 6),
                "type": och[A.OBJ_TYPE],
                "state": struct.unpack_from("<H", och, A.OBJ_STATE)[0],
                "stamp": struct.unpack_from("<i", och, A.OBJ_STAMP)[0],
            }
            if ("bl", s) in bytag:
                bd = bytag[("bl", s)]
                entry["hp"] = round(struct.unpack_from("<f", bd, 4)[0], 3)
                entry["pitch"] = round(struct.unpack_from("<f", bd, A.BL_PITCH - (A.BL_HEALTH - 4))[0], 5)
                entry["arm"] = round(struct.unpack_from("<f", bd, A.BL_ARMOUR - (A.BL_HEALTH - 4))[0], 3)
            if ("cb", s) in bytag:
                cd = bytag[("cb", s)]
                entry["aim"] = cd[0x06]
                entry["weap"] = struct.unpack("b", cd[0x08:0x09])[0]
                entry["foot"] = round(struct.unpack("<f", cd[0x3C:0x40])[0], 4)
            if s >= 4 and ("drone", s - 4) in bytag:
                k = s - 4
                dd = bytag[("drone", k)]
                entry["bhp"] = round(struct.unpack_from("<f", dd, 4)[0], 3)
                entry["dmg"] = round(struct.unpack("<f", bytag[("dmg", k)])[0], 3)
                bvd = bytag[("bv", k)]
                entry["goal"] = bvd[:0xA0].hex()
                bwd = bytag[("bw", k)]
                clips, has = [], []
                for i in range(0x55):
                    clips.append(struct.unpack_from("<H", bwd, i * 0xC + 4)[0])
                    has.append(bwd[i * 0xC + 6])
                entry["clip"] = clips
                entry["hasw"] = has
                entry["resv"] = list(struct.unpack("<64H", bytag[("br", k)])[:0x21])
                bsd = bytag[("bs", k)]
                entry["distr"] = round(struct.unpack_from("<f", bsd, 0)[0], 3)
                entry["repend"] = struct.unpack_from("<I", bsd, 4)[0]
                entry["stat"] = {
                    "node": struct.unpack_from("<H", bsd, A.BOT_NODE - A.BOT_DISTRACT)[0],
                    "goalslot": bsd[A.BOT_GOALSLOT - A.BOT_DISTRACT],
                    "stype": bsd[A.BOT_STYPE - A.BOT_DISTRACT],
                    "curweap": bsd[A.BOT_CURWEAP - A.BOT_DISTRACT],
                    "armour": bsd[A.BOT_ARMOUR - A.BOT_DISTRACT],
                    "trait": struct.unpack("b", bsd[A.BOT_TRAIT - A.BOT_DISTRACT:A.BOT_TRAIT - A.BOT_DISTRACT + 1])[0],
                }
                bod = bytag[("bo", k)]
                oth = []
                for j in range(8):
                    q = j * 0x10
                    oth.append([
                        round(struct.unpack_from("<f", bod, q + 4)[0], 2),
                        struct.unpack_from("<I", bod, q + 12)[0],
                    ])
                entry["other"] = oth
            parts.append(entry)
        rec["pl"] = parts

        pks = []
        for obj, (info, pos) in cache["pinfo"].items():
            p = {"obj": obj, "pos": pos}
            if ("pi", obj) in bytag:
                ch = bytag[("pi", obj)]
                p["st"] = struct.unpack_from("<h", ch, 0)[0]
                p["cat"] = struct.unpack_from("<H", ch, 2)[0]
                p["item"] = struct.unpack_from("<H", ch, 4)[0]
                p["rsp"] = struct.unpack_from("<H", ch, 14)[0]
                p["idx"] = struct.unpack_from("<h", ch, 16)[0]
            pks.append(p)
        rec["pk"] = pks

        if frame0 % args.full_every == 0:
            blobs = pine.read_ranges([(a, n) for _, a, n in FULL_BLOBS])
            if pine.read32(A.GS_FRAME) == frame0 and pine.read32(A.GS_DONE) == frame0 - 1:
                rec["objx"] = {name: b.hex() for (name, _, _), b in zip(FULL_BLOBS, blobs)}
        if first is None:
            first = frame0
        elif frame0 != last + 1:
            missed += frame0 - last - 1
        last = frame0
        last_new = time.monotonic()
        stalled_warned = False
        records.append(rec)
        out.write(json.dumps(rec) + "\n")
        if len(records) % 200 == 0:
            print(f"  ... {len(records)} frames (frame {frame0})", flush=True)
        rel = frame0 - first
        while steps and steps[0][0] <= rel:
            vpad(*steps.pop(0)[1])
        if rel >= args.frames:
            break

    vpad("release")
    out.close()
    print(f"{len(records)} frames {first}..{last}, missed {missed}, resyncs {resyncs} -> {args.out}")


if __name__ == "__main__":
    main()
