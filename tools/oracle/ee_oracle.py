#!/usr/bin/env python3
"""Replay multiplayer rows from a PCSX2 P2S through GameFlow_Main.

The default runs Game_Run and Game_Draw, including draw-side visibility state,
while emitting the same mp_record v6 snapshot schema as the live recorder,
including player body AnimSet/layer state captured directly from EE RAM.
Use ``ee_oracle.py fill CHECKPOINT_DIR PCSX2.jsonl OUT.jsonl`` to replay dense
checkpoint intervals and validate every overlapping PCSX2 row before writing.
"""

import argparse
import json
import pathlib
import struct
import subprocess
import sys
import math
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


def _write_frame_timing_script(source, dest):
    source_rows = []
    with open(source, encoding="utf-8") as inp:
        for line_number, line in enumerate(inp, 1):
            if not line.strip():
                continue
            row = json.loads(line)
            if "frame" not in row:
                raise ValueError(f"{source}:{line_number}: missing frame")
            source_rows.append(row)
    if not source_rows or not any(
            "rate" in row or "frame_rate_int" in row for row in source_rows):
        return False
    rows_written = 0
    with open(dest, "w", encoding="ascii") as out:
        for row in source_rows:
            if "rate" not in row and "frame_rate_int" not in row:
                continue
            rate = float(row.get("rate", row.get("frame_rate_int", 0)))
            rate_int = int(row.get("frame_rate_int", round(rate)))
            if not math.isfinite(rate) or rate <= 0 or rate_int <= 0:
                raise ValueError(f"{source}: frame {row['frame']} has invalid rate")
            rate_mul = float(row.get("frame_rate_mul", 60.0 / rate))
            rec_rate = float(row.get("rec_frame_rate", 1.0 / rate))
            vblank_count = int(row.get("vblank_count", 0))
            if (not math.isfinite(rate_mul) or not math.isfinite(rec_rate)
                    or rate_mul <= 0 or rec_rate <= 0 or vblank_count < 0):
                raise ValueError(f"{source}: frame {row['frame']} has invalid timing")
            out.write(f"{int(row['frame'])} {rate_int} "
                      f"{rate:.9g} {rate_mul:.9g} {rec_rate:.9g} {vblank_count}\n")
            rows_written += 1
    return rows_written > 0


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
    if len(sys.argv) > 1 and sys.argv[1] == "fill":
        import ee_oracle_fill
        return ee_oracle_fill.main(sys.argv[2:])
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("state", help="initial PCSX2 .p2s checkpoint")
    ap.add_argument("out", help="output mp_record v6 JSONL")
    ap.add_argument("--elf", default=str(pathlib.Path.home() / "Projects/nightfire-data/ps2/ACTION.ELF"))
    ap.add_argument("--nfmips", default="build/nfmips")
    ap.add_argument("--rows", type=int, required=True,
                    help="number of snapshots, including the initial P2S row")
    ap.add_argument("--inputs", help="contiguous JSONL supplying pad inputs")
    ap.add_argument("--timing-inputs", help="JSONL supplying recorded per-frame timing globals")
    ap.add_argument("--watch-human-hp", type=int, choices=range(4),
                    help="log writes to one MP human's BLData HP, including PC and $ra")
    ap.add_argument("--weapon-anim-raw", action="store_true",
                    help="capture the full human weapon-animation object range")
    ap.add_argument("--sample-at-game-run-hook", action="store_true",
                    help="snapshot inside Game_Run before Game_Draw completes")
    ap.add_argument("--game-flow", dest="game_flow", action="store_true", default=True,
                    help="run GameFlow_Main and its view capture (default)")
    ap.add_argument("--no-game-flow", dest="game_flow", action="store_false",
                    help="run Game_Run only, without draw-side visibility state")
    ap.add_argument("--trace-rng", action="store_true",
                    help="print each Rand_Random caller and result to stderr")
    ap.add_argument("--give-weapon", action="append", type=int, default=[],
                    help="use Player_EquipWeapon to grant and select a human MP weapon")
    ap.add_argument("--timeout", type=float, default=1200.0)
    ap.add_argument("--heap-dump", help="write the final EE Mem heap block map")
    args = ap.parse_args()
    if args.sample_at_game_run_hook and not args.game_flow:
        ap.error("--sample-at-game-run-hook requires the default GameFlow_Main")
    if args.rows < 1:
        ap.error("--rows must be positive")
    if args.watch_human_hp is not None:
        command_watch_human_hp = ["--watch-human-hp", str(args.watch_human_hp)]
    else:
        command_watch_human_hp = []
    for path in (args.state, args.elf, args.nfmips):
        if not pathlib.Path(path).is_file():
            ap.error(f"file does not exist: {path}")

    with tempfile.TemporaryDirectory(prefix="ee-oracle-") as temp_dir:
        pad_script = pathlib.Path(temp_dir) / "pads.txt"
        timing_script = pathlib.Path(temp_dir) / "frame-timing.txt"
        command = [str(pathlib.Path(args.nfmips).resolve()), args.elf, "mp-oracle",
                   "--state", args.state, "--rows", str(args.rows)]
        command += command_watch_human_hp
        if args.trace_rng:
            command.append("--trace-rng")
        if args.sample_at_game_run_hook:
            command.append("--sample-at-game-run-hook")
        if not args.game_flow:
            command.append("--no-game-flow")
        for weapon in args.give_weapon:
            command += ["--give-weapon", str(weapon)]
        pad_span = None
        if args.heap_dump:
            command += ["--heap-dump", str(pathlib.Path(args.heap_dump).expanduser())]
        if args.inputs:
            pad_span = _write_pad_script(args.inputs, pad_script)
            command += ["--pads", str(pad_script)]
        timing_source = args.timing_inputs or args.inputs
        if timing_source and _write_frame_timing_script(timing_source, timing_script):
            command += ["--frame-timing", str(timing_script)]

        instances = []

        def pine_factory(slot):
            pine = _StreamPine(slot, command=command, expected_rows=args.rows)
            if pad_span and args.rows > 1:
                first_input_frame = pine.frame + 1
                last_input_frame = pine.frame + args.rows - 1
                if not (pad_span[0] <= first_input_frame
                        and pad_span[1] >= last_input_frame):
                    pine.close()
                    raise ValueError(
                        f"input frames {pad_span[0]}..{pad_span[1]} do not cover "
                        f"frames {first_input_frame}..{last_input_frame}")
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
                     "timer_frame": pine.timer_frame, "overflow_delta": 0}, chunks)

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
            record_args = ["mp_record.py", args.out, "--frames", str(args.rows - 1),
                           "--seedable", "--timeout", str(args.timeout)]
            if args.weapon_anim_raw:
                record_args.append("--weapon-anim-raw")
            sys.argv = record_args
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
