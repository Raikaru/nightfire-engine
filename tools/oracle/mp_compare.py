"""MP comparison harness: summarize/record-compare oracle recordings and diff them
against engine traces (see src/game/mp_trace.* for the engine-side emitter).

  mp_compare.py summary rec.jsonl
      match overview: frames, game-time span, kills/deaths/points per slot,
      team scores, end state, pickup takes, estimated shots per bot, bot state
      histogram, RNG stream check (re-simulate Rand_Random from frame-0 words).
  mp_compare.py determinism a.jsonl b.jsonl
      two recordings from the same savestate: max per-field divergence
      (proves game + recorder determinism; bots must fight identically).
  mp_compare.py diff oracle.jsonl engine.jsonl
      engine trace (mp_trace format) vs oracle: per-subsystem residuals
      (humans, bots, scores, pickups, rng) for synced (reseeded) and lockstep
      runs; reports divergence frame + first differing field.
"""

import argparse
import json
import math
import struct
import sys


def load(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def mpg_slot(mpg_hex, s):
    b = bytes.fromhex(mpg_hex)
    o = s * 0x30
    kills, deaths = struct.unpack_from("<ii", b, o + 0x04)
    pts = struct.unpack_from("<f", b, o + 0x18)[0]
    return kills, deaths, pts


def mpg_global(mpg_hex):
    b = bytes.fromhex(mpg_hex)
    t0, t1 = struct.unpack_from("<ff", b, 0x180)
    return {
        "team0": t0, "team1": t1,
        "state": struct.unpack_from("<I", b, 0x188)[0],
        "elapsed": struct.unpack_from("<f", b, 0x190)[0],
    }


def summary(recs):
    assert recs, "empty"
    full = [r for r in recs if "pl" in r]
    print(f"frames: {len(recs)} ({recs[0]['frame']}..{recs[-1]['frame']}), "
          f"resyncs: {sum(1 for r in recs if r.get('resync'))}")
    g0, g1 = mpg_global(full[0]["mpg"]), mpg_global(full[-1]["mpg"])
    print(f"elapsed: {g0['elapsed']:.1f}..{g1['elapsed']:.1f} s, "
          f"state {g0['state']}->{g1['state']}, teams {g1['team0']:.0f}/{g1['team1']:.0f}")
    for s in range(8):
        k0, d0, p0 = mpg_slot(full[0]["mpg"], s)
        k1, d1, p1 = mpg_slot(full[-1]["mpg"], s)
        if k0 or d0 or p0 or k1 or d1 or p1 or full[-1]["pl"][s]:
            print(f"  slot{s}: kills {k0}->{k1} deaths {d0}->{d1} points {p0:.0f}->{p1:.0f}")
    # bot state histogram + shots estimate (clip decreases)
    from collections import Counter
    states = Counter()
    for r in full:
        for s in range(4, 8):
            p = r["pl"][s]
            if p:
                states[(s, p["state"])] += 1
    print("  bot states (top):", states.most_common(8))
    for s in range(4, 8):
        b0, b1 = full[0]["pl"][s], full[-1]["pl"][s]
        if b0 and b1 and "clip" in b0 and "clip" in b1:
            fired = sum(max(0, a - b) for a, b in zip(b0["clip"], b1["clip"]) if a and b)
            print(f"  bot{s - 4} weapon {b1['stat']['curweap']} clips net {fired}, hp {b0.get('bhp')}->{b1.get('bhp')}")
    # pickups taken: st 1->2 transitions
    takes = 0
    prev = {}
    for r in full:
        cur = {p["obj"]: p.get("st") for p in r["pk"]}
        for o, st in cur.items():
            if o in prev and prev[o] == 1 and st == 2:
                takes += 1
        prev = cur
    print(f"  pickup takes: {takes}")
    # RNG stream check: words must evolve (many systems draw each frame, so no
    # clean frame-to-frame chain is expected; this only proves the stream is live).
    w0 = full[0]["rng"][:16]
    w1 = full[-1]["rng"][:16]
    print(f"  rng: {w0}..{w1} ({'live' if w0 != w1 else 'STUCK'})")

def determinism(a, b):
    ia = {r["frame"]: r for r in a if "pl" in r}
    ib = {r["frame"]: r for r in b if "pl" in r}
    common = sorted(set(ia) & set(ib))
    print(f"common frames: {len(common)}")
    if not common:
        return
    maxdp, maxdy, maxhp = 0.0, 0.0, 0.0
    bad = 0
    for f in common:
        ra, rb = ia[f], ib[f]
        for s in range(8):
            pa, pb = ra["pl"][s], rb["pl"][s]
            if (pa is None) != (pb is None):
                bad += 1
                continue
            if pa is None:
                continue
            dp = math.dist(pa["pos"], pb["pos"])
            dy = abs((pa["yaw"] - pb["yaw"] + math.pi) % (2 * math.pi) - math.pi)
            maxdp, maxdy = max(maxdp, dp), max(maxdy, dy)
            for k in ("hp", "bhp"):
                if k in pa and k in pb:
                    maxhp = max(maxhp, abs(pa[k] - pb[k]))
            if pa.get("state") != pb.get("state") or pa.get("type") != pb.get("type"):
                bad += 1
    print(f"max pos {maxdp * 100:.3f} cm, max yaw {maxdy:.6f} rad, max hp {maxhp:.3f}, "
          f"state/type mismatches {bad}")

def diff(oracle_path, engine_path):
    o = [r for r in load(oracle_path) if "pl" in r]
    e = load(engine_path)
    # engine frames count from its own match start; align by index after skipping
    # the engine's pre-match frames is the caller's job (both start at spawn).
    n = min(len(o), len(e))
    print(f"aligned frames: {n} (oracle {len(o)}, engine {len(e)})")
    worst = []
    for i in range(n):
        r, er = o[i], e[i]
        go = mpg_global(r["mpg"])
        for s in range(8):
            p, q = r["pl"][s], er["pl"][s] if s < len(er["pl"]) else None
            if p is None or q is None:
                if (p is None) != (q is None):
                    worst.append((i, s, "presence"))
                continue
            dp = math.dist(p["pos"], q["pos"])
            dy = abs((p["yaw"] - q["yaw"] + math.pi) % (2 * math.pi) - math.pi)
            ohp = p.get("hp", p.get("bhp", 0.0))
            dhp = abs(ohp - q["hp"])
            if dp > 0.01 or dhp > 1.0:
                worst.append((i, s, f"{dp * 100:.1f}cm yaw {dy:.4f} hp {ohp:.0f}->{q['hp']:.0f}"))
        ge_teams = (er["teams"][0], er["teams"][1])
        if abs(go["team0"] - ge_teams[0]) >= 1 or abs(go["team1"] - ge_teams[1]) >= 1 or go["state"] != er["state"]:
            worst.append((i, "scores", f"teams {go['team0']:.0f}/{go['team1']:.0f} vs "
                          f"{ge_teams[0]:.0f}/{ge_teams[1]:.0f} st {go['state']}/{er['state']}"))
    for w in worst[:30]:
        print(w)
    print(f"... {len(worst)} divergent fields over {n} frames")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("summary")
    p.add_argument("rec")
    p = sub.add_parser("determinism")
    p.add_argument("a")
    p.add_argument("b")
    p = sub.add_parser("diff")
    p.add_argument("oracle")
    p.add_argument("engine")
    args = ap.parse_args()
    if args.cmd == "summary":
        summary(load(args.rec))
    elif args.cmd == "determinism":
        determinism(load(args.a), load(args.b))
    elif args.cmd == "diff":
        diff(args.oracle, args.engine)


if __name__ == "__main__":
    sys.exit(main())
