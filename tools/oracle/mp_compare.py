"""MP comparison harness: summarize/record-compare oracle recordings and diff them
against engine traces (see src/game/mp_trace.* for the engine-side emitter).

  mp_compare.py summary rec.jsonl
      match overview: frames, game-time span, kills/deaths/points per slot,
      team scores, end state, pickup takes, estimated shots per bot, bot state
      histogram, RNG stream check, and projectile coverage when recorded.
  mp_compare.py determinism a.jsonl b.jsonl
      two recordings from the same savestate: max per-field divergence
      (proves game + recorder determinism; bots must fight identically).
  mp_compare.py diff oracle.jsonl engine.jsonl
      exact-frame-aligned per-field residuals and the first divergence.
      Oracle frames absent from the engine and vice versa are reported.
      --verbose prints every differing field for every common frame.
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
    if "mpg" in full[0]:
        g0, g1 = mpg_global(full[0]["mpg"]), mpg_global(full[-1]["mpg"])
        elapsed0, elapsed1 = g0["elapsed"], g1["elapsed"]
        state0, state1 = g0["state"], g1["state"]
        teams = (g1["team0"], g1["team1"])
    else:
        first, last = full[0], full[-1]
        elapsed0 = first.get("total_elapsed", first.get("elapsed", 0.0))
        elapsed1 = last.get("total_elapsed", last.get("elapsed", 0.0))
        state0 = first.get("state_code", first.get("state", 0))
        state1 = last.get("state_code", last.get("state", 0))
        team_values = last.get("teams", (0.0, 0.0))
        teams = tuple(team_values[:2]) if len(team_values) >= 2 else (0.0, 0.0)
    print(f"elapsed: {elapsed0:.1f}..{elapsed1:.1f} s, "
          f"state {state0}->{state1}, teams {teams[0]:.0f}/{teams[1]:.0f}")

    def score_for(record, slot):
        if "mpg" in record:
            return mpg_slot(record["mpg"], slot)
        row = next((row for row in record.get("scores", [])
                    if row.get("slot") == slot), None)
        if row is None:
            return 0, 0, 0.0
        return row.get("k", 0), row.get("d", 0), row.get("p", 0.0)

    for s in range(8):
        k0, d0, p0 = score_for(full[0], s)
        k1, d1, p1 = score_for(full[-1], s)
        if k0 or d0 or p0 or k1 or d1 or p1 or full[-1]["pl"][s]:
            print(f"  slot{s}: kills {k0}->{k1} deaths {d0}->{d1} points {p0:.0f}->{p1:.0f}")
    from collections import Counter
    states = Counter()
    for r in full:
        for s in range(4, 8):
            p = r["pl"][s]
            if p:
                bot_rows = r.get("bots", [])
                bot_state = (bot_rows[s - 4].get("state")
                             if s - 4 < len(bot_rows) and isinstance(bot_rows[s - 4], dict)
                             else None)
                states[(s, p.get("state", p.get("substate", bot_state)))] += 1
    print("  bot states (top):", states.most_common(8))
    for s in range(4, 8):
        b0, b1 = full[0]["pl"][s], full[-1]["pl"][s]
        if b0 and b1 and "clip" in b0 and "clip" in b1:
            fired = sum(max(0, a - b) for a, b in zip(b0["clip"], b1["clip"]) if a and b)
            stat = b1.get("stat", {})
            weapon = stat.get("curweap", b1.get("weap", "?"))
            print(f"  bot{s - 4} weapon {weapon} clips net {fired}, hp {b0.get('bhp', b0.get('hp'))}->{b1.get('bhp', b1.get('hp'))}")
    # pickups taken: st 1->2 transitions
    takes = 0
    prev = {}
    for r in full:
        cur = {p.get("obj", p.get("idx")): p.get("st") for p in r.get("pk", [])}
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
    projectile_rows = [r for r in full if "projectiles_available" in r]
    if projectile_rows:
        active = [len(r.get("projectiles", [])) for r in projectile_rows]
        gaps = sum(not r["projectiles_available"] for r in projectile_rows)
        print(f"  projectiles: peak {max(active)}, unavailable {gaps}/{len(projectile_rows)} samples")

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

def flatten_fields(value, prefix="", fields=None):
    if fields is None:
        fields = {}
    if isinstance(value, dict):
        for key, child in value.items():
            flatten_fields(child, f"{prefix}.{key}" if prefix else str(key), fields)
    elif isinstance(value, (list, tuple)):
        for i, child in enumerate(value):
            flatten_fields(child, f"{prefix}[{i}]", fields)
    else:
        fields[prefix] = value
    return fields


def diff_fields(record):
    fields = {}
    if "timer_frame" in record:
        fields["timer_frame"] = record["timer_frame"]
    players = record.get("pl", [])
    bots = record.get("bots", [])
    for slot in range(max(8, len(players))):
        player = players[slot] if slot < len(players) else None
        path = f"pl[{slot}]"
        if player is None:
            fields[f"{path}.present"] = False
            continue
        fields[f"{path}.present"] = True
        player = dict(player)
        if "hp" not in player and "bhp" in player:
            player["hp"] = player["bhp"]
        player.pop("bhp", None)
        if "autolock_target_slot" in player:
            target = player["autolock_target_slot"]
            player["lock_victim"] = -1 if target is None else target
        if "aim_flags" in player:
            player["aim"] = bool(player["aim_flags"] & 1)
        if isinstance(player.get("bl_raw"), str):
            bl = bytes.fromhex(player["bl_raw"])
            if len(bl) >= 0x8f0:
                player["zoom"] = struct.unpack_from("<f", bl, 0x8d0)[0]
                player["lock_yaw"] = struct.unpack_from("<f", bl, 0x120)[0]
                player["lock_pitch"] = struct.unpack_from("<f", bl, 0x124)[0]
        if "out" not in player and player.get("type") in (0x11, 0x12):
            player["out"] = True
        elif "out" not in player and "type" in player:
            player["out"] = False
        if slot < 4 and "dead" not in player and "state" in player:
            player["dead"] = player["state"] in (2, 3)
        if isinstance(player.get("weapon_timers"), dict):
            player["weapon_timers"] = {k: v for k, v in player["weapon_timers"].items() if k != "weapon_anim"}
        if slot >= 4 and "substate" in player:
            player.setdefault("state", player["substate"])
        if slot >= 4 and isinstance(player.get("goal"), str) and isinstance(player.get("stat"), dict):
            goal_raw = bytes.fromhex(player["goal"])
            active_goal = int(player["stat"].get("goalslot", 0xFF))
            active_goal = -1 if active_goal == 0xFF else active_goal
            player["active_goal"] = active_goal
            player["goal_type"] = -1
            player["goal_kind"] = -1
            player["goal_target"] = -1
            if 0 <= active_goal < 2 and len(goal_raw) >= (active_goal + 1) * 0x50:
                offset = active_goal * 0x50
                player["goal_type"] = goal_raw[offset + 0x45]
                player["goal_kind"] = goal_raw[offset + 0x4A]
                if player["goal_type"] != 0:
                    target_ptr = struct.unpack_from("<I", goal_raw, offset + 0x3C)[0]
                    if target_ptr == 0:
                        player["goal_target"] = -1
                    else:
                        refs = record.get("bot_goal_targets", [])
                        ref = next((r for r in refs if r.get("bot_slot") == slot
                                    and r.get("goal_slot") == active_goal), None)
                        if ref is not None:
                            for field in ("pickup_index", "objective_index", "target_slot"):
                                if field in ref:
                                    player["goal_target"] = ref[field]
                                    break
                            else:
                                player.pop("goal_target")
                        else:
                            player.pop("goal_target")
        common = {"pos", "yaw", "type", "state", "hp", "alive", "out", "mp_status"}
        if slot < 4:
            common |= {"arm", "dead", "vel", "fall_vel", "substate", "pitch", "foot", "zoom", "aim",
                       "lock_victim", "lock_yaw", "lock_pitch", "ammo_pool", "weapon_slots",
                       "weapon_timers", "weapon_anim_state"}
        else:
            common |= {"active_goal", "goal_type", "goal_kind", "goal_target"}
        player = {key: value for key, value in player.items() if key in common}
        flatten_fields(player, path, fields)

    rng = record.get("rng_words")
    if rng is None:
        rng = record.get("rng")
    if isinstance(rng, str):
        raw = bytes.fromhex(rng)
        rng = list(struct.unpack_from(f"<{min(2, len(raw) // 4)}I", raw))
    if isinstance(rng, (list, tuple)):
        flatten_fields(rng[:2], "rng", fields)

    if "mpg" in record:
        try:
            match = mpg_global(record["mpg"])
            fields["teams[0]"] = match["team0"]
            fields["teams[1]"] = match["team1"]
            fields["state_code"] = match["state"]
            raw_mpg = bytes.fromhex(record["mpg"])
            for slot in range(8):
                kills, deaths, points = mpg_slot(record["mpg"], slot)
                fields[f"scores[{slot}].k"] = kills
                fields[f"scores[{slot}].d"] = deaths
                fields[f"scores[{slot}].p"] = points
                if slot < len(players) and players[slot] is not None:
                    fields[f"pl[{slot}].mp_status"] = struct.unpack_from("<H", raw_mpg, slot * 0x30 + 0x26)[0]
        except (ValueError, struct.error):
            fields["mpg.invalid"] = True

    if "teams" in record:
        flatten_fields(record["teams"], "teams", fields)
    if "mpg" not in record:
        fields["state_code"] = record.get("state_code", record.get("state"))
    for row in record.get("scores", []):
        slot = row.get("slot", row.get("idx"))
        if slot is not None:
            for key in ("k", "d", "p"):
                if key in row:
                    fields[f"scores[{slot}].{key}"] = row[key]
    if "mpg" not in record and "scores" in record:
        for slot in range(8):
            for key in ("k", "d", "p"):
                fields.setdefault(f"scores[{slot}].{key}", 0)
    for row in record.get("pk", []):
        idx = row.get("idx")
        if idx is None:
            continue
        pickup = dict(row)
        if "respawn_units" in pickup:
            pickup["units"] = pickup["respawn_units"]
            if "remaining_s" not in pickup:
                rate = float(record.get("rate", 30.0))
                age = max(0.0, (int(record["frame"]) - int(pickup.get("stamp", record["frame"]))) / rate)
                pickup["remaining_s"] = max(0.0, float(pickup["units"]) * 10.0 - age) if pickup.get("st") == 2 else 0.0
        for key in ("st", "cat", "item", "units", "remaining_s", "stamp", "lifetime_frames",
                    "amount", "radar_hidden", "pos", "visit_until"):
            if key in pickup:
                flatten_fields(pickup[key], f"pk[{idx}].{key}", fields)
    objective_rows = record.get("objectives", record.get("objs", []))
    ordinals = {}
    for row in objective_rows:
        kind, team = row.get("kind"), row.get("team")
        identity = (kind, team)
        ordinal = ordinals.get(identity, 0)
        ordinals[identity] = ordinal + 1
        path = f"objs[{kind},{team},{ordinal}]"
        objective = {
            "kind": kind,
            "team": team,
            "state": row.get("state"),
            "timer": row.get("timer"),
            "carrier": row.get("carrier_slot", row.get("carrier", -1)),
            "last_damager": row.get("last_damager"),
            "capturer": row.get("capturer"),
        }
        if objective["carrier"] is None:
            objective["carrier"] = -1
        if "obj_raw" in row:
            if kind in (0, 1, 3) and "pos_0x40" in row:
                objective["pos"] = row["pos_0x40"]
        elif "pos" in row:
            objective["pos"] = row["pos"]
        for key, value in objective.items():
            if value is not None:
                flatten_fields(value, f"{path}.{key}", fields)
    if "inputs" in record:
        flatten_fields(record["inputs"], "inputs", fields)
    if "pad_all" in record:
        value = [{k: v for k, v in row.items() if k != "settings_raw"} for row in record["pad_all"]]
        flatten_fields(value, "pad_all", fields)
    if "projectiles_available" in record:
        fields["projectiles_available"] = record["projectiles_available"]
    if "projectiles" in record:
        projectile_rows = [dict(p) for p in record["projectiles"]]
        for projectile in projectile_rows:
            if "in_air" not in projectile and "resting" in projectile:
                projectile["in_air"] = not projectile["resting"]
            projectile.pop("resting", None)
            projectile.pop("yaw", None)

        def projectile_key(projectile):
            owner = projectile.get("owner_slot", projectile.get("owner"))
            weapon = projectile.get("weapon_id", projectile.get("weapon"))
            pos = projectile.get("pos", [])
            return (owner if isinstance(owner, int) else -1,
                    weapon if isinstance(weapon, int) else -1, *pos)

        projectile_rows.sort(key=projectile_key)
        fields["projectiles.count"] = len(projectile_rows)
        for i, projectile in enumerate(projectile_rows):
            path = f"projectiles[{i}]"
            fields[f"{path}.weapon"] = projectile.get("weapon_id", projectile.get("weapon"))
            fields[f"{path}.owner"] = projectile.get("owner_slot", projectile.get("owner"))
            for key in ("pos", "dir", "speed", "travelled", "timer", "state",
                        "bounces", "in_air"):
                if key in projectile:
                    flatten_fields(projectile[key], f"{path}.{key}", fields)
    return fields


def residual(a, b, tolerance):
    if isinstance(a, bool) or isinstance(b, bool):
        return (0.0, a == b)
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        delta = abs(float(a) - float(b))
        return delta, delta <= tolerance if isinstance(a, float) or isinstance(b, float) else delta == 0.0
    return (0.0 if a == b else None), a == b

def index_records(path, require_players=False):
    indexed = {}
    for row in load(path):
        if "frame" not in row or (require_players and "pl" not in row):
            continue
        frame = row["frame"]
        if isinstance(frame, bool) or not isinstance(frame, int):
            raise ValueError(f"{path}: frame must be an integer, got {frame!r}")
        if frame in indexed:
            raise ValueError(f"{path}: duplicate frame {frame}")
        indexed[frame] = row
    return indexed




def _rng_kind(event, source):
    kind = event.get("kind" if source else "call")
    if source:
        return {
            "Rand_Random": "Random",
            "Rand_Rand": "RandInt",
            "Rand_FRand": "FRand",
            "Rand_FRand_MVar2": "MVar2",
        }.get(kind, kind)
    return kind


def _rng_result(event, source):
    key = "result_u32" if source else "result_bits"
    value = event.get(key)
    if value is None and source:
        value = event.get("result_hex")
    if isinstance(value, str):
        return int(value, 0)
    return value


def _rng_caller(event, source):
    if source:
        callsite = event.get("callsite")
        if callsite is None and event.get("caller_ra") is not None:
            callsite = (int(event["caller_ra"]) - 8) & 0xFFFFFFFF
        return f"callsite=0x{int(callsite):08x}" if callsite is not None else "caller unknown"
    file = event.get("file", "?")
    line = event.get("line", "?")
    function = event.get("function", "?")
    return f"{file}:{line} ({function})"


def diff_rng_events(oracle, engine, common):
    comparable = [frame for frame in common
                  if "rng_calls" in oracle[frame] and "rng_calls" in engine[frame]]
    if not comparable:
        print("RNG sequence: unavailable (both files need aligned per-call rng_calls records)")
        return
    if len(comparable) != len(common):
        print(f"  per-call trace coverage: {len(comparable)}/{len(common)} aligned frames")
    oracle_rows = [oracle[frame] for frame in comparable]
    engine_rows = [engine[frame] for frame in comparable]

    def by_frame(records):
        # PINE drains RNG events after sampling the Game_Run ring. An event
        # tagged for frame N can therefore live on the JSONL row for N-1.
        events = {}
        for row in records.values():
            for event in row.get("rng_calls", []):
                frame = int(event.get("frame", row["frame"]))
                events.setdefault(frame, []).append(event)
        return events

    oracle_events = by_frame(oracle)
    engine_events = by_frame(engine)
    divergent = []
    total_oracle = total_engine = 0
    for frame in comparable:
        expected = oracle_events.get(frame, [])
        actual = engine_events.get(frame, [])
        total_oracle += len(expected)
        total_engine += len(actual)
        first = None
        for index in range(max(len(expected), len(actual))):
            source = expected[index] if index < len(expected) else None
            target = actual[index] if index < len(actual) else None
            if source is None or target is None:
                first = index
                break
            if (_rng_kind(source, True) != _rng_kind(target, False) or
                    _rng_result(source, True) != _rng_result(target, False)):
                first = index
                break
        if first is not None:
            divergent.append((frame, expected, actual, first))

    print(f"RNG sequence: {len(divergent)}/{len(comparable)} traced frames differ; "
          f"{total_oracle} oracle calls, {total_engine} engine calls")
    if not divergent:
        print("  first RNG divergence: none")
    else:
        frame, expected, actual, index = divergent[0]
        source = expected[index] if index < len(expected) else None
        target = actual[index] if index < len(actual) else None
        def describe(event, is_source):
            if event is None:
                return "<no call>"
            return (f"{_rng_kind(event, is_source)} "
                    f"result=0x{int(_rng_result(event, is_source) or 0):08x} "
                    f"{_rng_caller(event, is_source)}")
        print(f"  first RNG divergence: frame {frame}, call #{index}; "
              f"oracle {len(expected)}, engine {len(actual)}")
        print(f"    oracle: {describe(source, True)}")
        print(f"    engine: {describe(target, False)}")

    source_lost = sum(int(row.get("rng_trace", {}).get("lost_events", 0))
                      + int(row.get("rng_trace", {}).get("overflow_delta", 0))
                      for row in oracle_rows)
    engine_dropped = sum(int(row.get("rng_call_dropped", 0)) for row in engine_rows)
    if source_lost or engine_dropped:
        print(f"  incomplete trace: oracle lost/overflow {source_lost}, "
              f"engine dropped {engine_dropped}")


def diff(oracle_path, engine_path, tolerance=0.001, verbose=False):
    if not math.isfinite(tolerance) or tolerance < 0:
        raise ValueError("tolerance must be finite and non-negative")
    oracle = index_records(oracle_path, require_players=True)
    engine = index_records(engine_path)
    common = sorted(oracle.keys() & engine.keys())
    print(f"frame-aligned frames: {len(common)} (oracle {len(oracle)}, engine {len(engine)}); "
          f"oracle-only {len(oracle.keys() - engine.keys())}, engine-only {len(engine.keys() - oracle.keys())}")
    if not common:
        print("no common frame numbers; traces cannot be compared")
        return

    field_counts = {}
    recorder_only_counts = {}
    field_max = {}
    first = None
    divergent_frames = 0
    for frame in common:
        expected = diff_fields(oracle[frame])
        actual = diff_fields(engine[frame])
        source_pk = {int(p["idx"]): p for p in oracle[frame].get("pk", []) if "idx" in p}
        recorder_only = set()
        for index, pickup in source_pk.items():
            # Static pickup amount/radar visibility are placement metadata from the
            # recorder; the engine represents these through its loaded pickup table.
            # They are not dynamic runtime state and have no serialized engine peer.
            if "respawn_units" in pickup and int(pickup.get("lifetime_frames", 0)) == 0:
                recorder_only.update(f"pk[{index}].{field}" for field in ("amount", "radar_hidden"))
        for path in tuple(actual):
            if not path.startswith("pk["):
                continue
            close = path.find("]")
            field = path[close + 2:]
            index = int(path[3:close])
            if field in ("amount", "radar_hidden") and field not in source_pk.get(index, {}):
                recorder_only.add(path)
            elif field.startswith("visit_until[") and "visit_until" not in source_pk.get(index, {}):
                recorder_only.add(path)
        for path in tuple(actual):
            if not path.startswith("pl["):
                continue
            close = path.find("]")
            field = path[close + 2:]
            if (field in ("active_goal", "goal_type", "goal_kind", "goal_target", "weapon_anim_state")
                    and path not in expected):
                recorder_only.add(path)
        if "timer_frame" in actual and "timer_frame" not in expected:
            recorder_only.add("timer_frame")
        for path in recorder_only:
            if path in expected or path in actual:
                recorder_only_counts[path] = recorder_only_counts.get(path, 0) + 1
            expected.pop(path, None)
            actual.pop(path, None)
        mismatches = []
        for path in sorted(expected.keys() | actual.keys()):
            if path not in expected:
                a, b, delta = "<missing>", actual[path], None
                equal = False
            elif path not in actual:
                a, b, delta = expected[path], "<missing>", None
                equal = False
            else:
                a, b = expected[path], actual[path]
                delta, equal = residual(a, b, tolerance)
            if isinstance(delta, (int, float)):
                field_max[path] = max(field_max.get(path, 0.0), delta)
            if equal:
                continue
            field_counts[path] = field_counts.get(path, 0) + 1
            mismatches.append((path, a, b, delta))
            if first is None:
                first = (frame, path, a, b, delta)
        if mismatches:
            divergent_frames += 1
            if verbose:
                print(f"frame {frame}: {len(mismatches)} divergent fields")
                for path, a, b, delta in mismatches:
                    print(f"  {path}: oracle={a!r} engine={b!r} residual={delta!r}")
            else:
                path, a, b, delta = mismatches[0]
                print(f"frame {frame}: {len(mismatches)} divergent fields; first "
                      f"{path} oracle={a!r} engine={b!r} residual={delta!r}")

    print(f"divergent frames: {divergent_frames}/{len(common)}; tolerance {tolerance:g}")
    for path in sorted(field_counts, key=lambda key: (-field_counts[key], key)):
        maximum = field_max.get(path)
        residual_text = f", max residual {maximum:g}" if maximum is not None else ""
        print(f"  {path}: {field_counts[path]} frames{residual_text}")
    if recorder_only_counts:
        print("recorder-only fields (not compared):")
        for path in sorted(recorder_only_counts, key=lambda key: (-recorder_only_counts[key], key)):
            print(f"  {path}: {recorder_only_counts[path]} frames")
    if first is None:
        print("first divergence: none")
    else:
        frame, path, a, b, delta = first
        print(f"first divergence: frame {frame}, {path}: oracle={a!r} engine={b!r} "
              f"residual={delta!r}")
    diff_rng_events(oracle, engine, common)



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
    p.add_argument("--tolerance", type=float, default=0.001,
                   help="absolute tolerance for floating-point fields (default: 0.001)")
    p.add_argument("--verbose", action="store_true",
                   help="print all per-field differences on every common frame")
    args = ap.parse_args()
    if args.cmd == "summary":
        summary(load(args.rec))
    elif args.cmd == "determinism":
        determinism(load(args.a), load(args.b))
    elif args.cmd == "diff":
        diff(args.oracle, args.engine, args.tolerance, args.verbose)


if __name__ == "__main__":
    sys.exit(main())
