"""Capture one human MP spawn pose and a 30-tick reference row, freezing bot slot 4."""
import argparse
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mp_addrs as A
from pine import Pine, WRITE32
from trace import GLB_VIEWER


def vpad(*words):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "nf-vpad.sock"))
    sock.sendall((" ".join(map(str, words)) + "\n").encode())
    reply = sock.recv(256)
    sock.close()
    if not reply.startswith(b"ok"):
        raise RuntimeError(f"vpad failed: {reply!r}")


def floats(raw, count):
    return list(struct.unpack("<" + "f" * count, raw))


def f32_word(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]

def pcsx2_pid():
    socket_path = os.path.join(
        os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"),
        "pcsx2.sock",
    )
    socket_inode = None
    with open("/proc/net/unix", encoding="ascii") as stream:
        for line in stream:
            fields = line.split()
            if len(fields) >= 8 and fields[-1] == socket_path:
                socket_inode = fields[6]
                break
    if socket_inode is None:
        raise RuntimeError(f"PCSX2 PINE socket not found: {socket_path}")

    socket_ref = f"socket:[{socket_inode}]"
    matches = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            for fd in os.listdir(f"/proc/{entry}/fd"):
                try:
                    if os.readlink(f"/proc/{entry}/fd/{fd}") == socket_ref:
                        matches.append(int(entry))
                        break
                except OSError:
                    continue
        except OSError:
            continue
    if len(matches) != 1:
        raise RuntimeError(f"expected one PCSX2 PINE socket owner, found {matches}")
    return matches[0]


