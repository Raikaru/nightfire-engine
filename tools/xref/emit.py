#!/usr/bin/env python3
"""Combine match tables into build/xref/index.tsv and per-symbol C files.

Inputs:  $XREF/matches/ps2_<t>__gc_<t>.tsv, ps2_<t>__xbox_<t>.tsv, gc_<t>__xbox_<t>.tsv
         $XREF/decomp/<prog>.jsonl
Outputs: $XREF/index.tsv
         $XREF/{gc,xbox}/{action,driving}/<ps2_symbol>.c
         $XREF/coverage.json   (stats consumed by docs/xref.md)

Xbox pairs: the direct PS2<->Xbox match wins; a GC-composed pair (PS2<->GC<->Xbox,
method "via-gc") fills gaps.  When direct and composed disagree, the pair with the
higher confidence is kept; on a tie both are dropped (counted in coverage.json).
"""
from __future__ import annotations

import collections
import csv
import json
import os
import pathlib
import re
import shutil

HOME = pathlib.Path.home()
PS2 = pathlib.Path(os.environ.get("NIGHTFIRE_PS2", HOME / "Projects/nightfire-ps2"))
XREF = PS2 / "build" / "xref"
RANK = {"high": 2, "medium": 1}
PLATFORM_PROG = {"gc": "gc_{t}", "xbox": "xbox_{t}"}


def read_pairs(name: str) -> dict[int, tuple[int, str, str]]:
    p = XREF / "matches" / f"{name}.tsv"
    out = {}
    if not p.exists():
        return out
    with open(p) as f:
        for r in csv.DictReader(f, delimiter="\t"):
            out[int(r["a_addr"], 16)] = (int(r["b_addr"], 16), r["method"], r["confidence"])
    return out


def load_decomp(prog: str) -> dict[int, str]:
    p = XREF / "decomp" / f"{prog}.jsonl"
    out = {}
    if p.exists():
        for line in p.read_text().splitlines():
            d = json.loads(line)
            out[d["addr"]] = d["c"]
    return out


def main() -> None:
    manifest = json.loads((PS2 / "build/dump_manifest.json").read_text())
    rows = []
    cov = {"targets": {}, "xbox_direct_vs_gc": {}}
    for t in ("action", "driving"):
        names = {va: name for name, va, _s, _tu in manifest[t]}
        gc = read_pairs(f"ps2_{t}__gc_{t}")
        xb = read_pairs(f"ps2_{t}__xbox_{t}")
        gx = read_pairs(f"gc_{t}__xbox_{t}")
        # compose PS2 -> GC -> Xbox
        agree = disagree = filled = dropped = 0
        final_xb = dict(xb)
        taken = {b for b, _m, _c in xb.values()}
        for a, (g, gm, gconf) in gc.items():
            if g not in gx:
                continue
            x, xm, xconf = gx[g]
            conf = min(gconf, xconf, key=RANK.get)
            if a in xb:
                if xb[a][0] == x:
                    agree += 1
                else:
                    disagree += 1
                    if RANK[conf] > RANK[xb[a][2]]:
                        final_xb[a] = (x, "via-gc", conf)
                    elif RANK[conf] == RANK[xb[a][2]]:
                        del final_xb[a]
                        dropped += 1
                continue
            if x in taken:
                continue
            final_xb[a] = (x, f"via-gc({gm}+{xm})", conf)
            taken.add(x)
            filled += 1
        cov["xbox_direct_vs_gc"][t] = dict(agree=agree, disagree=disagree, dropped=dropped, filled=filled)

        for plat, pairs in (("gc", gc), ("xbox", final_xb)):
            prog = PLATFORM_PROG[plat].format(t=t)
            dec = load_decomp(prog)
            rename = {b: names[a] for a, (b, _m, _c) in pairs.items() if a in names}
            outdir = XREF / plat / t
            if outdir.exists():
                shutil.rmtree(outdir)
            outdir.mkdir(parents=True)
            fun_re = re.compile(r"\bFUN_([0-9a-fA-F]{8})\b")

            def sub(m):
                v = int(m.group(1), 16)
                return rename.get(v, m.group(0))

            for a, (b, meth, conf) in sorted(pairs.items()):
                if a not in names:
                    continue
                name = names[a]
                rows.append((t, name, a, plat, b, conf, meth))
                text = dec.get(b, "")
                body = fun_re.sub(sub, text) if text else "/* no Ghidra decompilation available */\n"
                hdr = (f"/* {plat.upper()} counterpart of PS2 {t.upper()}.ELF {name} @ 0x{a:08x}\n"
                       f" * {plat} address 0x{b:08x}  confidence={conf}  method={meth}\n"
                       f" * Calls to matched functions are renamed to their PS2 symbols; FUN_xxxxxxxx = unmatched.\n */\n")
                (outdir / f"{name}.c").write_text(hdr + body)
        cov["targets"][t] = dict(ps2_funcs=len(names))

    rows.sort(key=lambda r: (r[0], r[2], r[3]))
    with open(XREF / "index.tsv", "w") as f:
        f.write("ps2_exe\tps2_symbol\tps2_addr\tplatform\taddr\tconfidence\tmethod\n")
        for t, name, a, plat, b, conf, meth in rows:
            f.write(f"{t}\t{name}\t0x{a:08x}\t{plat}\t0x{b:08x}\t{conf}\t{meth}\n")
    (XREF / "coverage.json").write_text(json.dumps(cov, indent=1))
    print(f"index rows: {len(rows)}")
    print(json.dumps(cov["xbox_direct_vs_gc"]))


if __name__ == "__main__":
    main()
