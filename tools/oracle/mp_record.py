"""MP match recorder: per-logic-frame PINE batch reads of full match state to JSONL.

Usage: python3 mp_record.py out.jsonl [--load-slot N] [--frames N] [--script s.txt]
         [--seedable] [--rng-calls] [--weapon-anim-raw] [--checkpoint-dir DIR]
         [--checkpoint-every N] [--checkpoint-slot N] [--full-every 30] [--timeout S]

Superset of tools/oracle/trace.py (single-player movement columns kept where
they overlap: frame/pos/yaw/rate/pad/act/flg) plus, per frame: RNG words,
MPSettings slice, MPGame slots + globals, per-participant pos/yaw/pitch/state/
health/armour/weapon/aim/foot, per-bot drone health + BOT_vars goals/caches/
clips/reserves, pickups, switch channels, and live type-5 projectile objects
with their BU_tag payloads; objective ext blobs at `--full-every` intervals or
every accepted frame in `--seedable` mode.

Speed design: the static `Game_Run` return hook copies each requested source
range into a bounded EE ring after the logic update. PINE drains published
snapshots; pointer-dependent ranges refresh on resync frames. Dynamic-list
walks remain incremental and are retried after a torn or invalid link.
"<frame offset> <vpad command...>".
"""

import argparse
import atexit
import json
import os
import pathlib
import shutil
import socket
import struct
import sys
import time

import mp_addrs as A
import mp_frame_trace as F
import mp_rng_trace as R
from pine import Pine, WRITE32


def vpad(*words):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "nf-vpad.sock"))
    s.sendall((" ".join(map(str, words)) + "\n").encode())
    reply = s.recv(256)
    s.close()
    return reply


def load_script(path):
    steps = []
    for line in open(path):
        line = line.split("#")[0].split()
        if line:
            steps.append((int(line[0]), line[1:]))
    return sorted(steps, key=lambda s: s[0])

def save_checkpoint(pine, output_dir, slot, requested_frame, sample_frame):
    config_home = pathlib.Path(os.environ.get(
        "XDG_CONFIG_HOME", str(pathlib.Path.home() / ".config")))
    state_dir = config_home / "PCSX2" / "sstates"
    state_pattern = f"{pine.game_id()}*.{slot}.p2s"
    states = list(state_dir.glob(state_pattern))
    if len(states) > 1:
        raise RuntimeError(f"expected at most one PCSX2 savestate for slot {slot}, found {len(states)}")
    state_path = states[0] if states else None
    previous = state_path.stat() if state_path else None

    boundary_deadline = time.monotonic() + 10.0
    while time.monotonic() < boundary_deadline:
        counters = pine.read_ranges([
            (A.GS_DONE, 4), (A.GS_FRAME_START, 4),
            (A.GS_DONE, 4), (A.GS_FRAME_START, 4),
        ])
        done_before, frame_before, done_check, frame_check = (
            struct.unpack("<I", raw)[0] for raw in counters)
        if done_before == done_check and frame_before == frame_check:
            break
        time.sleep(0.002)
    else:
        raise RuntimeError(f"could not reach a coherent logic-frame boundary near frame {sample_frame}")

    pine.save_state(slot)
    if state_path is None:
        states = list(state_dir.glob(state_pattern))
        if len(states) != 1:
            raise RuntimeError(f"expected one PCSX2 savestate for slot {slot}, found {len(states)}")
        state_path = states[0]
    deadline = time.monotonic() + 10.0
    stable = 0
    last_stat = None
    while time.monotonic() < deadline:
        try:
            current = state_path.stat()
        except FileNotFoundError:
            time.sleep(0.05)
            continue
        signature = (current.st_size, current.st_mtime_ns)
        changed = previous is None or signature != (previous.st_size, previous.st_mtime_ns)
        stable = stable + 1 if changed and signature == last_stat else 0
        if stable >= 2:
            break
        last_stat = signature
        time.sleep(0.05)
    else:
        raise RuntimeError(f"PCSX2 did not finish writing savestate slot {slot}")

    after = pine.read_ranges([(A.GS_DONE, 4), (A.GS_FRAME_START, 4)])
    done_after, frame_after = (struct.unpack("<I", raw)[0] for raw in after)
    os.makedirs(output_dir, exist_ok=True)
    name = f"frame-{sample_frame:08d}.p2s"
    checkpoint = pathlib.Path(output_dir) / name
    shutil.copyfile(state_path, checkpoint)
    metadata = {
        "protocol": "mp-checkpoint-v1",
        "requested_frame": requested_frame,
        "sample_frame": sample_frame,
        "frame_before_save": frame_before,
        "done_before_save": done_before,
        "frame_after_save": frame_after,
        "done_after_save": done_after,
        "pcsx2_slot": slot,
        "savestate": name,
        "size_bytes": checkpoint.stat().st_size,
    }
    with open(checkpoint.with_suffix(".json"), "w", encoding="utf-8") as stream:
        json.dump(metadata, stream, separators=(",", ":"))
        stream.write("\n")
    return metadata


def valid_checkpoint_args(ap, args):
    if args.checkpoint_every <= 0:
        ap.error("--checkpoint-every must be positive")
    if not 0 <= args.checkpoint_slot <= 255:
        ap.error("--checkpoint-slot must be in 0..255")
    if args.checkpoint_first is not None and args.checkpoint_first < 0:
        ap.error("--checkpoint-first must be non-negative")
    if args.checkpoint_first is not None and not args.checkpoint_dir:
        ap.error("--checkpoint-first requires --checkpoint-dir")
    if args.checkpoint_dir:
        args.checkpoint_dir = os.path.abspath(os.path.expanduser(args.checkpoint_dir))
 
def freeze_object_pose(pine, obj, pose):
    values = [*pose[0], pose[1]]
    body = b"".join(
        struct.pack("<BI", WRITE32, obj + addr) + struct.pack("<f", value)
        for addr, value in zip((A.OBJ_POS, A.OBJ_POS + 4, A.OBJ_POS + 8, A.OBJ_YAW), values)
    )
    pine._transact(body)


def read_ranges_batched(pine, ranges):
    """Keep large oracle snapshots under PINE's 4000-op transaction limit."""
    chunks, batch, words = [], [], 0
    for addr, size in ranges:
        count = ((addr & 7) + size + 7) // 8
        if count > 4000:
            raise ValueError(f"PINE range too large: {addr:#x}+{size:#x}")
        if batch and words + count > 4000:
            chunks.extend(pine.read_ranges(batch))
            batch, words = [], 0
        batch.append((addr, size))
        words += count
    if batch:
        chunks.extend(pine.read_ranges(batch))
    return chunks


FULL_BLOBS = [
    ("flags", A.FLAGS, 0x120), ("bases", A.BASES, 0x120),
    ("uplinks", A.UPLINKS, 0x480), ("demo", A.DEMOLITION, 0x90),
    ("prot", A.PROTECTION, 0x90), ("ge", A.GOLDENEYE, 0x240),
    ("bp", A.BLUEPRINT, 0x90), ("esp", A.ESPONAGE_BASE, 0x120),
    ("hill", A.HILL, 0x90),
]

MAX_DYNAMIC_OBJECTS = 4096
EE_RAM_START = 0x00100000
EE_RAM_END = 0x02000000
AI_ROUTE_OFFSET = 0x860
AI_ROUTE_RAW_SIZE = 0x100
AI_PATH_POINTER_OFFSET = 0x954
AI_PATH_RAW_SIZE = 0x200
AI_PATH_CHILD_POINTER_OFFSETS = (0x34, 0x3C, 0x40)
AI_PATH_CHILD_RAW_SIZE = 0x200
AI_ROUTE_NODE_POINTER_OFFSET = 0x950



def valid_ee_pointer(addr, size):
    return (EE_RAM_START <= addr and addr + size <= EE_RAM_END and not addr & 3)

def objective_addresses(blobs):
    addresses = set()
    for raw in blobs:
        for offset in range(0, len(raw), 0x90):
            back_pointer = struct.unpack_from("<I", raw, offset + 0x84)[0]
            if back_pointer >= 0xE0:
                address = back_pointer - 0xE0
                if valid_ee_pointer(address, 0x138):
                    addresses.add(address)
    return sorted(addresses)


def ai_path_child_pointers(raw):
    return {
        offset: struct.unpack_from("<I", raw, offset)[0]
        for offset in AI_PATH_CHILD_POINTER_OFFSETS
    }


def refresh_ai_path_slot(pine, cache, k):
    drone = cache["drone"].get(k)
    if not drone:
        cache["ai_paths"].pop(k, None)
        return
    drone_raw = pine.read_block(drone, A.DRONE_RAW_SIZE)
    address = struct.unpack_from("<I", drone_raw, AI_PATH_POINTER_OFFSET)[0]
    path = {"address": address, "children": {}, "valid": False}
    if valid_ee_pointer(address, AI_PATH_RAW_SIZE):
        path_raw = pine.read_block(address, AI_PATH_RAW_SIZE)
        path["children"] = ai_path_child_pointers(path_raw)
        path["valid"] = True
    cache["ai_paths"][k] = path


def refresh_ai_paths(pine, cache):
    for k in range(4):
        refresh_ai_path_slot(pine, cache, k)
ANIM_OWNER_OFFSET = 0xDC
ANIM_SUBOBJECT_OFFSET = 0x70
ANIM_LAYER_HEAD_OFFSET = 0x2C
ANIM_LAYER_RAW_SIZE = 0xC0
ANIM_SEQ_RAW_SIZE = 0xB0
ANIM_MAX_LAYERS = 64


