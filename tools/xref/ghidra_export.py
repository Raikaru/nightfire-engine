#!/usr/bin/env python3
"""Import + analyse one Nightfire binary in headless Ghidra and export match features.

One Ghidra project per program (so several programs can run in parallel):
    $XREF/ghidra/<prog>/<prog>.gpr

Outputs (all under $XREF = nightfire-ps2/build/xref):
    feat/<prog>.json      per-function features (calls, strings, constants, size)
    decomp/<prog>.jsonl   {"addr": int, "c": str} Ghidra C for every function
                          (GC/Xbox only; PS2 already has build/{ida,ghidra} dumps)

Usage:  ghidra_export.py <prog> [--no-decomp] [--threads N]
        prog in: gc_action gc_driving xbox_action xbox_driving ps2_action ps2_driving
"""
from __future__ import annotations

import argparse
import json
import math
import os
import pathlib
import struct
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

HOME = pathlib.Path.home()
PS2 = pathlib.Path(os.environ.get("NIGHTFIRE_PS2", HOME / "Projects/nightfire-ps2"))
XREF = PS2 / "build" / "xref"
os.environ.setdefault("GHIDRA_INSTALL_DIR", str(HOME / "ghidra_12.1.3_PUBLIC"))

PROGS = {
    # name: (binary, language, compiler, r13 for PPC SDA base)
    "gc_action": (PS2 / "gc/modules/Nightfire.elf", "PowerPC:BE:32:Gekko_Broadway", "default", 0x80316000),
    "gc_driving": (XREF / "bin/gc_driving.elf", "PowerPC:BE:32:Gekko_Broadway", "default", 0x8037FBC0),
    "xbox_action": (XREF / "bin/xbox_default.exe", "x86:LE:32:default", "windows", None),
    "xbox_driving": (XREF / "bin/xbox_driving.exe", "x86:LE:32:default", "windows", None),
    "ps2_action": (PS2 / "original/ACTION.ELF", "r5900:LE:32:default", "default", None),
    "ps2_driving": (PS2 / "original/DRIVING.ELF", "r5900:LE:32:default", "default", None),
}


def nice_float(bits: int) -> bool:
    """A 32-bit pattern that plausibly is a float literal rather than an int/pointer."""
    if bits in (0, 0x80000000):
        return False
    exp = (bits >> 23) & 0xFF
    if exp == 0 or exp == 0xFF:
        return False
    f = struct.unpack("<f", struct.pack("<I", bits))[0]
    return 1e-6 <= abs(f) <= 1e7


def prepare_gc_driving() -> None:
    """GC driving.elf has e_machine 0; give Ghidra a copy with EM_PPC."""
    dst = XREF / "bin/gc_driving.elf"
    if dst.exists():
        return
    data = bytearray((PS2 / "gc/modules/driving.elf").read_bytes())
    struct.pack_into(">H", data, 0x12, 20)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(bytes(data))


