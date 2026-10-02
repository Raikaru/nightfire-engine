#!/usr/bin/env python3
"""Convert a retail Xbox XBE into a minimal PE32 image Ghidra's PE loader accepts.

Sections keep their XBE virtual addresses (ImageBase = XBE base, normally
0x10000).  Section/file alignment is 0x20 (every XBE section VA we have seen is
0x20-aligned), so RVAs map 1:1.  No import directory is emitted; the kernel
thunk table address is written to a sidecar JSON so the Ghidra import step can
label `xboxkrnl_<ordinal>` slots.

Usage: xbe2pe.py IN.xbe OUT.exe   (writes OUT.exe.json with header facts)
"""
from __future__ import annotations

import json
import struct
import sys

RETAIL_ENTRY_KEY = 0xA8FC57AB
RETAIL_THUNK_KEY = 0x5B6D40B6
ALIGN = 0x20


def parse_xbe(data: bytes) -> dict:
    if data[:4] != b"XBEH":
        raise SystemExit("not an XBE")
    u32 = lambda o: struct.unpack_from("<I", data, o)[0]
    base = u32(0x104)

    def cstr(va: int) -> str:
        o = va - base
        return data[o:data.index(b"\0", o)].decode("latin1")

    sections = []
    sh = u32(0x120) - base
    for i in range(u32(0x11C)):
        flags, va, vsize, raw, rsize, name_va = struct.unpack_from("<6I", data, sh + i * 0x38)
        sections.append(dict(name=cstr(name_va), flags=flags, va=va, vsize=vsize, raw=raw, rsize=rsize))
    libs = []
    lo = u32(0x164) - base
    for i in range(u32(0x160)):
        name = data[lo + i * 16:lo + i * 16 + 8].rstrip(b"\0").decode("latin1")
        libs.append([name, list(struct.unpack_from("<4H", data, lo + i * 16 + 8))])
    return dict(
        base=base,
        entry=u32(0x128) ^ RETAIL_ENTRY_KEY,
        kernel_thunk=u32(0x158) ^ RETAIL_THUNK_KEY,
        pdb_path=cstr(u32(0x14C)),
        sections=sections,
        libraries=libs,
    )

def is_code(s: dict) -> bool:
    """XBE marks .rdata/DOLBY executable; only real x86 sections count as code."""
    return bool(s["flags"] & 4) and s["name"] not in (".rdata", "DOLBY") and not s["name"].startswith("$$")


def build_pe(data: bytes, info: dict) -> bytes:
    base = info["base"]
    secs = info["sections"]
    nsec = len(secs)
    hdr_size = 0x40 + 4 + 20 + 224 + 40 * nsec
    hdr_size = (hdr_size + ALIGN - 1) & ~(ALIGN - 1)

    # Raw data placed at file offset == RVA so low-alignment PE rules hold.
    image_end = max(s["va"] + s["vsize"] for s in secs) - base
    image_end = (image_end + ALIGN - 1) & ~(ALIGN - 1)
    first_rva = min(s["va"] for s in secs) - base
    assert hdr_size <= first_rva, "headers overlap first section"
    out = bytearray(max(s["va"] - base + s["rsize"] for s in secs))

    dos = bytearray(0x40)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x40)
    out[0:0x40] = dos
    out[0x40:0x44] = b"PE\0\0"
    # COFF header: i386, nsec, characteristics EXECUTABLE|32BIT|RELOCS_STRIPPED
    struct.pack_into("<HHIIIHH", out, 0x44, 0x14C, nsec, 0, 0, 0, 224, 0x0103)
    code_size = sum(s["vsize"] for s in secs if is_code(s))
    opt = struct.pack(
        "<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII",
        0x10B, 7, 0, code_size, 0, 0,
        info["entry"] - base, 0x1000, 0, base,
        ALIGN, ALIGN, 4, 0, 0, 0, 4, 0, 0,
        image_end, hdr_size, 0, 3, 0,
        0x100000, 0x1000, 0x100000, 0x1000, 0, 16,
    )
    opt += b"\0" * (8 * 16)
    struct.pack_into(f"<{len(opt)}s", out, 0x58, opt)
    so = 0x58 + 224
    for s in secs:
        name = s["name"].encode("latin1")[:8].ljust(8, b"\0")
        ch = 0x40000000  # readable
        if is_code(s):
            ch |= 0x20000000 | 0x00000020  # execute | code
        else:
            ch |= 0x00000040  # initialized data
        if s["flags"] & 1:
            ch |= 0x80000000  # writable
        rva = s["va"] - base
        struct.pack_into("<8sIIIIIIHHI", out, so, name, s["vsize"], rva, s["rsize"], rva, 0, 0, 0, 0, ch)
        out[rva:rva + s["rsize"]] = data[s["raw"]:s["raw"] + s["rsize"]]
        so += 40
    return bytes(out)


def main() -> None:
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()
    info = parse_xbe(data)
    open(dst, "wb").write(build_pe(data, info))
    with open(dst + ".json", "w") as f:
        json.dump(info, f, indent=1)
    print(f"{src}: entry={info['entry']:#x} thunk={info['kernel_thunk']:#x} pdb={info['pdb_path']}")


if __name__ == "__main__":
    main()