def refresh_animation_slot(pine, cache, k, obj=None, kind="bot"):
    """Resolve the sAnimObject and body-layer chain behind an obj_tag owner."""
    bucket = cache["bot_anim"] if kind == "bot" else cache["human_anim"]
    if obj is None:
        obj = cache["objs"][4 + k]
    if not obj:
        bucket.pop(k, None)
        return
    obj_raw = pine.read_block(obj, 0x100)
    owner = struct.unpack_from("<I", obj_raw, ANIM_OWNER_OFFSET)[0]
    if not valid_ee_pointer(owner, 0x120):
        bucket.pop(k, None)
        return
    anim = owner + ANIM_SUBOBJECT_OFFSET
    if not valid_ee_pointer(anim + 0xCC, 4):
        bucket.pop(k, None)
        return
    head = pine.read32(anim + ANIM_LAYER_HEAD_OFFSET)
    layers = []
    walked = set()
    cursor = head
    chain_complete = True
    while cursor:
        if (cursor in walked or len(layers) >= ANIM_MAX_LAYERS
                or not valid_ee_pointer(cursor, ANIM_LAYER_RAW_SIZE)):
            chain_complete = False
            layers = []
            break
        walked.add(cursor)
        raw = pine.read_block(cursor, ANIM_LAYER_RAW_SIZE)
        seq_primary = struct.unpack_from("<I", raw, 0x50)[0]
        layers.append({
            "address": cursor,
            "seq_primary": seq_primary if valid_ee_pointer(seq_primary, ANIM_SEQ_RAW_SIZE) else 0,
        })
        cursor = struct.unpack_from("<I", raw, 0x48)[0]
    bucket[k] = {
        "obj": obj, "owner": owner, "anim": anim, "head": head,
        "layers": layers, "chain_complete": chain_complete,
    }


def refresh_animations(pine, cache):
    for k in range(4):
        refresh_animation_slot(pine, cache, k)

def add_animation_ranges(ranges, tags, k, state, prefix):
    anim = state["anim"]
    tags += [(f"{prefix}_list_head", k), (f"{prefix}_root_height", k),
             (f"{prefix}_distance_step", k), (f"{prefix}_distance_accum", k),
             (f"{prefix}_foot_height", k)]
    ranges += [(anim + 0x2C, 4), (anim + 0x5C, 4),
               (anim + 0x64, 4), (anim + 0x6C, 4), (anim + 0xCC, 4)]
    for index, layer in enumerate(state["layers"]):
        tags.append((f"{prefix}_layer_raw", (k, index)))
        ranges.append((layer["address"], ANIM_LAYER_RAW_SIZE))
        if layer["seq_primary"]:
            tags.append((f"{prefix}_seq_raw", (k, index)))
            ranges.append((layer["seq_primary"], ANIM_SEQ_RAW_SIZE))


def anim_layer_rows(bytag, k, descriptor, prefix="anim"):
    layers = []
    for index, layer in enumerate(descriptor["layers"]):
        raw = bytag[(f"{prefix}_layer_raw", (k, index))]
        flags = struct.unpack_from("<I", raw, 0x78)[0]
        drive = raw[0xB4]
        blend_time, blend_duration = struct.unpack_from("<2f", raw, 0xA8)
        blend_progress = struct.unpack_from("<f", raw, 0x9C)[0]
        direction = struct.unpack_from("b", raw, 0xB6)[0]
        fade_direction = "in" if direction > 0 else "out" if direction < 0 else "steady"
        sequence = None
        seq_addr = layer["seq_primary"]
        seq_raw = bytag.get((f"{prefix}_seq_raw", (k, index)))
        if seq_addr and seq_raw is not None:
            seq_flags = struct.unpack_from("<I", seq_raw, 0x98)[0]
            sample_ptr = struct.unpack_from("<I", seq_raw, 0x84)[0]
            sequence = {
                "ptr": seq_addr,
                "raw": seq_raw.hex(),
                "previous_root": list(struct.unpack_from("<4f", seq_raw, 0)),
                "last_root_delta": list(struct.unpack_from("<4f", seq_raw, 0x10)),
                "sampled_frame": struct.unpack_from("<h", seq_raw, 0xA0)[0],
                "fractional_frame": struct.unpack_from("<f", seq_raw, 0x9C)[0],
                "flags": seq_flags,
                "sample_ptr": sample_ptr,
                "have_root": bool(sample_ptr and not seq_flags & 0x20000000),
            }
        frame = struct.unpack_from("<f", raw, 0x90)[0]
        layers.append({
            "raw": raw.hex(),
            "ptr": layer["address"],
            "script_id": struct.unpack_from("<I", raw, 0x74)[0],
            "flags": flags,
            "drive_type": drive,
            "type": {"time": drive == 0, "distance": drive == 1, "phase": drive == 2},
            "frame": frame,
            "phase": frame if drive == 2 else None,
            "previous_frame": struct.unpack_from("<f", raw, 0x94)[0],
            "speed": struct.unpack_from("<f", raw, 0x98)[0],
            "blend_time": blend_time,
            "blend_duration": blend_duration,
            "fade_progress": blend_progress,
            "fade_direction": fade_direction,
            "effective_weight": blend_progress,
            "primary_id": struct.unpack_from("<I", raw, 0x80)[0],
            "phase_partner_ptr": struct.unpack_from("<I", raw, 0x4C)[0],
            "pair_weight": struct.unpack_from("<f", raw, 0x88)[0],
            "aux_weight_8c": struct.unpack_from("<f", raw, 0x8C)[0],
            "strafe": bool(flags & 0x4000),
            "fresh": bool(flags & 0x20000000),
            "deleting": bool(flags & 0x10000000),
            "stop_state": raw[0xB5],
            "resource_flags_b2": raw[0xB2],
            "resource_flags_b3": raw[0xB3],
            "state_b6": raw[0xB6],
            "sequence": sequence,
        })
    return layers

def animation_record(bytag, k, state, prefix="anim"):
    return {
        "owner_ptr": state["owner"],
        "s_anim_object_ptr": state["anim"],
        "layer_head_ptr": state["head"],
        "layer_order": "oldest_to_newest",
        "layers_complete": state["chain_complete"],
        "root_height": struct.unpack(
            "<f", bytag[(f"{prefix}_root_height", k)])[0],
        "distance_step": struct.unpack(
            "<f", bytag[(f"{prefix}_distance_step", k)])[0],
        "distance_accumulator": struct.unpack(
            "<f", bytag[(f"{prefix}_distance_accum", k)])[0],
        "foot_height": struct.unpack(
            "<f", bytag[(f"{prefix}_foot_height", k)])[0],
        "layers": anim_layer_rows(bytag, k, state, prefix),
    }








def refresh_projectile_cache(pine, cache, head, initial=False):
    """Walk only newly prepended DynamicObjList nodes; return new type-5 objects."""
    if initial or not cache["dynamic_scan_ok"]:
        # Retry from the current head after a torn/bogus link rather than
        # permanently poisoning every later projectile sample.
        cache["dynamic_members"].clear()
        cache["projectiles"].clear()
        cache["dynamic_scan_ok"] = True
    cursor = head
    walked = set()
    added = []
    while cursor and cursor not in cache["dynamic_members"]:
        if cursor in walked or len(walked) >= MAX_DYNAMIC_OBJECTS:
            cache["dynamic_scan_ok"] = False
            break
        if cursor < 0x00100000 or cursor >= 0x02000000 or cursor & 0xF:
            cache["dynamic_scan_ok"] = False
            break
        walked.add(cursor)
        raw = pine.read_block(cursor, 0x100)
        cache["dynamic_members"].add(cursor)
        if raw[A.OBJ_TYPE] == 5:
            data = struct.unpack_from("<I", raw, A.OBJ_CUSTOM_DATA)[0]
            if data:
                cache["projectiles"][cursor] = data
                added.append(cursor)
            else:
                cache["dynamic_scan_ok"] = False
        cursor = struct.unpack_from("<I", raw, A.OBJ_LIST_NEXT)[0]
    cache["dynamic_head"] = head
    return added
def rehydrate_trace_hooks(pine):
    """Restore both phase and RNG trampolines atomically after LOADSTATE."""
    ops = [(WRITE32, F.CONFIG_BASE, "<I", 0)]
    ops.extend((WRITE32, F.CODE_BASE + 4 * i, "<I", word)
               for i, word in enumerate(F.frame_hook_words()))
    ops.extend(((WRITE32, F.ENTRY, "<I", F.jump_word()),
                (WRITE32, F.ENTRY + 4, "<I", F.ENTRY_ORIGINAL[1])))
    for index, (_, entry, kind, result_class, first_words) in enumerate(R.FUNCTIONS):
        stub = R.CODE_BASE + index * 0x100
        _, words = R._trampoline(entry, kind, result_class, first_words, stub)
        ops.extend((WRITE32, stub + 4 * i, "<I", word)
                   for i, word in enumerate(words))
        ops.extend(((WRITE32, entry, "<I", R._j(0x02, stub)),
                    (WRITE32, entry + 4, "<I", 0)))
    R._write_ops(pine, ops)




