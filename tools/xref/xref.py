#!/usr/bin/env python3
"""Show PS2 pseudocode next to the matched GameCube / Xbox decompilation.

    python3 tools/xref/xref.py Player_SSWalk
    python3 tools/xref/xref.py Intersect_CylGeom --stack
    python3 tools/xref/xref.py Collide_ --list          # list matching symbols + coverage

Query resolution: exact PS2 symbol, then exact base name (mangling stripped:
`Player_SSWalk__FP6BLDataP7obj_tag` -> `Player_SSWalk`), then case-insensitive
substring.  More than --max hits prints the candidate list instead.

Data comes from nightfire-ps2 (override with $NIGHTFIRE_PS2):
    build/ida/<exe>/<sym>.c, build/ghidra/<exe>/<sym>.c   PS2 pseudocode
    build/xref/{gc,xbox}/<exe>/<sym>.c                    counterparts
    build/xref/index.tsv                                  pair metadata
See docs/xref.md.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import pathlib
import shutil
import sys

ENGINE = pathlib.Path(__file__).resolve().parents[2]
PS2 = pathlib.Path(os.environ.get("NIGHTFIRE_PS2", ENGINE.parent / "nightfire-ps2"))
XREF = PS2 / "build" / "xref"
EXES = ("action", "driving")


def base_name(sym: str) -> str:
    s = sym.split(".NON_MATCHING")[0]
    i = s.find("__", 1)
    return s[:i] if i > 0 else s


def load_symbols() -> list[tuple[str, str, int]]:
    manifest = json.loads((PS2 / "build" / "dump_manifest.json").read_text())
    return [(exe, name, va) for exe in EXES for name, va, _size, _tu in manifest[exe]]


def load_index() -> dict[tuple[str, str], dict[str, dict]]:
    idx: dict[tuple[str, str], dict[str, dict]] = {}
    p = XREF / "index.tsv"
    if not p.exists():
        sys.exit(f"missing {p}; run tools/xref pipeline (docs/xref.md)")
    with open(p) as f:
        for r in csv.DictReader(f, delimiter="\t"):
            idx.setdefault((r["ps2_exe"], r["ps2_symbol"]), {})[r["platform"]] = r
    return idx


def resolve(query: str, syms, exe_filter: str | None):
    syms = [s for s in syms if not exe_filter or s[0] == exe_filter]
    exact = [s for s in syms if s[1] == query]
    if exact:
        return exact
    base = [s for s in syms if base_name(s[1]) == query]
    if base:
        return base
    q = query.lower()
    return [s for s in syms if q in s[1].lower()]


def read(p: pathlib.Path) -> str | None:
    return p.read_text(errors="replace") if p.exists() else None


def sources(exe: str, sym: str, meta: dict, which_ps2: str) -> list[tuple[str, str]]:
    out = []
    ida = read(PS2 / "build" / "ida" / exe / f"{sym}.c")
    gh = read(PS2 / "build" / "ghidra" / exe / f"{sym}.c")
    if which_ps2 in ("ida", "both") or (which_ps2 == "auto" and ida):
        out.append(("PS2 IDA", ida or "/* no IDA dump */"))
    if which_ps2 in ("ghidra", "both") or (which_ps2 == "auto" and not ida):
        out.append(("PS2 Ghidra", gh or "/* no Ghidra dump */"))
    for plat, label in (("gc", "GameCube"), ("xbox", "Xbox")):
        r = meta.get(plat)
        if r:
            text = read(XREF / plat / exe / f"{sym}.c") or "/* file missing */"
            out.append((f"{label} {r['addr']} [{r['confidence']}, {r['method']}]", text))
        else:
            out.append((label, f"/* no {label} counterpart matched */"))
    return out


def render_columns(cols: list[tuple[str, str]], width: int) -> str:
    n = len(cols)
    cw = max(30, (width - 3 * (n - 1)) // n)
    split = [[t] + ["-" * cw] + body.expandtabs(4).splitlines() for t, body in cols]
    h = max(len(s) for s in split)
    lines = []
    for i in range(h):
        cells = []
        for s in split:
            c = s[i] if i < len(s) else ""
            c = c if len(c) <= cw else c[:cw - 1] + "›"
            cells.append(c.ljust(cw))
        lines.append(" | ".join(cells).rstrip())
    return "\n".join(lines)


def render_stack(cols: list[tuple[str, str]]) -> str:
    return "\n".join(f"===== {t} =====\n{body.rstrip()}\n" for t, body in cols)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("query")
    ap.add_argument("--exe", choices=EXES, help="restrict to ACTION or DRIVING")
    ap.add_argument("--stack", action="store_true", help="print sources one after another (full width)")
    ap.add_argument("--ps2", choices=("auto", "ida", "ghidra", "both"), default="auto",
                    help="PS2 pseudocode source (auto = IDA, Ghidra fallback)")
    ap.add_argument("--width", type=int, default=0, help="column layout width (default: terminal, min 200)")
    ap.add_argument("--max", type=int, default=4, help="max symbols to print before listing instead")
    ap.add_argument("--list", action="store_true", help="only list matching symbols with GC/Xbox coverage")
    args = ap.parse_args()

    syms = load_symbols()
    idx = load_index()
    hits = resolve(args.query, syms, args.exe)
    if not hits:
        sys.exit(f"no PS2 symbol matches {args.query!r}")
    if args.list or len(hits) > args.max:
        gcn = sum(1 for e, s, _ in hits if "gc" in idx.get((e, s), {}))
        xbn = sum(1 for e, s, _ in hits if "xbox" in idx.get((e, s), {}))
        print(f"{len(hits)} symbols  (GC {gcn}, Xbox {xbn})")
        for exe, sym, va in hits:
            m = idx.get((exe, sym), {})
            tags = " ".join(f"{p}={m[p]['addr']}({m[p]['confidence'][0]})" for p in ("gc", "xbox") if p in m)
            print(f"  {exe:7} 0x{va:08x} {sym}  {tags}")
        if not args.list:
            print(f"(more than --max={args.max}; refine the query or pass --exe/--max)")
        return
    width = args.width or max(200, shutil.get_terminal_size((240, 50)).columns)
    for exe, sym, va in hits:
        meta = idx.get((exe, sym), {})
        cols = sources(exe, sym, meta, args.ps2)
        print(f"##### {exe.upper()}.ELF {sym} @ 0x{va:08x}")
        print(render_stack(cols) if args.stack else render_columns(cols, width))
        print()


if __name__ == "__main__":
    main()
