#!/usr/bin/env python3
"""Run original Game_Run from a PCSX2 P2S through nfmips and decode MP rows.

This streams compressed EE RAM snapshots from nfmips into mp_record's existing
v5 decoder, so the live-PINE and interpreter sources share one JSONL schema.
"""

import argparse
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

import mp_frame_trace as F
import mp_record as R

RAM_SIZE = 32 << 20


def _read_exact(stream, size):
    chunks = bytearray()
    while len(chunks) < size:
        part = stream.read(size - len(chunks))
        if not part:
            raise RuntimeError("nfmips ended inside the oracle snapshot stream")
        chunks.extend(part)
    return bytes(chunks)


def _write_pad_script(source, dest):
    source_rows = []
    with open(source, encoding="utf-8") as inp:
        for line_number, line in enumerate(inp, 1):
            if not line.strip():
                continue
            row = json.loads(line)
            if "frame" not in row:
                raise ValueError(f"{source}:{line_number}: missing frame")
            source_rows.append(row)
    if not source_rows:
        raise ValueError(f"{source}: no input rows")
    frames = [int(row["frame"]) for row in source_rows]
    if any(b != a + 1 for a, b in zip(frames, frames[1:])):
        raise ValueError(f"{source}: input rows must be contiguous")
    with open(dest, "w", encoding="ascii") as out:
        for row in source_rows:
            for pad in row.get("pad_all", []):
                port = pad.get("port")
                if port is None:
                    continue
                buttons = int(pad.get("w", 0))
                sticks = pad.get("s", (128, 128, 128, 128))
                if len(sticks) != 4:
                    raise ValueError(f"{source}: frame {row['frame']} has malformed stick data")
                values = [int(value) for value in sticks]
                if not 0 <= int(port) < 4 or not 0 <= buttons <= 0xFFFF or any(
                        value < 0 or value > 255 for value in values):
                    raise ValueError(f"{source}: frame {row['frame']} has invalid pad data")
                out.write(f"{row['frame']} {int(port)} {buttons} "
                          + " ".join(str(value) for value in values) + "\n")
    return frames[0], frames[-1]


class _StreamPine:
    def __init__(self, slot, *, command, expected_rows):
        self.process = subprocess.Popen(command, stdout=subprocess.PIPE)
        self.closed = False
        header = _read_exact(self.process.stdout, 16)
        magic, version, rows, ram_size = struct.unpack("<4sIII", header)
        if magic != b"NFOR" or version != 1:
            raise RuntimeError("unsupported nfmips oracle stream")
        if rows != expected_rows or ram_size != RAM_SIZE:
            raise RuntimeError(
                f"nfmips stream shape mismatch: rows={rows}, RAM={ram_size}")
        self.rows = rows
        self.index = 0
        self.ram = b""
        self.frame = self.done = self.timer_frame = 0
        self.emitted = 0
        self._load_snapshot()

    def _load_snapshot(self):
        if self.index >= self.rows:
            return False
        frame, done, timer_frame, compressed_size = struct.unpack(
            "<4I", _read_exact(self.process.stdout, 16))
        compressed = _read_exact(self.process.stdout, compressed_size)
        ram = zlib.decompress(compressed)
        if len(ram) != RAM_SIZE:
            raise RuntimeError(f"nfmips snapshot has {len(ram)} bytes, expected {RAM_SIZE}")
        self.frame, self.done, self.timer_frame = frame, done, timer_frame
        self.ram = ram
        self.index += 1
        return True

    def advance(self):
        return self._load_snapshot()

    def read_block(self, address, size):
        if address < 0 or size < 0 or address + size > RAM_SIZE:
            raise ValueError(f"oracle read outside EE RAM: {address:#x}+{size:#x}")
        return self.ram[address:address + size]

    def read32(self, address):
        return struct.unpack("<I", self.read_block(address, 4))[0]

    def read_ranges(self, ranges):
        return [self.read_block(address, size) for address, size in ranges]

    def close(self):
        if self.closed:
            return
        self.closed = True
        if self.process.stdout:
            self.process.stdout.close()
        try:
            code = self.process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            code = self.process.wait()
        if code:
            raise RuntimeError(f"nfmips oracle exited with status {code}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("state", help="initial PCSX2 .p2s checkpoint")
    ap.add_argument("out", help="output mp_record v5 JSONL")
    ap.add_argument("--elf", default=str(pathlib.Path.home() / "Projects/nightfire-data/ps2/ACTION.ELF"))
    ap.add_argument("--nfmips", default="build/nfmips")
    ap.add_argument("--rows", type=int, required=True,
                    help="number of snapshots, including the initial P2S row")
    ap.add_argument("--inputs", help="contiguous mp_record JSONL supplying pad_all values")
    ap.add_argument("--timeout", type=float, default=1200.0)
    args = ap.parse_args()
    if args.rows < 1:
        ap.error("--rows must be positive")
    for path in (args.state, args.elf, args.nfmips):
        if not pathlib.Path(path).is_file():
            ap.error(f"file does not exist: {path}")

    with tempfile.TemporaryDirectory(prefix="ee-oracle-") as temp_dir:
        pad_script = pathlib.Path(temp_dir) / "pads.txt"
        command = [str(pathlib.Path(args.nfmips).resolve()), args.elf, "mp-oracle",
                   "--state", args.state, "--rows", str(args.rows)]
        pad_span = None
        if args.inputs:
            pad_span = _write_pad_script(args.inputs, pad_script)
            command += ["--pads", str(pad_script)]

        instances = []

        def pine_factory(slot):
            pine = _StreamPine(slot, command=command, expected_rows=args.rows)
            if (pad_span and args.rows > 1
                    and not (pad_span[0] <= pine.frame + 1
                             and pad_span[1] >= pine.frame + args.rows - 1)):
                pine.close()
                raise ValueError(
                    f"input frames {pad_span[0]}..{pad_span[1]} do not cover "
                    f"frames {pine.frame + 1}..{pine.frame + args.rows - 1}")
            instances.append(pine)
            return pine
        def configure_ranges(pine, ranges):
            payload_size = sum(size for _, size in ranges)
            return {"payload_size": payload_size,
                    "slot_size": payload_size + F.SNAPSHOT_HEADER_SIZE,
                    "capacity": 1, "source": "nfmips"}
        def read_next(pine, state, timeout):
            if pine.emitted:
                if not pine.advance():
                    return None
            pine.emitted += 1
            chunks = pine.read_ranges(state["ranges"])
            return ({"done": pine.done, "frame": pine.frame,
                     "timer_frame": pine.timer_frame}, chunks)

        original_factory = R.Pine
        original_vpad = R.vpad
        original_configure = F.configure_ranges
        original_read_next = F.read_next
        original_argv = sys.argv
        try:
            R.Pine = pine_factory
            R.vpad = lambda *words: b"ok"
            F.configure_ranges = lambda pine, ranges: {
                **configure_ranges(pine, ranges), "ranges": list(ranges)}
            F.read_next = read_next
            sys.argv = ["mp_record.py", args.out, "--frames", str(args.rows - 1),
                        "--seedable", "--timeout", str(args.timeout)]
            result = R.main()
            instances[0].close()
            return result
        finally:
            sys.argv = original_argv
            F.configure_ranges = original_configure
            F.read_next = original_read_next
            R.Pine = original_factory
            R.vpad = original_vpad


if __name__ == "__main__":
    sys.exit(main())
