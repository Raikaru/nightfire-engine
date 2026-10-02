#!/usr/bin/env python3
"""Coverage of build/xref/index.tsv over PS2 functions, as markdown tables (docs/xref.md)."""
from __future__ import annotations

import collections
import csv
import json
import os
import pathlib
import re

HOME = pathlib.Path.home()
PS2 = pathlib.Path(os.environ.get("NIGHTFIRE_PS2", HOME / "Projects/nightfire-ps2"))
XREF = PS2 / "build" / "xref"

# subsystem -> (exe, predicate on (name, tu))
SUBSYSTEMS = {
    "collision (Collide_*, Intersect_*)": ("action", lambda n, tu: n.startswith(("Collide", "Intersect"))),
    "player movement (Player_*, Plr*)": ("action", lambda n, tu: n.startswith(("Player", "Plr"))),
    "anim (Anim*)": ("action", lambda n, tu: n.startswith("Anim")),
    "drone AI (NDrone*, Drone*)": ("action", lambda n, tu: n.startswith(("NDrone", "Drone"))),
    "weapons (Bullet*, BOTWEAP*, Gun*, Weapon*, Explode*, Autoaim*)":
        ("action", lambda n, tu: n.startswith(("Bullet", "BOTWEAP", "Gun", "Weapon", "Explode", "Autoaim"))),
    "scripts (Script_*)": ("action", lambda n, tu: n.startswith("Script")),
    "bots (BOT*, AINetwork*)": ("action", lambda n, tu: n.startswith(("BOT", "AINetwork"))),
    "driving vehicle physics (PBondCar/PVehicle/PhysicsData/Simulation/Smackable TUs, Physics*/Rigid*/Rb*/WCollision*)":
        ("driving", lambda n, tu: tu.startswith(("PBondCar_", "PVehicle_", "PhysicsData_", "Simulation_", "Smackable_"))
         or n.startswith(("Physics", "Rigid", "Rb", "WCollision"))),
    "driving AI (AI* named)": ("driving", lambda n, tu: n.startswith("AI")),
    "driving weapons (SWeaponManager/Missile TUs)": ("driving", lambda n, tu: tu.startswith(("SWeaponManager_", "Missile_"))),
}


def main() -> None:
    manifest = json.loads((PS2 / "build/dump_manifest.json").read_text())
    idx = collections.defaultdict(dict)
    with open(XREF / "index.tsv") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            idx[(r["ps2_exe"], r["ps2_symbol"])][r["platform"]] = r

    def row(label, items):
        n = len(items)
        g = sum(1 for k in items if "gc" in idx.get(k, {}))
        x = sum(1 for k in items if "xbox" in idx.get(k, {}))
        e = sum(1 for k in items if idx.get(k))
        gh = sum(1 for k in items if idx.get(k, {}).get("gc", {}).get("confidence") == "high")
        xh = sum(1 for k in items if idx.get(k, {}).get("xbox", {}).get("confidence") == "high")
        pct = lambda v: f"{100 * v / n:.1f}%" if n else "-"
        return f"| {label} | {n} | {g} ({pct(g)}) | {gh} | {x} ({pct(x)}) | {xh} | {e} ({pct(e)}) |"

    hdr = "| set | PS2 funcs | GC | GC high | Xbox | Xbox high | GC or Xbox |\n|---|---:|---:|---:|---:|---:|---:|"
    print("### Overall\n")
    print(hdr)
    for t in ("action", "driving"):
        items = [(t, n) for n, *_ in manifest[t]]
        print(row(f"{t.upper()}.ELF all", items))
        named = [(t, n) for n, *_ in manifest[t] if not re.match(r"func_[0-9A-F]{8}$", n)]
        print(row(f"{t.upper()}.ELF named (no func_)", named))
        big = [(t, n) for n, va, s, tu in manifest[t] if s >= 64]
        print(row(f"{t.upper()}.ELF size>=64B", big))
    print("\n### Subsystems\n")
    print(hdr)
    for label, (t, pred) in SUBSYSTEMS.items():
        items = [(t, n) for n, va, s, tu in manifest[t] if pred(n, tu)]
        print(row(f"{t}: {label}", items))
    print("\n### Methods (pairs by primary evidence family)\n")

    def family(m: str) -> str:
        for f in ("via-gc", "strset", "str", "bsim", "call", "data", "const", "order"):
            if m.startswith(f):
                return f
        return m

    meth = collections.Counter()
    for (t, _n), plats in idx.items():
        for p, r in plats.items():
            meth[(t, p, family(r["method"]), r["confidence"])] += 1
    fams = ("str", "strset", "bsim", "call", "data", "const", "order", "via-gc")
    print("| exe/platform | " + " | ".join(f"{f} (high/med)" for f in fams) + " |")
    print("|---|" + "---:|" * len(fams))
    for t in ("action", "driving"):
        for p in ("gc", "xbox"):
            cells = [f"{meth[(t, p, f, 'high')]}/{meth[(t, p, f, 'medium')]}" for f in fams]
            print(f"| {t}/{p} | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()