def load_manifest(target: str):
    return json.loads((PS2 / "build/dump_manifest.json").read_text())[target]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("prog", choices=sorted(PROGS))
    ap.add_argument("--no-decomp", action="store_true")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--heap", default="3g")
    args = ap.parse_args()
    prog = args.prog
    binary, lang, comp, r13 = PROGS[prog]
    if prog == "gc_driving":
        prepare_gc_driving()

    from pyghidra.launcher import HeadlessPyGhidraLauncher
    launcher = HeadlessPyGhidraLauncher()
    launcher.add_vmargs(f"-Xmx{args.heap}")
    launcher.start()
    import pyghidra

    projdir = XREF / "ghidra" / prog
    projdir.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    open_args = dict(project_location=str(projdir), project_name=prog, language=lang, compiler=comp,
                     program_name=prog)
    # Session 1: import + setup + analysis; leaving the context saves the program.
    with pyghidra.open_program(str(binary), analyze=False, **open_args) as flat:
        program = flat.getCurrentProgram()
        from ghidra.program.util import GhidraProgramUtilities
        if not GhidraProgramUtilities.isAnalyzed(program):
            setup(program, prog, r13)
            print(f"[{prog}] analysing...", flush=True)
            flat.analyzeAll(program)
            GhidraProgramUtilities.markProgramAnalyzed(program)
            if prog.startswith("ps2_"):
                fix_ps2_functions(program, prog)
            print(f"[{prog}] analysis done in {time.time() - t0:.0f}s", flush=True)
        if not prog.startswith("ps2_"):
            opts = program.getOptions("Program Information")
            if not opts.getBoolean("xref.gapfill", False):
                n = fill_gaps(flat, program, prog)
                with tx(program, "gapfill flag"):
                    opts.setBoolean("xref.gapfill", True)
                print(f"[{prog}] gap fill created {n} functions", flush=True)
    # Session 2: export (re-runnable without re-analysis).
    with pyghidra.open_program(str(binary), analyze=False, **open_args) as flat:
        program = flat.getCurrentProgram()
        feats = export_features(program, prog)
        out = XREF / "feat" / f"{prog}.json"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(feats))
        print(f"[{prog}] features: {len(feats['funcs'])} funcs -> {out}", flush=True)
        if not args.no_decomp and not prog.startswith("ps2_"):
            decompile_all(program, prog, args.threads)
    print(f"[{prog}] total {time.time() - t0:.0f}s", flush=True)


def tx(program, label):
    class _T:
        def __enter__(self):
            self.id = program.startTransaction(label)

        def __exit__(self, et, ev, tb):
            program.endTransaction(self.id, et is None)
    return _T()


def fill_gaps(flat, program, prog: str) -> int:
    """Create functions auto-analysis missed (code only reached through vtables /
    callback tables): code pointers found in data, then non-padding bytes that
    follow a function's end.  Repeats until no new function appears."""
    import jpype
    from ghidra.app.cmd.disassemble import DisassembleCommand
    from ghidra.app.cmd.function import CreateFunctionCmd
    from ghidra.util.task import ConsoleTaskMonitor
    mon = ConsoleTaskMonitor()
    mem = program.getMemory()
    fm = program.getFunctionManager()
    listing = program.getListing()
    space = program.getAddressFactory().getDefaultAddressSpace()
    big = mem.isBigEndian()
    fmt = ">I" if big else "<I"
    pad = {0xCC, 0x90} if not big else {0x00}
    code = [(b.getStart().getOffset(), b.getEnd().getOffset()) for b in mem.getBlocks()
            if b.isExecute() and b.isInitialized()]

    def in_code(v):
        return any(lo <= v <= hi for lo, hi in code)

    def block_bytes(b):
        arr = jpype.JArray(jpype.JByte)(int(b.getSize()))
        b.getBytes(b.getStart(), arr)
        return bytes(x & 0xFF for x in arr)

    data_blobs = [(b.getStart().getOffset(), block_bytes(b)) for b in mem.getBlocks()
                  if not b.isExecute() and b.isInitialized() and b.getSize() < 0x1000000]
    code_blobs = {b.getStart().getOffset(): block_bytes(b) for b in mem.getBlocks()
                  if b.isExecute() and b.isInitialized()}
    total = 0
    for _round in range(4):
        cand = set()
        for base, blob in data_blobs:
            for off in range(0, len(blob) - 3, 4):
                v = struct.unpack_from(fmt, blob, off)[0]
                if v & (3 if big else 0) or not in_code(v):
                    continue
                a = space.getAddress(v)
                if fm.getFunctionContaining(a) is None and listing.getInstructionContaining(a) is None:
                    cand.add(v)
        funcs = sorted((f.getEntryPoint().getOffset(), f.getBody().getMaxAddress().getOffset())
                       for f in fm.getFunctions(True))
        for (s0, e0), (s1, _e1) in zip(funcs, funcs[1:]):
            p = e0 + 1
            blob_base = next((lo for lo, hi in code if lo <= p <= hi), None)
            if blob_base is None:
                continue
            blob = code_blobs[blob_base]
            while p < s1 and blob[p - blob_base] in pad:
                p += 1
            if big:
                p = (p + 3) & ~3
            if s1 - p >= 8 and listing.getInstructionContaining(space.getAddress(p)) is None:
                cand.add(p)
        made = 0
        with tx(program, "gapfill"):
            for v in sorted(cand):
                a = space.getAddress(v)
                if fm.getFunctionContaining(a) is not None:
                    continue
                DisassembleCommand(a, None, True).applyTo(program, mon)
                if listing.getInstructionAt(a) is None:
                    continue
                if CreateFunctionCmd(a).applyTo(program, mon):
                    made += 1
        flat.analyzeChanges(program)
        total += made
        print(f"[{prog}] gapfill round {_round}: {len(cand)} candidates, {made} functions", flush=True)
        if made == 0:
            break
    return total


