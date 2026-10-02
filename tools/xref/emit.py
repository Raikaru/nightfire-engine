#!/usr/bin/env python3
"""Turn match tables into build/xref/index.tsv and per-symbol C files.

Inputs:  $XREF/matches/ps2_<t>__{gc,xbox}_<t>.tsv  (match.py)
         $XREF/decomp/<prog>.jsonl                 (ghidra_export.py)
Outputs: $XREF/index.tsv
         $XREF/{gc,xbox}/{action,driving}/<ps2_symbol>.c
"""
from __future__ import annotations

import csv
import json
import os
import pathlib
import re
import shutil

HOME = pathlib.Path.home()
PS2 = pathlib.Path(os.environ.get("NIGHTFIRE_PS2", HOME / "Projects/nightfire-ps2"))
XREF = PS2 / "build" / "xref"


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
    fun_re = re.compile(r"\bFUN_([0-9a-fA-F]{8})\b")
    for t in ("action", "driving"):
        names = {va: name for name, va, _s, _tu in manifest[t]}
        for plat in ("gc", "xbox"):
            pairs = read_pairs(f"ps2_{t}__{plat}_{t}")
            dec = load_decomp(f"{plat}_{t}")
            rename = {b: names[a] for a, (b, _m, _c) in pairs.items() if a in names}
            outdir = XREF / plat / t
            if outdir.exists():
                shutil.rmtree(outdir)
            outdir.mkdir(parents=True)

            def sub(m):
                return rename.get(int(m.group(1), 16), m.group(0))

            for a, (b, meth, conf) in sorted(pairs.items()):
                if a not in names:
                    continue
                name = names[a]
                rows.append((t, name, a, plat, b, conf, meth))
                text = dec.get(b, "")
                body = fun_re.sub(sub, text) if text else "/* Ghidra decompiler failed or timed out on this function */\n"
                hdr = (f"/* {plat.upper()} counterpart of PS2 {t.upper()}.ELF {name} @ 0x{a:08x}\n"
                       f" * {plat} address 0x{b:08x}  confidence={conf}  method={meth}\n"
                       f" * Calls to matched functions are renamed to their PS2 symbols; FUN_xxxxxxxx = unmatched.\n"
                       f" * Struct offsets and vector sizes differ between platforms (see docs/xref.md).\n */\n")
                (outdir / f"{name}.c").write_text(hdr + body)

    rows.sort(key=lambda r: (r[0], r[2], r[3]))
    with open(XREF / "index.tsv", "w") as f:
        f.write("ps2_exe\tps2_symbol\tps2_addr\tplatform\taddr\tconfidence\tmethod\n")
        for t, name, a, plat, b, conf, meth in rows:
            f.write(f"{t}\t{name}\t0x{a:08x}\t{plat}\t0x{b:08x}\t{conf}\t{meth}\n")
    print(f"index rows: {len(rows)}")


if __name__ == "__main__":
    main()