def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--frames", type=int, default=9000)
    ap.add_argument("--load-slot", type=int)
    ap.add_argument("--script")
    ap.add_argument("--full-every", type=int, default=30)
    ap.add_argument("--seedable", action="store_true",
                    help="emit v5 seed fields, including resolved bot animation layers each frame")
    ap.add_argument("--weapon-anim-raw", action="store_true",
                    help="capture coherent 0x100-byte human BLData+0x7e8 objects (requires --seedable)")
    ap.add_argument("--rng-calls", action="store_true",
                    help="temporarily instrument four ACTION.ELF RNG entries and record caller/result events")
    ap.add_argument("--rng-calls-preinstalled", action="store_true",
                    help="attach to RNG hooks installed from PCSX2 pnach at boot")
    ap.add_argument("--freeze-bot", type=int, choices=range(4, 8),
                    help="hold this MP bot's initial position and yaw while recording")
    ap.add_argument("--face-bot", type=int, choices=range(4, 8),
                    help="place the human 8 units behind this frozen bot, facing it")
    ap.add_argument("--timeout", type=float, default=1200.0)
    ap.add_argument("--checkpoint-dir", help="copy periodic PINE savestates into this frame-keyed directory")
    ap.add_argument("--checkpoint-every", type=int, default=60,
                    help="logic-frame interval between savestates (default: 60)")
    ap.add_argument("--checkpoint-slot", type=int, default=250,
                    help="temporary PCSX2 savestate slot copied into checkpoint-dir")
    ap.add_argument("--checkpoint-first", type=int,
                    help="absolute GameState+0x3c frame for the first checkpoint")
    ap.add_argument("--pine-slot", type=int, default=28011,
                    help="PINE slot for the PCSX2 instance (default: 28011)")
    args = ap.parse_args()
    if args.rng_calls and args.rng_calls_preinstalled:
        ap.error("choose only one RNG hook installation mode")
    valid_checkpoint_args(ap, args)
    if args.face_bot is not None and args.freeze_bot != args.face_bot:
        ap.error("--face-bot must name the same slot as --freeze-bot")
    if args.weapon_anim_raw and not args.seedable:
        ap.error("--weapon-anim-raw requires --seedable")

    pine = Pine(args.pine_slot)
    steps = load_script(args.script) if args.script else []
    vpad("release")
    freeze_pose = None
    freeze_obj = 0
    face_pose = None
    face_obj = 0
    rng_trace = None
    if args.load_slot is not None:
        before = pine.read32(A.GS_FRAME_START)
        pine.load_state(args.load_slot)
        start = time.monotonic()
        while abs(pine.read32(A.GS_FRAME_START) - before) <= 20 and time.monotonic() - start < 4.0:
            time.sleep(0.001)
        rehydrate_trace_hooks(pine)
    if args.rng_calls_preinstalled:
        rng_trace = R.attach(pine)
    if args.freeze_bot is not None:
        freeze_obj = pine.read32(
            A.MPGAME + args.freeze_bot * A.MP_SLOT_STRIDE + A.MPG_OBJ)
        if not freeze_obj or pine.read_block(freeze_obj + A.OBJ_TYPE, 1)[0] != 2:
            raise RuntimeError(f"slot {args.freeze_bot} does not contain a live bot")
        freeze_pose = (
            struct.unpack("<3f", pine.read_block(freeze_obj + A.OBJ_POS, 12)),
            struct.unpack("<f", pine.read_block(freeze_obj + A.OBJ_YAW, 4))[0],
        )
        freeze_object_pose(pine, freeze_obj, freeze_pose)
        if args.face_bot is not None:
            face_obj = pine.read32(A.MPGAME + A.MPG_OBJ)
            if not face_obj or pine.read_block(face_obj + A.OBJ_TYPE, 1)[0] != 3:
                raise RuntimeError("slot 0 does not contain a live human player")
            face_pose = ((freeze_pose[0][0], freeze_pose[0][1],
                          freeze_pose[0][2] - 8.0), 0.0)
            freeze_object_pose(pine, face_obj, face_pose)
            bl = pine.read32(face_obj + A.OBJ_BL)
            if bl:
                pine.write(WRITE32, bl + A.BL_PITCH, 0)
    n_humans = struct.unpack("<I", pine.read_block(A.MPSETTINGS + A.MPS_HUMANS, 4))[0]
    if args.rng_calls:
        rng_trace = R.install(pine)
        atexit.register(R.uninstall, pine, rng_trace)
    out = open(args.out, "w", buffering=1)
    cache = {"objs": [0] * 8, "bl": {}, "cb": {}, "drone": {}, "pinfo": {}, "pkcount": -1,
             "ports": [], "dynamic_head": 0, "dynamic_members": set(),
             "dynamic_scan_ok": True, "projectiles": {}, "goal_refs": {}, "ai_paths": {},
             "objective_ptrs": [], "route_nodes": {}, "weapon_anim_targets": {},
             "bot_anim": {}, "human_anim": {}}
    def resolve(pine):
        """(Re)resolve all pointer caches with unguarded reads; caller retries."""
        settings = pine.read_block(A.PLAYER_SETTING, A.PLAYER_SETTING_STRIDE * 4)
        cache["ports"] = [settings[i * A.PLAYER_SETTING_STRIDE + 0x156] for i in range(4)]
        mg = pine.read_block(A.MPGAME, 0x1D0)
        objs = [struct.unpack_from("<I", mg, s * A.MP_SLOT_STRIDE + A.MPG_OBJ)[0] for s in range(8)]
        cache["objs"] = objs
        for s in range(n_humans):
            if objs[s]:
                od = pine.read_block(objs[s], 0x100)
                cache["bl"][s] = struct.unpack_from("<I", od, A.OBJ_BL)[0]
                cache["cb"][s] = struct.unpack_from("<I", od, A.OBJ_COLL)[0]
        cache["weapon_anim_targets"] = {}
        cache["human_anim"] = {}
        if args.seedable:
            for slot, bl in cache["bl"].items():
                if not valid_ee_pointer(bl + 0x7E8, 4):
                    continue
                target = pine.read32(bl + 0x7E8)
                if valid_ee_pointer(target, 0x100) and not target & 0xF:
                    cache["weapon_anim_targets"][slot] = target
        for k in range(4):
            if objs[4 + k]:
                cache["drone"][k] = struct.unpack("<I", pine.read_block(
                    A.BOT_VARS + k * A.BOT_VARS_STRIDE + A.BOT_DRONE + 4, 4))[0]
        cache["route_nodes"] = {}
        if args.seedable:
            for k, drone in cache["drone"].items():
                if not valid_ee_pointer(drone, A.DRONE_RAW_SIZE):
                    continue
                raw = pine.read_block(drone, A.DRONE_RAW_SIZE)
                route = raw[AI_ROUTE_OFFSET:AI_ROUTE_OFFSET + AI_ROUTE_RAW_SIZE]
                address = struct.unpack_from("<I", raw, AI_ROUTE_NODE_POINTER_OFFSET)[0]
                count = struct.unpack_from("<H", route, 0x82)[0]
                size = count * 2
                if size == 0 or valid_ee_pointer(address, size):
                    cache["route_nodes"][k] = (address, count, size)
        mpk = pine.read_block(A.MPPICKUPS, 64 * A.MPPICKUP_STRIDE)
        pinfo = {}
        for i in range(64):
            obj = struct.unpack_from("<I", mpk, i * A.MPPICKUP_STRIDE)[0]
            if obj:
                pos = struct.unpack_from("<4f", mpk, i * A.MPPICKUP_STRIDE + A.MPPICKUP_POS)
                info = struct.unpack("<I", pine.read_block(obj + A.PICKUPINFO_OFF, 4))[0]
                pinfo[obj] = (info, [round(v, 2) for v in pos[:3]], i)
        cache["pinfo"] = pinfo
        if args.seedable:
            blobs = read_ranges_batched(
                pine, [(addr, size) for _, addr, size in FULL_BLOBS])
            cache["objective_ptrs"] = objective_addresses(blobs)
        goal_refs = {}
        for k in cache["drone"]:
            bv = A.BOT_VARS + k * A.BOT_VARS_STRIDE
            raw = pine.read_block(bv, 0x90)
            goal_refs[k] = [
                struct.unpack_from("<I", raw, goal * 0x50 + A.GOAL_TARGET)[0]
                for goal in range(2)
            ]
        cache["goal_refs"] = goal_refs
        if args.seedable:
            refresh_ai_paths(pine, cache)
            refresh_animations(pine, cache)
            if args.weapon_anim_raw:
                for slot in range(n_humans):
                    refresh_animation_slot(
                        pine, cache, slot,
                        obj=cache["weapon_anim_targets"].get(slot, 0), kind="human")
        cache["pkcount"] = struct.unpack("<H", pine.read_block(
            A.MPSETTINGS + A.MPS_PICKUP_COUNT, 2))[0]
        return objs

    resolve(pine)
    if args.freeze_bot is not None:
        bot_obj = cache["objs"][args.freeze_bot]
        if not freeze_pose:
            if not bot_obj or pine.read_block(bot_obj + A.OBJ_TYPE, 1)[0] != 2:
                raise RuntimeError(f"slot {args.freeze_bot} does not contain a live bot")
            freeze_obj = bot_obj
            freeze_pose = (
                struct.unpack("<3f", pine.read_block(bot_obj + A.OBJ_POS, 12)),
                struct.unpack("<f", pine.read_block(bot_obj + A.OBJ_YAW, 4))[0],
            )
        elif bot_obj != freeze_obj:
            raise RuntimeError(f"bot slot {args.freeze_bot} changed object during recorder setup")
 
    refresh_projectile_cache(
        pine, cache, pine.read32(A.DYNAMIC_OBJ_LIST + A.OBJ_LIST_NEXT), initial=True)
    first = last = None
    frame_ring = None
    frame_schema = None
    last_new = time.monotonic()
    records, missed, resyncs = [], 0, 0
    deadline = time.monotonic() + args.timeout
    stalled_warned = False
    checkpoint_next = None

    while time.monotonic() < deadline:
        if time.monotonic() - last_new > 45.0 and not stalled_warned:
            # No new frame in 45 s: the match is over (states 4/5 freeze the
            # update counters) or a long load is running. Confirm and leave.
            st = pine.read32(A.MPGAME + A.MPG_STATE)
            if st in (4, 5):
                print(f"  ... match over (state {st}), exiting", flush=True)
                break
            print("  ... stalled 45 s, still waiting", flush=True)
            stalled_warned = True
        if freeze_pose is not None:
            freeze_object_pose(pine, cache["objs"][args.freeze_bot], freeze_pose)
        projectile_old_head = cache["dynamic_head"]
        ranges, tags = [], []
        ranges += [(A.GS_DONE, 4), (A.GS_FRAME_START, 4), (A.GS_FRAME, 4)]
        tags += [("done", 0), ("frame", 0), ("timer_frame", 0)]
        ranges += [(A.MPGAME, 0x1D0), (A.MPSETTINGS + A.MPS_MP_ACTIVE, 0x60),
                   (A.RNG_WORDS, 16), (A.SWITCH_FD - 1, 4), (A.FRAME_RATE, 4),
                   (A.FRAME_RATE_INT, 4), (A.MP_ASSASSINATION_TARGET, 4),
                   (A.MP_ASSASSIN, 4), (A.GOLDENEYE_EFFECT, 4),
                   (A.GOLDENEYE_TARGET, 4)]
        tags += [("mpg", 0), ("mps", 0), ("rng", 0), ("sw", 0), ("rate", 0),
                 ("rate_int", 0), ("assassination_target", 0), ("assassin", 0),
                 ("golden_effect", 0), ("golden_target", 0)]
        for s in range(4):
            setting = A.PLAYER_SETTING + s * A.PLAYER_SETTING_STRIDE
            port = cache["ports"][s]
            tags += [("pad", s), ("setting", s)]
            ranges += [(A.TSLOT0 + port * A.TSLOT_STRIDE + 0x120, 0x30),
                       (setting, A.PLAYER_SETTING_STRIDE)]
        if args.seedable:
            tags.append(("mp_roster", 0))
            ranges.append((A.MPSETTINGS, A.MP_SLOT_STRIDE * A.MP_NSLOTS))
            for s in range(n_humans):
                if cache["bl"].get(s):
                    tags.append(("bl_raw", s))
                    ranges.append((cache["bl"][s], A.BL_RAW_SIZE))
                    target = cache["weapon_anim_targets"].get(s)
                    if target:
                        tags.append(("weapon_anim_state", s))
                        ranges.append((target + 0xF4, 2))
                        if args.weapon_anim_raw:
                            tags.append(("weapon_anim_raw", s))
                            ranges.append((target, 0x100))
                if cache["cb"].get(s):
                    tags.append(("cb_raw", s))
                    ranges.append((cache["cb"][s], A.CB_RAW_SIZE))
            for k, d in cache["drone"].items():
                bv = A.BOT_VARS + k * A.BOT_VARS_STRIDE
                tags += [("dr_raw", k), ("bv_raw", k)]
                ranges += [(d, A.DRONE_RAW_SIZE), (bv, A.BOT_VARS_STRIDE)]
                route_node = cache["route_nodes"].get(k)
                if route_node and route_node[2]:
                    tags.append(("route_nodes", k))
                    ranges.append((route_node[0], route_node[2]))
            for k, state in cache["bot_anim"].items():
                add_animation_ranges(ranges, tags, k, state, "anim")
            if args.weapon_anim_raw:
                for slot, state in cache["human_anim"].items():
                    add_animation_ranges(ranges, tags, slot, state, "human_anim")
            for k, state in cache["bot_anim"].items():
                owner = state["owner"]
                if valid_ee_pointer(owner, A.CB_RAW_SIZE):
                    tags.append(("cb_raw", 4 + k))
                    ranges.append((owner, A.CB_RAW_SIZE))
        for s, o in enumerate(cache["objs"]):
            if o:
                tags.append(("obj", s))
                ranges.append((o, 0x100))
        for obj, data in cache["projectiles"].items():
            tags += [("projectile_obj", obj), ("projectile_data", obj)]
            ranges += [(obj, 0x100), (data, A.BULLET_RAW_SIZE)]
        tags.append(("projectile_head0", 0))
        ranges.append((A.DYNAMIC_OBJ_LIST + A.OBJ_LIST_NEXT, 4))
        for s in range(n_humans):
            if s in cache["bl"] and cache["bl"][s]:
                tags.append(("bl", s))
                ranges.append((cache["bl"][s] + A.BL_HEALTH - 4, 0x40))
                tags.append(("feedback", s))
                ranges.append((cache["bl"][s] + A.BL_FADE_COLOUR, A.BL_PAIN_ALPHA - A.BL_FADE_COLOUR + 1))
                tags.append(("fade", s))
                ranges.append((cache["bl"][s] + A.BL_FADE_TOTAL, A.BL_FADE_TIMER - A.BL_FADE_TOTAL + 4))
                tags.append(("autotarget", s))
                ranges.append((cache["bl"][s] + A.BL_AUTOTARGET, 4))
            if s in cache["cb"] and cache["cb"][s]:
                tags.append(("cb", s))
                ranges.append((cache["cb"][s] + 0x90, 0x50))
        for k in range(4):
            d = cache["drone"].get(k)
            if d:
                bv = A.BOT_VARS + k * A.BOT_VARS_STRIDE
                tags.append(("drone", k))
                ranges.append((d + A.DRONE_HEALTH - 4, 0x20))
                tags.append(("dmg", k))
                ranges.append((d + A.DRONE_LASTDMG, 4))
                tags.append(("bv", k))
                ranges.append((bv, 0xA0))
                tags.append(("bo", k))
                ranges.append((bv + A.BOT_OTHER, 0x80))
                tags.append(("bw", k))
                ranges.append((bv + A.BOT_WEAPONS, 0x55 * 0xC))
                tags.append(("br", k))
                ranges.append((bv + A.BOT_RESERVE, 0x80))
                tags.append(("bs", k))
                ranges.append((bv + A.BOT_DISTRACT, 0x4C))
                for target in cache["goal_refs"].get(k, []):
                    offset = target - A.MPPICKUPS
                    if (0 <= offset < 64 * A.MPPICKUP_STRIDE
                            and offset % A.MPPICKUP_STRIDE == 0):
                        tags.append(("goal_target_raw", target))
                        ranges.append((target, A.MPPICKUP_STRIDE))
                    elif (target not in cache["objs"]
                          and target not in cache["objective_ptrs"]
                          and valid_ee_pointer(target, 4)):
                        tags.append(("goal_target_link", target))
                        ranges.append((target, 4))
        if args.seedable:
            for k, path in cache["ai_paths"].items():
                if not path["valid"]:
                    continue
                tags.append(("ai_path_raw", k))
                ranges.append((path["address"], AI_PATH_RAW_SIZE))
                for offset, pointer in path["children"].items():
                    if valid_ee_pointer(pointer, AI_PATH_CHILD_RAW_SIZE):
                        tags.append(("ai_path_child_raw", (k, offset)))
                        ranges.append((pointer, AI_PATH_CHILD_RAW_SIZE))
        for obj, (info, pos, pickup_idx) in cache["pinfo"].items():
            tags += [("pkstamp", obj), ("pkvisit", obj)]
            ranges += [(obj + A.OBJ_STAMP, 4),
                       (A.MPPICKUPS + pickup_idx * A.MPPICKUP_STRIDE + A.MPPICKUP_VISIT, 16)]
            if info:
                tags.append(("pi", obj))
                ranges.append((info + A.PI_STATE, 0x18))
            tags.append(("pkflags", obj))
            ranges.append((obj + A.OBJ_FLAGS, 1))
        if args.seedable:
            for name, addr, size in FULL_BLOBS:
                tags.append(("objx", name))
                ranges.append((addr, size))
            for obj in cache["objective_ptrs"]:
                tags.append(("mp_object_raw", obj))
                ranges.append((obj, 0x138))
        tags.append(("projectile_head1", 0))
        ranges.append((A.DYNAMIC_OBJ_LIST + A.OBJ_LIST_NEXT, 4))
        # tentatively assume this frame number for the full blobs
        ranges += [(A.GS_DONE, 4), (A.GS_FRAME_START, 4), (A.GS_FRAME, 4)]
        tags += [("done1", 0), ("frame1", 0), ("timer_frame1", 0)]

        if any(addr < 0x00100000 or addr + size > 0x02000000
               for addr, size in ranges):
            # A cached dynamic pointer went stale between resolution and batch
            # assembly; do not send a malformed EE address to PINE.
            resyncs += 1
            time.sleep(0.005)
            resolve(pine)
            refresh_projectile_cache(
                pine, cache, pine.read32(A.DYNAMIC_OBJ_LIST + A.OBJ_LIST_NEXT))
            continue
        schema = tuple(ranges)
        if (schema != frame_schema
                or (first is None and pine.read32(F.CONFIG_BASE) != len(ranges))):
            frame_ring = F.configure_ranges(pine, ranges)
            frame_schema = schema
            print(f"  ... frame ring: {frame_ring['payload_size']} bytes, "
                  f"{frame_ring['capacity']} slots", flush=True)
        snapshot = F.read_next(pine, frame_ring,
                               timeout=0.01 if first is None else 1.0)
        if snapshot is None:
            continue
        frame_meta, chunks = snapshot
        bytag = {}
        for (kind, s), ch in zip(tags, chunks):
            bytag.setdefault((kind, s), ch)
        if (struct.unpack("<I", bytag[("done", 0)])[0] != frame_meta["done"]
                or struct.unpack("<I", bytag[("frame", 0)])[0] != frame_meta["frame"]
                or struct.unpack("<I", bytag[("timer_frame", 0)])[0]
                != frame_meta["timer_frame"]):
            raise RuntimeError("frame snapshot metadata does not match captured ranges")
        done0 = struct.unpack("<I", bytag[("done", 0)])[0]
        frame0 = struct.unpack("<I", bytag[("frame", 0)])[0]
        timer_frame0 = struct.unpack("<I", bytag[("timer_frame", 0)])[0]
        done1 = struct.unpack("<I", bytag[("done1", 0)])[0]
        frame1 = struct.unpack("<I", bytag[("frame1", 0)])[0]
        timer_frame1 = struct.unpack("<I", bytag[("timer_frame1", 0)])[0]
        if frame0 != frame1 or timer_frame0 != timer_frame1 or done0 != done1:
            continue
        objective_ptrs = []
        if args.seedable:
            objective_ptrs = objective_addresses(
                [bytag[("objx", name)] for name, _, _ in FULL_BLOBS])
            if objective_ptrs != cache["objective_ptrs"]:
                cache["objective_ptrs"] = objective_ptrs
                continue
        if args.seedable:
            pointer_changed = False
            for slot in range(n_humans):
                bl_raw = bytag.get(("bl_raw", slot))
                if bl_raw is None:
                    continue
                target = struct.unpack_from("<I", bl_raw, 0x7E8)[0]
                if not (valid_ee_pointer(target, 0x100) and not target & 0xF):
                    target = 0
                if target != cache["weapon_anim_targets"].get(slot, 0):
                    if target:
                        cache["weapon_anim_targets"][slot] = target
                    else:
                        cache["weapon_anim_targets"].pop(slot, None)
                    if args.weapon_anim_raw:
                        refresh_animation_slot(
                            pine, cache, slot, obj=target, kind="human")
                    pointer_changed = True
            for k in range(4):
                state = cache["bot_anim"].get(k)
                obj_raw = bytag.get(("obj", 4 + k))
                if obj_raw is None:
                    continue
                owner = struct.unpack_from("<I", obj_raw, ANIM_OWNER_OFFSET)[0]
                if state is None:
                    if valid_ee_pointer(owner, 0x120):
                        refresh_animation_slot(pine, cache, k)
                        pointer_changed = True
                    continue
                head = struct.unpack("<I", bytag[("anim_list_head", k)])[0]
                if (owner != state["owner"] or head != state["head"]
                        or state["obj"] != cache["objs"][4 + k]):
                    refresh_animation_slot(pine, cache, k)
                    pointer_changed = True
                    continue
                for index, layer in enumerate(state["layers"]):
                    raw = bytag[("anim_layer_raw", (k, index))]
                    next_ptr = struct.unpack_from("<I", raw, 0x48)[0]
                    seq_ptr = struct.unpack_from("<I", raw, 0x50)[0]
                    seq_ptr = seq_ptr if valid_ee_pointer(seq_ptr, ANIM_SEQ_RAW_SIZE) else 0
                    expected_next = (
                        state["layers"][index + 1]["address"]
                        if index + 1 < len(state["layers"]) else 0)
                    if next_ptr != expected_next or seq_ptr != layer["seq_primary"]:
                        refresh_animation_slot(pine, cache, k)
                        pointer_changed = True
                        break
            if args.weapon_anim_raw:
                for slot in range(n_humans):
                    state = cache["human_anim"].get(slot)
                    target = cache["weapon_anim_targets"].get(slot, 0)
                    raw_obj = bytag.get(("weapon_anim_raw", slot))
                    if raw_obj is None:
                        continue
                    owner = struct.unpack_from("<I", raw_obj, ANIM_OWNER_OFFSET)[0]
                    if state is None:
                        if valid_ee_pointer(owner, 0x120):
                            refresh_animation_slot(
                                pine, cache, slot, obj=target, kind="human")
                            pointer_changed = True
                        continue
                    head = struct.unpack(
                        "<I", bytag[("human_anim_list_head", slot)])[0]
                    if (owner != state["owner"] or head != state["head"]
                            or state["obj"] != target):
                        refresh_animation_slot(
                            pine, cache, slot, obj=target, kind="human")
                        pointer_changed = True
                        continue
                    for index, layer in enumerate(state["layers"]):
                        raw = bytag[("human_anim_layer_raw", (slot, index))]
                        next_ptr = struct.unpack_from("<I", raw, 0x48)[0]
                        seq_ptr = struct.unpack_from("<I", raw, 0x50)[0]
                        seq_ptr = seq_ptr if valid_ee_pointer(
                            seq_ptr, ANIM_SEQ_RAW_SIZE) else 0
                        expected_next = (
                            state["layers"][index + 1]["address"]
                            if index + 1 < len(state["layers"]) else 0)
                        if (next_ptr != expected_next
                                or seq_ptr != layer["seq_primary"]):
                            refresh_animation_slot(
                                pine, cache, slot, obj=target, kind="human")
                            pointer_changed = True
                            break
            for k in range(4):
                drone_raw = bytag.get(("dr_raw", k))
                if drone_raw is None:
                    continue
                route = drone_raw[AI_ROUTE_OFFSET:AI_ROUTE_OFFSET + AI_ROUTE_RAW_SIZE]
                address = struct.unpack_from("<I", drone_raw, AI_ROUTE_NODE_POINTER_OFFSET)[0]
                count = struct.unpack_from("<H", route, 0x82)[0]
                size = count * 2
                descriptor = ((address, count, size)
                              if size == 0 or valid_ee_pointer(address, size) else None)
                if descriptor != cache["route_nodes"].get(k):
                    if descriptor is None:
                        cache["route_nodes"].pop(k, None)
                    else:
                        cache["route_nodes"][k] = descriptor
                    pointer_changed = True
            if pointer_changed:
                continue
 
        if last is not None and frame0 == last:
            time.sleep(0.005)
            continue

        checkpoint_meta = None
        if args.checkpoint_dir:
            if checkpoint_next is None:
                checkpoint_next = (
                    args.checkpoint_first if args.checkpoint_first is not None else frame0)
            if frame0 >= checkpoint_next:
                checkpoint_meta = save_checkpoint(
                    pine, args.checkpoint_dir, args.checkpoint_slot, checkpoint_next, frame0)
                checkpoint_next = max(
                    checkpoint_meta["frame_before_save"], checkpoint_meta["frame_after_save"]
                ) + args.checkpoint_every

        mpg = bytag[("mpg", 0)]
        objs = [struct.unpack_from("<I", mpg, s * A.MP_SLOT_STRIDE + A.MPG_OBJ)[0] for s in range(8)]
        pkcount = struct.unpack_from("<H", bytag[("mps", 0)], A.MPS_PICKUP_COUNT - A.MPS_MP_ACTIVE)[0]
        if objs != cache["objs"] or pkcount != cache["pkcount"]:
            # Object set changed (respawn/re-register): core-only frame, re-resolve.
            resyncs += 1
            rec = {"frame": frame0, "timer_frame": timer_frame0, "resync": 1, "mpg": mpg.hex(),
                   "mps": bytag[("mps", 0)].hex(), "rng": bytag[("rng", 0)].hex(),
                   "projectiles": [], "projectiles_available": False,
                   "state_missing": ["projectiles"]}
            if rng_trace is not None:
                rec["rng_calls"], rec["rng_trace"] = R.read_events(pine, rng_trace)
            if first is None:
                first = frame0
            elif frame0 != last + 1:
                missed += frame0 - last - 1
            last = frame0
            last_new = time.monotonic()
            stalled_warned = False
            if checkpoint_meta:
                rec["checkpoint"] = checkpoint_meta
            records.append(rec)
            out.write(json.dumps(rec) + "\n")
            refresh_projectile_cache(
                pine, cache, pine.read32(A.DYNAMIC_OBJ_LIST + A.OBJ_LIST_NEXT))
            resolve(pine)
            continue
        rec = {"frame": frame0, "timer_frame": timer_frame0}
        if rng_trace is not None:
            rec["rng_calls"], rec["rng_trace"] = R.read_events(pine, rng_trace)
        rec["rate"] = struct.unpack("<f", bytag[("rate", 0)])[0]
        rec["frame_rate_int"] = struct.unpack("<I", bytag[("rate_int", 0)])[0]
        rec["pad_all"] = []
        for s in range(4):
            pad = bytag[("pad", s)]
            settings = bytag[("setting", s)]
            entry = {
                "port": cache["ports"][s],
                "w": struct.unpack_from("<H", pad, 2)[0],
                "s": list(pad[8:12]),
                "act": struct.unpack_from("<40f", settings, 0x14),
                "flg": list(settings[0x104:0x104 + 40]),
                "settings_raw": settings.hex(),
            }
            rec["pad_all"].append(entry)
        rec["pad"] = {"w": rec["pad_all"][0]["w"], "s": rec["pad_all"][0]["s"]}
        rec["act"] = rec["pad_all"][0]["act"]
        rec["flg"] = bytes(rec["pad_all"][0]["flg"]).hex()
        rec["rng"] = bytag[("rng", 0)].hex()
        rec["rng_words"] = list(struct.unpack("<4I", bytag[("rng", 0)]))
        sw = bytag[("sw", 0)]
        rec["sw"] = {"fd": sw[1], "fe": sw[2]}
        rec["mps"] = bytag[("mps", 0)].hex()
        if args.seedable:
            roster = bytag[("mp_roster", 0)]
            rec["mp_roster"] = []
            for slot in range(A.MP_NSLOTS):
                raw = roster[slot * A.MP_SLOT_STRIDE:(slot + 1) * A.MP_SLOT_STRIDE]
                name_raw = raw[:0x20]
                rec["mp_roster"].append({
                    "slot": slot,
                    "name": name_raw.split(b"\0", 1)[0].decode("ascii", "replace"),
                    "name_raw": name_raw.hex(),
                    "team": struct.unpack_from("<I", raw, 0x20)[0],
                    "character": struct.unpack_from("<I", raw, 0x24)[0],
                    "hud": raw[0x28],
                    "handicap": struct.unpack_from("<i", raw, 0x2C)[0],
                })
        rec["mpg"] = mpg.hex()
        for field, tag in (("assassin", "assassin"),
                           ("target", "assassination_target"),
                           ("golden_target", "golden_target")):
            pointer = struct.unpack("<I", bytag[(tag, 0)])[0]
            rec[field] = objs.index(pointer) if pointer and pointer in objs else -1
            rec[field + "_ptr"] = pointer
        rec["golden_effect_handle"] = struct.unpack(
            "<I", bytag[("golden_effect", 0)])[0]
        rec["golden_effect_active"] = rec["golden_effect_handle"] != 0
        rec["state_missing"] = ["transient_hit_zone"]
        rec["projectiles_available"] = True
        if args.seedable:
            changed_goal_targets = []
            for k, refs in cache["goal_refs"].items():
                bv_raw = bytag[("bv_raw", k)]
                for goal, cached_target in enumerate(refs):
                    target = struct.unpack_from(
                        "<I", bv_raw, goal * 0x50 + A.GOAL_TARGET)[0]
                    offset = target - A.MPPICKUPS
                    if (target != cached_target and
                            0 <= offset < 64 * A.MPPICKUP_STRIDE and
                            offset % A.MPPICKUP_STRIDE == 0):
                        changed_goal_targets.append(target)
            if changed_goal_targets:
                unique_targets = sorted(set(changed_goal_targets))
                goal_ranges = [(target, A.MPPICKUP_STRIDE)
                               for target in unique_targets]
                goal_ranges += [(A.GS_DONE, 4), (A.GS_FRAME_START, 4)]
                goal_chunks = read_ranges_batched(pine, goal_ranges)
                goal_done, goal_frame = (
                    struct.unpack("<I", raw)[0] for raw in goal_chunks[-2:])
                if goal_frame == frame0 and goal_done == done0:
                    for target, raw in zip(unique_targets, goal_chunks[:-2]):
                        bytag[("goal_target_raw", target)] = raw
            objective_index_by_ptr = {
                address: index for index, address in enumerate(objective_ptrs)
            }
            goal_targets = []
            for k, refs in cache["goal_refs"].items():
                bv_raw = bytag[("bv_raw", k)]
                for goal in range(len(refs)):
                    target = struct.unpack_from(
                        "<I", bv_raw, goal * 0x50 + A.GOAL_TARGET)[0]
                    ref = {"bot_slot": k + 4, "goal_slot": goal, "target_ptr": target}
                    offset = target - A.MPPICKUPS
                    if (0 <= offset < 64 * A.MPPICKUP_STRIDE
                            and offset % A.MPPICKUP_STRIDE == 0):
                        ref["pickup_index"] = offset // A.MPPICKUP_STRIDE
                        target_raw = bytag.get(("goal_target_raw", target))
                        if target_raw is not None:
                            ref["target_record_raw"] = target_raw.hex()
                    elif target == 0:
                        pass
                    elif target in objs:
                        ref["target_slot"] = objs.index(target)
                    elif target in objective_index_by_ptr:
                        ref["objective_index"] = objective_index_by_ptr[target]
                    else:
                        target_link = bytag.get(("goal_target_link", target))
                        if target_link is not None:
                            target_object = struct.unpack("<I", target_link)[0]
                            index = objective_index_by_ptr.get(target_object + 0x30)
                            if index is not None:
                                ref["objective_index"] = index
                    cache["goal_refs"].setdefault(k, [0, 0])[goal] = target
                    goal_targets.append(ref)
            rec["bot_goal_targets"] = goal_targets
            rec["seed_version"] = 5
            rec["state_complete"] = False

        parts = []
        for s, o in enumerate(objs):
            if not o or ("obj", s) not in bytag:
                parts.append(None)
                continue
            och = bytag[("obj", s)]
            pos = struct.unpack_from("<3f", och, A.OBJ_POS)
            entry = {
                "pos": [round(v, 4) for v in pos],
                "yaw": round(struct.unpack_from("<f", och, A.OBJ_YAW)[0], 6),
                "type": och[A.OBJ_TYPE],
                "state": struct.unpack_from("<H", och, A.OBJ_STATE)[0],
                "stamp": struct.unpack_from("<i", och, A.OBJ_STAMP)[0],
            }
            if args.seedable:
                entry["obj_raw"] = och.hex()
                entry["substate"] = struct.unpack_from("<H", och, A.OBJ_SUBSTATE)[0]
                entry["eye"] = list(struct.unpack_from("<3f", och, 0x70))
            if ("bl", s) in bytag:
                bd = bytag[("bl", s)]
                entry["hp"] = round(struct.unpack_from("<f", bd, 4)[0], 3)
                entry["pitch"] = round(struct.unpack_from("<f", bd, A.BL_PITCH - (A.BL_HEALTH - 4))[0], 5)
                entry["arm"] = round(struct.unpack_from("<f", bd, A.BL_ARMOUR - (A.BL_HEALTH - 4))[0], 3)
                entry["damage_flash"] = round(
                    struct.unpack_from("<f", bd, A.BL_DAMAGE_FLASH - (A.BL_HEALTH - 4))[0], 3)
            if ("autotarget", s) in bytag:
                target = struct.unpack("<I", bytag[("autotarget", s)])[0]
                entry["autolock_target_ptr"] = target
                entry["autolock_target_slot"] = next(
                    (slot for slot, ptr in enumerate(objs) if target and ptr == target), None)
            if ("feedback", s) in bytag:
                fb = bytag[("feedback", s)]
                entry["fade_colour"] = fb[0]
                entry["pain_dir"] = fb[A.BL_PAIN_DIR - A.BL_FADE_COLOUR]
                entry["pain_alpha"] = fb[A.BL_PAIN_ALPHA - A.BL_FADE_COLOUR]
            if ("fade", s) in bytag:
                fade = bytag[("fade", s)]
                entry["fade_total"] = round(struct.unpack_from("<f", fade, 0)[0], 3)
                entry["fade_timer"] = round(struct.unpack_from("<f", fade, 4)[0], 3)
            if ("cb", s) in bytag:
                cd = bytag[("cb", s)]
                entry["aim"] = cd[0x06]
                weapon_ptr = struct.unpack_from("<I", cd, 0x08)[0]
                entry["weapon_ptr"] = weapon_ptr
                if A.WEAPON_DATA <= weapon_ptr < A.WEAPON_DATA + 115 * A.WEAPON_DEF_STRIDE:
                    weapon_offset = weapon_ptr - A.WEAPON_DATA
                    if weapon_offset % A.WEAPON_DEF_STRIDE == 0:
                        entry["weapon_ptr_weapon_def_id"] = weapon_offset // A.WEAPON_DEF_STRIDE
                entry["foot"] = round(struct.unpack("<f", cd[0x3C:0x40])[0], 4)
            if args.seedable and ("bl_raw", s) in bytag:
                blr = bytag[("bl_raw", s)]
                entry["bl_raw"] = blr.hex()
                anim_raw = bytag.get(("weapon_anim_raw", s))
                if anim_raw is not None:
                    entry["weapon_anim_raw"] = anim_raw.hex()
                anim_state_raw = bytag.get(("weapon_anim_state", s))
                if anim_state_raw is not None:
                    entry["weapon_anim_state"] = struct.unpack("<h", anim_state_raw)[0]
                entry["vel"] = list(struct.unpack_from("<3f", blr, 0x10))
                entry["fall_vel"] = list(struct.unpack_from("<3f", blr, 0x50))
                entry["ammo_pool"] = list(struct.unpack_from("<33H", blr, 368))
                entry["weapon_slots"] = [
                    {"zoom": struct.unpack_from("<f", blr, 436 + i * 12)[0],
                     "clip": struct.unpack_from("<h", blr, 440 + i * 12)[0],
                     "owned": blr[442 + i * 12], "mode": blr[443 + i * 12],
                     "upgrade": struct.unpack_from("b", blr, 444 + i * 12)[0]}
                    for i in range(0x55)
                ]
                entry["weapon_timers"] = {
                    "fire_cooldown": struct.unpack_from("<f", blr, 2348)[0],
                    "last_gun": struct.unpack_from("<h", blr, 2352)[0],
                    "last_gadget": struct.unpack_from("<h", blr, 2354)[0],
                    "trigger_remaining": struct.unpack_from("<H", blr, 2358)[0],
                    "muzzle_timer": struct.unpack_from("<H", blr, 2360)[0],
                    "weapon_anim": struct.unpack_from("<I", blr, 2024)[0],
                }
            if args.seedable and ("cb_raw", s) in bytag:
                cbr = bytag[("cb_raw", s)]
                entry["cb_raw"] = cbr.hex()
                if s < n_humans:
                    entry["aim_flags"] = struct.unpack_from("<H", cbr, 0x60)[0]
                if "hp" in entry:
                    entry["alive"] = entry["hp"] > 0.0 and entry["type"] == 3
            if s >= 4 and ("drone", s - 4) in bytag:
                k = s - 4
                if args.seedable:
                    entry["drone_raw"] = bytag[("dr_raw", k)].hex()
                    entry["bv_raw"] = bytag[("bv_raw", k)].hex()
                    entry["alive"] = struct.unpack_from("<f", bytag[("dr_raw", k)], A.DRONE_HEALTH)[0] > 0.0
                dd = bytag[("drone", k)]
                entry["bhp"] = round(struct.unpack_from("<f", dd, 4)[0], 3)
                entry["dmg"] = round(struct.unpack("<f", bytag[("dmg", k)])[0], 3)
                bvd = bytag[("bv", k)]
                entry["goal"] = bvd[:0xA0].hex()
                bwd = bytag[("bw", k)]
                clips, has = [], []
                for i in range(0x55):
                    clips.append(struct.unpack_from("<H", bwd, i * 0xC + 4)[0])
                    has.append(bwd[i * 0xC + 6])
                entry["clip"] = clips
                entry["hasw"] = has
                entry["resv"] = list(struct.unpack("<64H", bytag[("br", k)])[:0x21])
                bsd = bytag[("bs", k)]
                entry["distr"] = round(struct.unpack_from("<f", bsd, 0)[0], 3)
                entry["repend"] = struct.unpack_from("<I", bsd, 4)[0]
                entry["stat"] = {
                    "node": struct.unpack_from("<H", bsd, A.BOT_NODE - A.BOT_DISTRACT)[0],
                    "goalslot": bsd[A.BOT_GOALSLOT - A.BOT_DISTRACT],
                    "stype": bsd[A.BOT_STYPE - A.BOT_DISTRACT],
                    "curweap": bsd[A.BOT_CURWEAP - A.BOT_DISTRACT],
                    "armour": bsd[A.BOT_ARMOUR - A.BOT_DISTRACT],
                    "trait": struct.unpack("b", bsd[A.BOT_TRAIT - A.BOT_DISTRACT:A.BOT_TRAIT - A.BOT_DISTRACT + 1])[0],
                }
                bod = bytag[("bo", k)]
                oth = []
                for j in range(8):
                    q = j * 0x10
                    oth.append([
                        round(struct.unpack_from("<f", bod, q + 4)[0], 2),
                        struct.unpack_from("<I", bod, q + 12)[0],
                    ])
                entry["other"] = oth
                anim_state = cache["bot_anim"].get(k)
                if anim_state:
                    entry["anim"] = animation_record(bytag, k, anim_state)
            if s < n_humans and args.weapon_anim_raw:
                anim_state = cache["human_anim"].get(s)
                if anim_state:
                    entry["anim"] = animation_record(
                        bytag, s, anim_state, "human_anim")
            parts.append(entry)
        rec["pl"] = parts
        if args.seedable:
            path_changed = False
            for k in range(4):
                drone_raw = bytag.get(("dr_raw", k))
                if drone_raw is None:
                    continue
                address = struct.unpack_from(
                    "<I", drone_raw, AI_PATH_POINTER_OFFSET)[0]
                path = cache["ai_paths"].get(k, {})
                path_raw = bytag.get(("ai_path_raw", k))
                if valid_ee_pointer(address, AI_PATH_RAW_SIZE):
                    if path.get("address") != address or path_raw is None:
                        refresh_ai_path_slot(pine, cache, k)
                        path_changed = True
                    else:
                        children = ai_path_child_pointers(path_raw)
                        if children != path["children"]:
                            cache["ai_paths"][k] = {
                                "address": address, "children": children, "valid": True,
                            }
                            path_changed = True
                elif path.get("address") != address or path.get("valid"):
                    cache["ai_paths"][k] = {
                        "address": address, "children": {}, "valid": False,
                    }
                    path_changed = True
            if path_changed:
                continue
            route_node_descriptors = {}
            route_node_raws = {}
            for k in range(4):
                if ("dr_raw", k) not in bytag:
                    continue
                drone_raw = bytag[("dr_raw", k)]
                route_raw = drone_raw[
                    AI_ROUTE_OFFSET:AI_ROUTE_OFFSET + AI_ROUTE_RAW_SIZE]
                address = struct.unpack_from(
                    "<I", drone_raw, AI_ROUTE_NODE_POINTER_OFFSET)[0]
                count = struct.unpack_from("<H", route_raw, 0x82)[0]
                size = count * 2
                route_node_descriptors[k] = (address, count, size)
                route_node_raws[k] = (
                    b"" if size == 0 else bytag.get(("route_nodes", k)))

            ai_path_rows = []
            for k in range(4):
                drone = cache["drone"].get(k)
                if not drone or ("dr_raw", k) not in bytag:
                    ai_path_rows.append({
                        "bot_slot": k + 4, "present": False, "complete": False,
                        "frame": frame0, "timer_frame": timer_frame0,
                    })
                    continue
                drone_raw = bytag[("dr_raw", k)]
                address = struct.unpack_from(
                    "<I", drone_raw, AI_PATH_POINTER_OFFSET)[0]
                route_raw = drone_raw[
                    AI_ROUTE_OFFSET:AI_ROUTE_OFFSET + AI_ROUTE_RAW_SIZE]
                path = cache["ai_paths"].get(k, {})
                path_raw = (bytag.get(("ai_path_raw", k))
                            if path.get("address") == address else None)
                route_node_address, route_node_count, route_node_size = route_node_descriptors[k]
                route_node_raw = route_node_raws.get(k)
                route_nodes_complete = (
                    route_node_raw is not None
                    and len(route_node_raw) == route_node_size
                )
                complete = route_nodes_complete and (
                    address == 0 or (
                        valid_ee_pointer(address, AI_PATH_RAW_SIZE) and path_raw is not None))
                pointers = ai_path_child_pointers(path_raw) if path_raw is not None else {}
                pointees = []
                for offset in AI_PATH_CHILD_POINTER_OFFSETS:
                    pointer = pointers.get(offset, 0)
                    valid = valid_ee_pointer(pointer, AI_PATH_CHILD_RAW_SIZE)
                    raw = None
                    if valid:
                        if path.get("children", {}).get(offset) == pointer:
                            raw = bytag.get(("ai_path_child_raw", (k, offset)))
                        if raw is None:
                            complete = False
                    elif pointer:
                        complete = False
                    pointees.append({
                        "source_offset": offset, "address": pointer,
                        "valid": valid, "size": AI_PATH_CHILD_RAW_SIZE if valid else 0,
                        "raw": raw.hex() if raw is not None else None,
                    })
                row = {
                    "bot_slot": k + 4, "present": True, "complete": complete,
                    "frame": frame0, "timer_frame": timer_frame0,
                    "drone_ptr": drone, "route_address": drone + AI_ROUTE_OFFSET,
                    "route_size": AI_ROUTE_RAW_SIZE, "route_raw": route_raw.hex(),
                    "route_node_address": route_node_address,
                    "route_node_count": route_node_count,
                    "route_node_size": route_node_size,
                    "route_node_raw": route_node_raw.hex() if route_node_raw is not None else None,
                    "ai_path_pointer_source_offset": AI_PATH_POINTER_OFFSET,
                    "ai_path_address": address,
                    "ai_path_valid": valid_ee_pointer(address, AI_PATH_RAW_SIZE),
                    "ai_path_size": AI_PATH_RAW_SIZE,
                    "ai_path_raw": path_raw.hex() if path_raw is not None else None,
                    "pointees": pointees,
                }
                ai_path_rows.append(row)
                if path_raw is not None:
                    cache["ai_paths"][k] = {
                        "address": address,
                        "children": pointers,
                        "valid": True,
                    }
                if not complete:
                    rec["state_missing"].append("bot_ai_path_pointees")
                if not route_nodes_complete:
                    rec["state_missing"].append("bot_ai_route_nodes")
            rec["bot_ai_paths"] = ai_path_rows

        projectile_gap = not cache["dynamic_scan_ok"]
        projectiles = []
        retired_projectiles = []
        head0 = struct.unpack("<I", bytag[("projectile_head0", 0)])[0]
        head1 = struct.unpack("<I", bytag[("projectile_head1", 0)])[0]
        if head0 != head1:
            projectile_gap = True
        added_projectiles = []
        if head0 != projectile_old_head or head1 != projectile_old_head:
            added_projectiles = refresh_projectile_cache(pine, cache, head1)
        if added_projectiles:
            valid_added = []
            for obj in added_projectiles:
                data = cache["projectiles"][obj]
                if data < 0x00100000 or data + A.BULLET_RAW_SIZE > 0x02000000:
                    projectile_gap = True
                else:
                    valid_added.append(obj)
            extra_ranges = []
            extra_tags = []
            for obj in valid_added:
                data = cache["projectiles"][obj]
                extra_tags += [("projectile_obj", obj), ("projectile_data", obj)]
                extra_ranges += [(obj, 0x100), (data, A.BULLET_RAW_SIZE)]
            if valid_added:
                extra_ranges += [
                    (A.DYNAMIC_OBJ_LIST + A.OBJ_LIST_NEXT, 4),
                    (A.GS_DONE, 4), (A.GS_FRAME, 4)]
                extra_chunks = read_ranges_batched(pine, extra_ranges)
                for tag, raw in zip(extra_tags, extra_chunks[:-3]):
                    bytag[tag] = raw
                extra_head, extra_done, extra_frame = (
                    struct.unpack("<I", raw)[0] for raw in extra_chunks[-3:])
                if (extra_head != head1 or extra_done != done0 or
                        extra_frame != frame0):
                    projectile_gap = True
                for obj in valid_added:
                    raw = bytag[("projectile_obj", obj)]
                    if (raw[A.OBJ_TYPE] != 5 or
                            struct.unpack_from("<I", raw, A.OBJ_CUSTOM_DATA)[0] !=
                            cache["projectiles"][obj]):
                        projectile_gap = True
        for obj, expected_data in cache["projectiles"].items():
            obj_raw = bytag.get(("projectile_obj", obj))
            data_raw = bytag.get(("projectile_data", obj))
            if obj_raw is None or data_raw is None or obj_raw[A.OBJ_TYPE] != 5:
                projectile_gap = True
                retired_projectiles.append(obj)
                continue
            data = struct.unpack_from("<I", obj_raw, A.OBJ_CUSTOM_DATA)[0]
            if data != expected_data:
                projectile_gap = True
                retired_projectiles.append(obj)
                continue
            owner = struct.unpack_from("<I", data_raw, A.BULLET_OWNER)[0]
            target = struct.unpack_from("<I", data_raw, A.BULLET_TARGET)[0]
            weapon_def = struct.unpack_from("<I", data_raw, A.BULLET_WEAPON_DEF)[0]
            weapon_id = None
            if A.WEAPON_DATA <= weapon_def < A.WEAPON_DATA + 115 * A.WEAPON_DEF_STRIDE:
                offset = weapon_def - A.WEAPON_DATA
                if offset % A.WEAPON_DEF_STRIDE == 0:
                    weapon_id = offset // A.WEAPON_DEF_STRIDE
            projectiles.append({
                "obj": obj,
                "data": data,
                "pos": list(struct.unpack_from("<3f", obj_raw, A.OBJ_POS)),
                "yaw": struct.unpack_from("<f", obj_raw, A.OBJ_YAW)[0],
                "state": struct.unpack_from("<H", obj_raw, A.OBJ_STATE)[0],
                "owner": owner,
                "owner_slot": next((slot for slot, ptr in enumerate(objs) if owner and ptr == owner), None),
                "target": target,
                "weapon_def": weapon_def,
                "weapon_id": weapon_id,
                "dir": list(struct.unpack_from("<3f", data_raw, A.BULLET_DIR)),
                "travelled": struct.unpack_from("<f", data_raw, A.BULLET_TRAVELLED)[0],
                "speed": struct.unpack_from("<f", data_raw, A.BULLET_SPEED)[0],
                "timer": struct.unpack_from("<f", data_raw, A.BULLET_TIMER)[0],
                "bounces": struct.unpack_from("<H", data_raw, A.BULLET_BOUNCES)[0],
                "in_air": data_raw[A.BULLET_IN_AIR],
                "obj_raw": obj_raw.hex(),
                "data_raw": data_raw.hex(),
            })
            state = struct.unpack_from("<H", obj_raw, A.OBJ_STATE)[0]
            if obj_raw[0xFE] & 1 or state in (2, 4):
                retired_projectiles.append(obj)
        for obj in retired_projectiles:
            cache["projectiles"].pop(obj, None)
        rec["projectiles"] = projectiles
        rec["projectiles_available"] = not projectile_gap
        if projectile_gap:
            rec["state_missing"].append("projectiles")

        pks = []
        for obj, (info, pos, pickup_idx) in cache["pinfo"].items():
            p = {"obj": obj, "pos": pos,
                 "stamp": struct.unpack("<i", bytag[("pkstamp", obj)])[0],
                 "visit_until": list(struct.unpack("<4f", bytag[("pkvisit", obj)]))}
            if ("pi", obj) in bytag:
                ch = bytag[("pi", obj)]
                p["st"] = struct.unpack_from("<h", ch, 0)[0]
                p["cat"] = struct.unpack_from("<H", ch, 2)[0]
                p["item"] = struct.unpack_from("<H", ch, 4)[0]
                p["respawn_units"] = struct.unpack_from(
                    "<H", ch, A.PI_RESPAWN_UNITS - A.PI_STATE)[0]
                p["lifetime_frames"] = struct.unpack_from(
                    "<H", ch, A.PI_LIFETIME_FRAMES - A.PI_STATE)[0]
                p["amount"] = struct.unpack_from(
                    "<H", ch, A.PI_AMOUNT - A.PI_STATE)[0]
                p["radar_hidden"] = bool(bytag[("pkflags", obj)][0] & 0x10)
                p["idx"] = struct.unpack_from("<h", ch, 16)[0]
            else:
                p["idx"] = pickup_idx
            pks.append(p)
        rec["pk"] = pks
        pickup_indexes = {p["idx"] for p in pks if "idx" in p}
        goal_targets_complete = all(
            ref["target_ptr"] == 0 or "target_slot" in ref
            or "objective_index" in ref
            or ref.get("pickup_index") in pickup_indexes
            for ref in rec.get("bot_goal_targets", [])
        )
        if not goal_targets_complete:
            rec["state_missing"].append("bot_goal_target_changed")

        if args.seedable:
            rec["objx"] = {name: bytag[("objx", name)].hex() for name, _, _ in FULL_BLOBS}
            rec["objectives"] = []
            for obj in objective_ptrs:
                raw = bytag[("mp_object_raw", obj)]
                kind = struct.unpack_from("<H", raw, 0xE0)[0]
                carrier = struct.unpack_from("<I", raw, 0xE8)[0]
                objective = {
                    "obj": obj,
                    "kind": kind,
                    "team": struct.unpack_from("<H", raw, 0xE2)[0],
                    "timer": struct.unpack_from("<H", raw, 0xE4)[0],
                    "carrier_slot": next((slot for slot, ptr in enumerate(objs)
                                          if carrier and ptr == carrier), None),
                    "state": struct.unpack_from("<h", raw, 0xF4)[0],
                    "substate": struct.unpack_from("<h", raw, 0xF6)[0],
                    "flags": raw[0xF0],
                    "radar_hidden": bool(raw[0xF0] & 0x10),
                    "pos_0x30": list(struct.unpack_from("<3f", raw, 0x30)),
                    "pos_0x40": list(struct.unpack_from("<3f", raw, 0x40)),
                    "yaw": struct.unpack_from("<f", raw, A.OBJ_YAW)[0],
                    "last_damager": struct.unpack_from("<h", raw, 0x130)[0],
                    "capturer": struct.unpack_from("<h", raw, 0x132)[0],
                    "draw_view_mask": raw[0x106],
                    "obj_raw": raw.hex(),
                }
                if kind in (3, 8):
                    objective["hit_points_candidate_0xF4"] = struct.unpack_from(
                        "<h", raw, 0xF4)[0]
                rec["objectives"].append(objective)
        elif frame0 % args.full_every == 0:
            blobs = read_ranges_batched(pine, [(addr, size) for _, addr, size in FULL_BLOBS])
            after_done, after_frame = (
                struct.unpack("<I", raw)[0]
                for raw in pine.read_ranges([(A.GS_DONE, 4), (A.GS_FRAME_START, 4)]))
            if after_frame == frame0 and after_done == done0:
                rec["objx"] = {
                    name: blob.hex()
                    for (name, _, _), blob in zip(FULL_BLOBS, blobs)
                }
        if args.seedable:
            pickup_fields_ready = all(
                isinstance(p, dict)
                and all(field in p for field in
                        ("idx", "st", "cat", "item", "stamp", "amount", "lifetime_frames", "visit_until"))
                and isinstance(p["visit_until"], list) and len(p["visit_until"]) == 4
                for p in rec["pk"])
            pickup_identity_ready = pickup_fields_ready and (
                len({p["idx"] for p in rec["pk"]}) == len(rec["pk"])
                and all(0 <= p["idx"] < 64 and 0 <= p["stamp"] <= timer_frame0 for p in rec["pk"]))
            pickup_seed_ready = pickup_fields_ready and pickup_identity_ready
            if not pickup_fields_ready:
                rec["state_missing"].append("pickup_seed_fields")
            elif not pickup_identity_ready:
                rec["state_missing"].append("pickup_seed_identity")
            projectile_refs_ready = rec["projectiles_available"] and all(
                p.get("owner_slot") is not None and p.get("weapon_id") is not None
                for p in rec["projectiles"])
            if rec["projectiles_available"] and not projectile_refs_ready:
                rec["state_missing"].append("projectile_seed_reference")
            weapon_anim_state_ready = all(
                p is None or not p.get("weapon_timers", {}).get("weapon_anim")
                or "weapon_anim_state" in p
                for p in rec["pl"][:n_humans])
            if not weapon_anim_state_ready:
                rec["state_missing"].append("weapon_anim_state")
            bot_animation_ready = all(
                p is None or p.get("anim", {}).get("layers_complete") is True
                for p in rec["pl"][4:8])
            if not bot_animation_ready:
                rec["state_missing"].append("bot_animation_layers")
            bot_collision_ready = all(
                p is None or "cb_raw" in p for p in rec["pl"][4:8])
            if not bot_collision_ready:
                rec["state_missing"].append("bot_collision_body")
            rec["seed_ready"] = (
                "objx" in rec and len(rec["rng_words"]) == 4 and len(rec["pad_all"]) == 4
                and all(p is None or "obj_raw" in p for p in rec["pl"])
                and all(p is None or "autolock_target_ptr" in p for p in rec["pl"][:n_humans])
                and pickup_seed_ready
                and len(rec.get("mp_roster", [])) == A.MP_NSLOTS
                and all("obj_raw" in objective for objective in rec.get("objectives", []))
                and "bot_goal_target_changed" not in rec["state_missing"]
                and projectile_refs_ready
                and weapon_anim_state_ready
                and bot_collision_ready
                and not any(field != "transient_hit_zone" for field in rec["state_missing"])
            )
            if "objx" not in rec:
                rec["state_missing"].append("objective_blobs")
        if first is None:
            first = frame0
        elif frame0 != last + 1:
            missed += frame0 - last - 1
        last = frame0
        last_new = time.monotonic()
        stalled_warned = False
        if checkpoint_meta:
            rec["checkpoint"] = checkpoint_meta
        records.append(rec)
        out.write(json.dumps(rec) + "\n")
        if len(records) % 200 == 0:
            print(f"  ... {len(records)} frames (frame {frame0})", flush=True)
        rel = frame0 - first
        while steps and steps[0][0] <= rel:
            vpad(*steps.pop(0)[1])
        if rel >= args.frames:
            break

    vpad("release")
    out.close()
    print(f"{len(records)} frames {first}..{last}, missed {missed}, resyncs {resyncs} -> {args.out}")


if __name__ == "__main__":
    main()