def setup(program, prog: str, r13) -> None:
    from java.math import BigInteger
    from ghidra.program.model.symbol import SourceType
    from ghidra.program.model.address import AddressSet
    space = program.getAddressFactory().getDefaultAddressSpace()
    to = space.getAddress
    with tx(program, "xref setup"):
        if r13 is not None:
            ctx = program.getProgramContext()
            reg = program.getRegister("r13")
            mem = program.getMemory()
            for blk in mem.getBlocks():
                if blk.isExecute():
                    ctx.setValue(reg, blk.getStart(), blk.getEnd(), BigInteger.valueOf(r13))
        if prog.startswith("xbox_"):
            info = json.loads((XREF / "bin" / (pathlib.Path(PROGS[prog][0]).name + ".json")).read_text())
            mem = program.getMemory()
            a = info["kernel_thunk"]
            st = program.getSymbolTable()
            while True:
                v = mem.getInt(to(a)) & 0xFFFFFFFF
                if v == 0:
                    break
                st.createLabel(to(a), f"xboxkrnl_{v & 0x7FFFFFFF}", SourceType.IMPORTED)
                a += 4
        if prog.startswith("ps2_"):
            target = prog[4:]
            fm = program.getFunctionManager()
            st = program.getSymbolTable()
            for name, va, size, _tu in load_manifest(target):
                try:
                    st.createLabel(to(va), name, SourceType.USER_DEFINED)
                except Exception:
                    pass
            import re
            cfg = PS2 / f"config/{target}.us/symbol_addrs.txt"
            for line in cfg.read_text(errors="ignore").splitlines():
                m = re.match(r"\s*([A-Za-z_$][\w$.]*)\s*=\s*0x([0-9A-Fa-f]+);", line)
                if m:
                    try:
                        st.createLabel(to(int(m.group(2), 16)), m.group(1), SourceType.USER_DEFINED)
                    except Exception:
                        pass
            from ghidra.app.cmd.disassemble import DisassembleCommand
            from ghidra.util.task import ConsoleTaskMonitor
            mon = ConsoleTaskMonitor()
            for name, va, size, _tu in load_manifest(target):
                DisassembleCommand(to(va), None, True).applyTo(program, mon)
                try:
                    fm.createFunction(name, to(va), AddressSet(to(va), to(va + size - 1)), SourceType.USER_DEFINED)
                except Exception:
                    pass


def fix_ps2_functions(program, prog: str) -> None:
    """Re-impose manifest boundaries after auto-analysis (it may merge/split)."""
    from ghidra.program.model.address import AddressSet
    from ghidra.program.model.symbol import SourceType
    space = program.getAddressFactory().getDefaultAddressSpace()
    to = space.getAddress
    fm = program.getFunctionManager()
    with tx(program, "ps2 boundaries"):
        for name, va, size, _tu in load_manifest(prog[4:]):
            body = AddressSet(to(va), to(va + size - 1))
            f = fm.getFunctionAt(to(va))
            try:
                for g in list(fm.getFunctionsOverlapping(body)):
                    if not g.getEntryPoint().equals(to(va)):
                        fm.removeFunction(g.getEntryPoint())
                if f is None:
                    fm.createFunction(name, to(va), body, SourceType.USER_DEFINED)
                else:
                    f.setBody(body)
                    f.setName(name, SourceType.USER_DEFINED)
            except Exception:
                pass