def write_pose(pine, obj, pos, yaw):
    fields = [(obj + A.OBJ_POS + 4 * i, value) for i, value in enumerate(pos)]
    fields.append((obj + A.OBJ_YAW, yaw))
    body = b"".join(
        struct.pack("<BI", WRITE32, addr) + struct.pack("<I", f32_word(value))
        for addr, value in fields
    )
    pine._transact(body)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--slot", type=int, required=True, help="saved match-start PCSX2 slot")
    ap.add_argument("--out", required=True, help="JSONL destination (one row)")
    ap.add_argument("--screenshot", required=True, help="PNG destination")
    ap.add_argument("--frames", type=int, default=30)
    ap.add_argument("--bots", type=int, default=1, help="expected bot count; bot slot 4 is frozen")
    ap.add_argument("--timeout", type=float, default=15.0)
    ap.add_argument("--screenshot-delay", type=float, default=0.0,
                    help="optional seconds after target frame before exact frozen capture")
    ap.add_argument("--crop", default=None,
                    help="optional PCSX2 viewport crop x,y,width,height in desktop pixels")
    args = ap.parse_args()
    if args.crop is not None:
        try:
            args.crop = tuple(int(value) for value in args.crop.split(","))
        except ValueError:
            ap.error("--crop must be x,y,width,height")
        if len(args.crop) != 4 or args.crop[2] <= 0 or args.crop[3] <= 0:
            ap.error("--crop must be x,y,width,height with positive size")
    if args.frames <= 0:
        ap.error("--frames must be positive")
    if args.screenshot_delay < 0:
        ap.error("--screenshot-delay must be non-negative")

    pine = Pine()
    pcsx2_stopped = False
    capture_pid = None
    try:
        if pine.game_id() != "SLUS-20579":
            raise RuntimeError(f"unexpected PCSX2 game: {pine.game_id()}")
        vpad("release")
        previous_frame = pine.read32(A.GS_FRAME_START)
        pine.load_state(args.slot)
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            frame = pine.read32(A.GS_FRAME_START)
            obj = pine.read32(A.GLB_PLAYERS)
            if (frame != previous_frame and obj
                    and pine.read_block(obj + A.OBJ_TYPE, 1)[0] == 3):
                break
            time.sleep(0.002)
        else:
            raise RuntimeError("game did not resume into a live MP human after savestate load")

        active, humans, bots, map_id = (
            struct.unpack("<I", value)[0]
            for value in pine.read_ranges([
                (A.MPSETTINGS + A.MPS_MP_ACTIVE, 4),
                (A.MPSETTINGS + A.MPS_HUMANS, 4),
                (A.MPSETTINGS + A.MPS_BOTS, 4),
                (A.MPSETTINGS + A.MPS_MAP, 4),
            ])
        )
        if active != 1:
            raise RuntimeError(f"savestate is not an active MP match (active={active})")
        if humans != 1:
            raise RuntimeError(f"fixed-pose capture requires one human (found {humans})")
        if bots != args.bots:
            raise RuntimeError(f"expected {args.bots} bots for fixed-pose capture (found {bots})")
        viewer = pine.read32(GLB_VIEWER)
        if not viewer:
            raise RuntimeError("viewer slot 0 pointer is null")
        obj = pine.read32(A.GLB_PLAYERS)
        if not obj:
            raise RuntimeError("human player slot 0 is empty")
        if pine.read_block(obj + A.OBJ_TYPE, 1)[0] != 3:
            raise RuntimeError("slot 0 does not point to a live human player object")
        bl = pine.read32(obj + A.OBJ_BL)
        cb = pine.read32(obj + A.OBJ_COLL)
        if not bl or not cb:
            raise RuntimeError("player BLData or collision-body pointer is null")
        bot_obj = pine.read32(A.MPGAME + 4 * A.MP_SLOT_STRIDE + A.MPG_OBJ) if bots else 0
        bot_pos = bot_yaw = None
        if bot_obj:
            if pine.read_block(bot_obj + A.OBJ_TYPE, 1)[0] != 2:
                raise RuntimeError("configured bot slot 4 does not point to a bot object")
            bot_pos_raw, bot_yaw_raw = pine.read_ranges([
                (bot_obj + A.OBJ_POS, 12), (bot_obj + A.OBJ_YAW, 4),
            ])
            bot_pos = floats(bot_pos_raw, 3)
            bot_yaw = struct.unpack("<f", bot_yaw_raw)[0]
            write_pose(pine, bot_obj, bot_pos, bot_yaw)

        native_pos, native_yaw, native_pitch = pine.read_ranges([
            (obj + A.OBJ_POS, 12), (obj + A.OBJ_YAW, 4), (bl + A.BL_PITCH, 4),
        ])
        pos = floats(native_pos, 3)
        yaw = struct.unpack("<f", native_yaw)[0]
        pitch = struct.unpack("<f", native_pitch)[0]
        write_pose(pine, obj, pos, yaw)
        pine.write(WRITE32, bl + A.BL_PITCH, f32_word(pitch))

        start_frame = pine.read32(A.GS_FRAME_START)
        target_frame = start_frame + args.frames
        deadline = time.monotonic() + args.timeout
        for expected in range(start_frame + 1, target_frame + 1):
            while True:
                frame = pine.read32(A.GS_FRAME_START)
                if frame >= expected:
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError(f"timed out at frame {frame}, target {expected}")
                time.sleep(0.001)
            if frame != expected:
                raise RuntimeError(f"missed target logic frame: expected {expected}, got {frame}")
            if bot_obj:
                write_pose(pine, bot_obj, bot_pos, bot_yaw)

        # Duplicate the frame counters at both ends of one PINE transaction to
        # reject any read that straddles the next game update.
        ranges = [
            (A.GS_FRAME_START, 4), (A.GS_FRAME, 4),
            (obj + A.OBJ_POS, 12), (obj + A.OBJ_YAW, 4), (bl + A.BL_PITCH, 4),
            (obj + 0x70, 12), (GLB_VIEWER, 4), (viewer + 0x118, 4),
            (cb + A.CB_98, 4),
            (bl + 0x133, 1),
        ]
        if bot_obj:
            ranges += [(bot_obj + A.OBJ_POS, 12), (bot_obj + A.OBJ_YAW, 4)]
        ranges += [(A.GS_FRAME_START, 4), (A.GS_FRAME, 4)]
        sample = pine.read_ranges(ranges)
        sf0, done0 = struct.unpack("<II", sample[0] + sample[1])
        sf1, done1 = struct.unpack("<II", sample[-2] + sample[-1])
        if sf0 != target_frame or sf1 != target_frame or done0 != done1:
            raise RuntimeError(f"frame counters changed during snapshot: {(sf0, done0)} -> {(sf1, done1)}")
        if args.screenshot_delay == 0:
            capture_pid = pcsx2_pid()
            os.kill(capture_pid, signal.SIGSTOP)
            pcsx2_stopped = True
        final_pos, final_yaw, final_pitch, eye, viewer_raw, fov, cb_98_raw = sample[2:9]
        if struct.unpack("<I", viewer_raw)[0] != viewer:
            raise RuntimeError("viewer slot 0 changed during snapshot")
        crosshair_raw = sample[9]
        final_bot_pos = floats(sample[10], 3) if bot_obj else None
        final_bot_yaw = struct.unpack("<f", sample[11])[0] if bot_obj else None

        map_name = next((name for ident, name in A.MP_MAPS if ident == map_id), "unknown")
        record = {
            "protocol": "fixed-spawn-v2",
            "slot": args.slot,
            "map_id": f"0x{map_id:08x}",
            "map": map_name,
            "mode": "Arena/FFA",
            "bots": bots,
            "humans": humans,
            "logic_frames_after_native_spawn": args.frames,
            "start_frame": start_frame,
            "target_frame": target_frame,
            "frame_start": sf1,
            "frame_done": done1,
            "native_pose": {"pos": pos, "yaw": yaw, "pitch": pitch},
            "pose": {"pos": floats(final_pos, 3), "yaw": struct.unpack("<f", final_yaw)[0],
                     "pitch": struct.unpack("<f", final_pitch)[0]},
            "cb_0x98_raw": cb_98_raw.hex(),
            "crosshair_kind": crosshair_raw[0],
            "crosshair_kind_source": "BLData+0x133 (HUD_UpdateCrossHair @0x19E6D0)",
            "eye": floats(eye, 3),
            "viewer": f"0x{viewer:08x}",
            "fov": struct.unpack("<f", fov)[0],
            "fov_source": "glb_viewer[0] +0x118 (Camera_CalcViewAngles)",
            "bot_pose_after": None if not bot_obj else {"pos": final_bot_pos, "yaw": final_bot_yaw},
            "bot_freeze_method": (
                "PINE restores slot4 bot obj+0x30 position and +0x54 yaw after each logic-frame start"
                if bot_obj else None
            ),
            "screenshot": os.path.abspath(args.screenshot),
        }
        screenshot_deadline = time.monotonic() + args.screenshot_delay
        screenshot_frame = target_frame
        while time.monotonic() < screenshot_deadline:
            frame = pine.read32(A.GS_FRAME_START)
            if frame > screenshot_frame:
                write_pose(pine, obj, pos, yaw)
                pine.write(WRITE32, bl + A.BL_PITCH, f32_word(pitch))
                if bot_obj:
                    write_pose(pine, bot_obj, bot_pos, bot_yaw)
                screenshot_frame = frame
            time.sleep(0.001)
        if args.screenshot_delay:
            frame_before = pine.read32(A.GS_FRAME_START)
            if frame_before > screenshot_frame:
                write_pose(pine, obj, pos, yaw)
                pine.write(WRITE32, bl + A.BL_PITCH, f32_word(pitch))
                if bot_obj:
                    write_pose(pine, bot_obj, bot_pos, bot_yaw)
            capture_pid = pcsx2_pid()
            os.kill(capture_pid, signal.SIGSTOP)
            pcsx2_stopped = True
        else:
            frame_before = target_frame
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        os.makedirs(os.path.dirname(os.path.abspath(args.screenshot)), exist_ok=True)
        display = os.environ.get("DISPLAY", ":100")
        if os.environ.get("WAYLAND_DISPLAY"):
            capture_env = os.environ.copy()
            capture_env["QT_QPA_PLATFORM"] = "wayland"
            subprocess.run(
                ["spectacle", "-b", "-n", "-o", args.screenshot],
                env=capture_env,
                check=True,
            )
            display = f"WAYLAND_DISPLAY={os.environ['WAYLAND_DISPLAY']}"
        else:
            subprocess.run(
                ["import", "-display", display, "-window", "root", args.screenshot],
                check=True,
            )
        if args.crop is not None:
            from PIL import Image
            image = Image.open(args.screenshot)
            x, y, width, height = args.crop
            image.crop((x, y, x + width, y + height)).save(args.screenshot)
        os.kill(capture_pid, signal.SIGCONT)
        pcsx2_stopped = False
        frame_after = pine.read32(A.GS_FRAME_START)
        record["screenshot_frame_range"] = [frame_before, frame_after]
        record["screenshot_lag_frames_range"] = [
            frame_before - target_frame, frame_after - target_frame]
        record["screenshot_frozen"] = frame_before == target_frame
        record["screenshot_crop"] = None if args.crop is None else list(args.crop)
        record["screenshot_display"] = display
        with open(args.out, "w", encoding="utf-8") as stream:
            stream.write(json.dumps(record, separators=(",", ":")) + "\n")
        print(json.dumps(record, indent=2), flush=True)
    finally:
        if pcsx2_stopped:
            os.kill(capture_pid, signal.SIGCONT)
        pine.close()


if __name__ == "__main__":
    main()
