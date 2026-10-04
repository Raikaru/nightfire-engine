#!/usr/bin/env python3
"""Fill MP recording gaps from P2S checkpoints and validate PCSX2 overlaps."""

import argparse
import collections
import json
import math
import os
import pathlib
import subprocess
import sys
import tempfile

import mp_compare


def _records(path):
    previous = None
    with open(path, encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            row = json.loads(line)
            frame = row.get("frame")
            if isinstance(frame, bool) or not isinstance(frame, int):
                raise ValueError(f"{path}:{line_number}: frame must be an integer")
            if previous is not None and frame <= previous:
                raise ValueError(
                    f"{path}:{line_number}: frames must be strictly increasing; "
                    f"{frame} follows {previous}")
            previous = frame
            yield row


def _checkpoints(directory):
    result = []
    for metadata_path in pathlib.Path(directory).glob("frame-*.json"):
        with metadata_path.open(encoding="utf-8") as stream:
            metadata = json.load(stream)
        frame = metadata.get("sample_frame")
        if isinstance(frame, bool) or not isinstance(frame, int):
            raise ValueError(f"{metadata_path}: missing integer sample_frame")
        state_name = metadata.get("savestate", metadata_path.with_suffix(".p2s").name)
        state = metadata_path.parent / state_name
        if not state.is_file():
            raise ValueError(f"{metadata_path}: checkpoint state is missing: {state}")
        if metadata_path.stem != f"frame-{frame:08d}":
            raise ValueError(f"{metadata_path}: filename does not match sample_frame {frame}")
        result.append((frame, state))
    result.sort(key=lambda item: item[0])
    if len(result) < 2:
        raise ValueError(f"{directory}: need at least two P2S checkpoints")
    if any(b[0] <= a[0] for a, b in zip(result, result[1:])):
        raise ValueError(f"{directory}: checkpoint frames must be unique and increasing")
    return result


def _flatten(row):
    return mp_compare.flatten_fields(row)


def _compare_rows(reference, generated, tolerance):
    # Checkpoint and partial/resync markers describe host-side capture state,
    # not EE state. A partial row can validate only the fields it captured.
    partial = reference.get("partial") is True
    metadata = {"checkpoint", "partial", "resync", "state_missing", "seed_ready"}
    reference = {key: value for key, value in reference.items() if key not in metadata}
    generated = {key: value for key, value in generated.items() if key not in metadata}
    expected = _flatten(reference)
    actual = _flatten(generated)
    if partial:
        expected = {field: value for field, value in expected.items()
                    if not field.endswith(".complete")}
        actual = {field: value for field, value in actual.items()
                  if not field.endswith(".complete")}
    mismatches = []
    fields = expected.keys() & actual.keys() if partial else expected.keys() | actual.keys()
    for field in sorted(fields):
        if field not in expected:
            mismatches.append((field, "<missing>", actual[field], None))
        elif field not in actual:
            mismatches.append((field, expected[field], "<missing>", None))
        else:
            delta, equal = mp_compare.residual(expected[field], actual[field], tolerance)
            if not equal:
                mismatches.append((field, expected[field], actual[field], delta))
    return mismatches


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate dense EE MP rows between P2S checkpoints, "
                    "then validate every overlapping PCSX2 row.")
    parser.add_argument("checkpoints", help="mp_record checkpoint directory")
    parser.add_argument("reference", help="PCSX2 mp_record v5 JSONL to validate against")
    parser.add_argument("out", help="write filled JSONL only if all overlaps match")
    parser.add_argument("--elf", default=str(pathlib.Path.home() / "Projects/nightfire-data/ps2/ACTION.ELF"))
    parser.add_argument("--nfmips", default="build/nfmips")
    parser.add_argument("--inputs", help="contiguous mp_record JSONL supplying pad values")
    parser.add_argument("--tolerance", type=float, default=0.001,
                        help="absolute tolerance for float fields (default: 0.001)")
    parser.add_argument("--verbose", action="store_true",
                        help="print every mismatch, rather than the first per frame")
    parser.add_argument("--watch-human-hp", type=int, choices=range(4))
    parser.add_argument("--weapon-anim-raw", action="store_true")
    parser.add_argument("--game-flow", dest="game_flow", action="store_true", default=True)
    parser.add_argument("--no-game-flow", dest="game_flow", action="store_false")
    parser.add_argument("--trace-rng", action="store_true")
    parser.add_argument("--give-weapon", action="append", type=int, default=[])
    parser.add_argument("--timeout", type=float, default=1200.0,
                        help="per-checkpoint replay timeout in seconds")
    args = parser.parse_args(argv)
    if not math.isfinite(args.tolerance) or args.tolerance < 0:
        parser.error("--tolerance must be finite and non-negative")
    for path in (args.reference, args.elf, args.nfmips):
        if not pathlib.Path(path).is_file():
            parser.error(f"file does not exist: {path}")
    checkpoints = _checkpoints(args.checkpoints)
    output = pathlib.Path(args.out).expanduser().resolve()
    reference = pathlib.Path(args.reference).expanduser().resolve()
    if output == reference:
        parser.error("out and reference must be different files")
    output.parent.mkdir(parents=True, exist_ok=True)

    field_counts = collections.Counter()
    first_mismatch = None
    common_rows = 0
    unmatched_segments = []
    total_rows = 0
    last_frame = None

    with tempfile.TemporaryDirectory(prefix="ee-oracle-fill-", dir=output.parent) as temp_dir:
        temp_dir = pathlib.Path(temp_dir)
        staged = temp_dir / "filled.jsonl"
        reference_rows = iter(_records(reference))
        ref = next(reference_rows, None)
        with staged.open("w", encoding="utf-8") as out:
            for segment_index, ((first_frame, state), (end_frame, _)) in enumerate(
                    zip(checkpoints, checkpoints[1:]), 1):
                row_count = end_frame - first_frame + 1
                segment_path = temp_dir / f"segment-{segment_index:05d}.jsonl"
                command = [sys.executable, str(pathlib.Path(__file__).with_name("ee_oracle.py")),
                           str(state), str(segment_path), "--rows", str(row_count),
                           "--elf", str(pathlib.Path(args.elf).resolve()),
                           "--nfmips", str(pathlib.Path(args.nfmips).resolve()),
                           "--timeout", str(args.timeout)]
                if args.inputs:
                    command += ["--inputs", str(pathlib.Path(args.inputs).resolve())]
                if args.watch_human_hp is not None:
                    command += ["--watch-human-hp", str(args.watch_human_hp)]
                if args.weapon_anim_raw:
                    command.append("--weapon-anim-raw")
                if not args.game_flow:
                    command.append("--no-game-flow")
                if args.trace_rng:
                    command.append("--trace-rng")
                for weapon in args.give_weapon:
                    command += ["--give-weapon", str(weapon)]
                subprocess.run(command, check=True)

                segment_common = 0
                segment_rows = 0
                segment_start = segment_end = None
                for generated in _records(segment_path):
                    frame = generated["frame"]
                    if segment_rows == 0:
                        segment_start = frame
                    segment_rows += 1
                    segment_end = frame
                    if not first_frame <= frame <= end_frame:
                        raise ValueError(
                            f"segment {first_frame}..{end_frame} emitted frame {frame}")
                    duplicate_boundary = last_frame is not None and frame == last_frame
                    if (last_frame is not None and not duplicate_boundary
                            and frame != last_frame + 1):
                        raise ValueError(f"filled rows have a gap before frame {frame}")
                    while ref is not None and ref["frame"] < frame:
                        ref = next(reference_rows, None)
                    if ref is not None and ref["frame"] == frame:
                        mismatches = _compare_rows(ref, generated, args.tolerance)
                        segment_common += 1
                        if not duplicate_boundary:
                            common_rows += 1
                        if mismatches:
                            for field, expected, actual, delta in mismatches:
                                field_counts[field] += 1
                            if first_mismatch is None:
                                first_mismatch = (frame, mismatches[0])
                            if args.verbose:
                                for field, expected, actual, delta in mismatches:
                                    print(f"frame {frame}: {field}: PCSX2={expected!r} "
                                          f"EE={actual!r} residual={delta!r}", file=sys.stderr)
                            elif first_mismatch == (frame, mismatches[0]):
                                field, expected, actual, delta = mismatches[0]
                                print(f"frame {frame}: {len(mismatches)} differing fields; "
                                      f"first {field}: PCSX2={expected!r} EE={actual!r} "
                                      f"residual={delta!r}", file=sys.stderr)
                    if duplicate_boundary:
                        continue
                    out.write(json.dumps(generated, separators=(",", ":")) + "\n")
                    last_frame = frame
                    total_rows += 1
                if (segment_rows != row_count or segment_start != first_frame
                        or segment_end != end_frame):
                    raise ValueError(
                        f"segment {first_frame}..{end_frame} emitted "
                        f"{segment_rows} rows ({segment_start}..{segment_end})")
                if segment_common == 0:
                    unmatched_segments.append((first_frame, end_frame))
                print(f"checkpoint segment {first_frame}..{end_frame}: "
                      f"{segment_common} overlapping PCSX2 rows")

        if unmatched_segments:
            for first_frame, end_frame in unmatched_segments:
                print(f"unverified checkpoint segment {first_frame}..{end_frame}: "
                      "no overlapping PCSX2 rows", file=sys.stderr)
        print(f"filled rows: {total_rows}; overlapping rows checked: {common_rows}; "
              f"float tolerance: {args.tolerance:g}")
        if first_mismatch is not None:
            frame, (field, expected, actual, delta) = first_mismatch
            print(f"first mismatch: frame {frame}, {field}: PCSX2={expected!r} "
                  f"EE={actual!r} residual={delta!r}", file=sys.stderr)
            for field, count in field_counts.most_common(30):
                print(f"  {field}: {count} mismatches", file=sys.stderr)
        valid = common_rows > 0 and not unmatched_segments and first_mismatch is None
        if not valid:
            print(f"not writing {output}: fill overlap validation failed", file=sys.stderr)
            return 1
        os.replace(staged, output)
    print(f"validated dense fill written to {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