def read_cstring(mem, addr, maxlen=300):
    from ghidra.program.model.mem import MemoryAccessException
    out = bytearray()
    try:
        for i in range(maxlen):
            b = mem.getByte(addr.add(i)) & 0xFF
            if b == 0:
                break
            out.append(b)
        else:
            return None
    except (MemoryAccessException, Exception):
        return None
    if len(out) < 4:
        return None
    if not all(32 <= c < 127 or c in (9, 10, 13) for c in out):
        return None
    return out.decode("latin1")


def export_features(program, prog: str) -> dict:
    from ghidra.program.model.address import AddressSet
    listing = program.getListing()
    mem = program.getMemory()
    fm = program.getFunctionManager()
    space = program.getAddressFactory().getDefaultAddressSpace()
    to = space.getAddress
    is_ps2 = prog.startswith("ps2_")
    is_ppc = prog.startswith("gc_")

    exec_set = AddressSet()
    for blk in mem.getBlocks():
        if blk.isExecute() and blk.isInitialized():
            exec_set.add(blk.getStart(), blk.getEnd())

    def in_mem(v: int) -> bool:
        try:
            return mem.contains(to(v))
        except Exception:
            return False

    if is_ps2:
        rows = [(name, va, size) for name, va, size, _tu in load_manifest(prog[4:])]
    else:
        rows = []
        for f in fm.getFunctions(True):
            if f.isThunk() or f.isExternal():
                continue
            ep = f.getEntryPoint()
            if not exec_set.contains(ep):
                continue
            rows.append((f.getName(), ep.getOffset(), int(f.getBody().getNumAddresses())))
    entries = {va for _n, va, _s in rows}

    funcs = []
    for name, va, size in rows:
        if is_ps2:
            body = AddressSet(to(va), to(va + size - 1))
        else:
            body = fm.getFunctionAt(to(va)).getBody()
        calls, strings, floats, ints, drefs = [], [], [], [], []
        ninstr = nbr = icalls = 0
        hi = {}  # reg -> lui/lis upper half
        for ins in listing.getInstructions(body, True):
            ninstr += 1
            ft = ins.getFlowType()
            if ft.isCall():
                fl = ins.getFlows()
                if fl:
                    calls.append(int(fl[0].getOffset()))
                else:
                    icalls += 1
            elif ft.isJump():
                fl = ins.getFlows()
                if ft.isConditional():
                    nbr += 1
                elif fl and int(fl[0].getOffset()) in entries and int(fl[0].getOffset()) != va:
                    calls.append(int(fl[0].getOffset()))  # tail call
            for ref in ins.getReferencesFrom():
                rt = ref.getReferenceType()
                if rt.isFlow():
                    continue
                ta = ref.getToAddress()
                if not ta.isMemoryAddress():
                    continue
                tv = int(ta.getOffset())
                if exec_set.contains(ta):
                    if tv in entries and tv != va:
                        calls.append(tv)  # function pointer taken: treat like a call edge
                    continue
                d = listing.getDataAt(ta)
                s = None
                if d is not None and d.hasStringValue():
                    try:
                        s = str(d.getValue())
                    except Exception:
                        s = None
                if s is None:
                    s = read_cstring(mem, ta)
                if s is not None and len(s) >= 4:
                    strings.append(s)
                    continue
                drefs.append(tv)
                # constant pool float/double?
                try:
                    if d is not None and d.getLength() == 8 and "double" in d.getDataType().getName().lower():
                        dv = float(d.getValue())
                        if dv != 0 and math.isfinite(dv):
                            fb = struct.unpack("<I", struct.pack("<f", dv))[0]
                            if abs(struct.unpack("<f", struct.pack("<I", fb))[0] - dv) <= abs(dv) * 1e-6:
                                if nice_float(fb):
                                    floats.append(fb)
                        continue
                    v = mem.getInt(ta) & 0xFFFFFFFF
                    if nice_float(v) and not in_mem(v):
                        floats.append(v)
                except Exception:
                    pass
            # immediates (MIPS lui/ori, PPC lis/ori/addi, x86 imm32)
            mn = ins.getMnemonicString().lower().lstrip("_")
            if is_ps2 or is_ppc:
                if mn in ("lui", "lis") and ins.getNumOperands() >= 2:
                    r = ins.getRegister(0)
                    sc = ins.getScalar(1)
                    if r is not None and sc is not None:
                        h = (int(sc.getUnsignedValue()) & 0xFFFF) << 16
                        hi[r.getName()] = h
                        if nice_float(h) and not in_mem(h):
                            floats.append(h)
                elif mn in ("ori", "addiu", "addi", "ori.") and ins.getNumOperands() >= 3:
                    rs = ins.getRegister(1)
                    sc = ins.getScalar(2)
                    if rs is not None and sc is not None and rs.getName() in hi:
                        lo = int(sc.getValue())
                        v = (hi[rs.getName()] + (lo if mn != "ori" else (lo & 0xFFFF))) & 0xFFFFFFFF
                        if not in_mem(v):
                            if nice_float(v):
                                floats.append(v)
                            elif v >= 0x10000:
                                ints.append(v)
            else:
                for oi in range(ins.getNumOperands()):
                    sc = ins.getScalar(oi)
                    if sc is None:
                        continue
                    v = int(sc.getUnsignedValue()) & 0xFFFFFFFF
                    if sc.bitLength() < 32 or in_mem(v):
                        continue
                    if nice_float(v):
                        floats.append(v)
                    elif 0x10000 <= v <= 0xFFFF0000:
                        ints.append(v)
        funcs.append(dict(name=name, addr=va, size=size, ninstr=ninstr, nbr=nbr, icalls=icalls,
                          calls=calls, strings=strings, floats=floats, ints=ints, drefs=drefs))
    return dict(prog=prog, funcs=funcs)


def decompile_all(program, prog: str, nthreads: int) -> None:
    from ghidra.app.decompiler import DecompInterface, DecompileOptions
    from ghidra.util.task import ConsoleTaskMonitor
    fm = program.getFunctionManager()
    funcs = [f for f in fm.getFunctions(True) if not f.isThunk() and not f.isExternal()]
    out = XREF / "decomp" / f"{prog}.jsonl"
    out.parent.mkdir(parents=True, exist_ok=True)
    done = set()
    if out.exists():
        for line in out.read_text().splitlines():
            try:
                done.add(json.loads(line)["addr"])
            except Exception:
                pass
    todo = [f for f in funcs if int(f.getEntryPoint().getOffset()) not in done]
    print(f"[{prog}] decompiling {len(todo)} / {len(funcs)}", flush=True)
    lock = threading.Lock()
    local = threading.local()
    fh = open(out, "a")
    counter = [0]

    def work(f):
        di = getattr(local, "di", None)
        if di is None:
            di = DecompInterface()
            di.setOptions(DecompileOptions())
            di.openProgram(program)
            local.di = di
            local.mon = ConsoleTaskMonitor()
        res = di.decompileFunction(f, 90, local.mon)
        df = res.getDecompiledFunction()
        text = str(df.getC()) if df is not None else ""
        with lock:
            fh.write(json.dumps({"addr": int(f.getEntryPoint().getOffset()), "c": text}) + "\n")
            counter[0] += 1
            if counter[0] % 500 == 0:
                fh.flush()
                print(f"[{prog}] decompiled {counter[0]}/{len(todo)}", flush=True)

    with ThreadPoolExecutor(nthreads) as ex:
        list(ex.map(work, todo))
    fh.close()


if __name__ == "__main__":
    sys.setrecursionlimit(20000)
    main()
