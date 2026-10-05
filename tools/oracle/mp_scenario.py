"""MP match setup driver: main menu (PINE slot 50) -> configured MP match via vpad.

Learned flow (2026-10-03, verified live):
  main menu -down-> Multiplayer -cross-> P_MPJOIN -cross-> join
  -cross-> codename -cross-> ready -guarded cross-> scenario wheel
  (-up x15 to clamp top, -down xN-> target) -cross-> map wheel (same clamp)
  -cross-> character (default) -cross-> handicap (0) -cross-> Scenario Options
  -down-> AI Bots -cross-> [Setup Bot k -cross-> char -cross-> config(Playing:Yes
  default) -cross->]* -triangle-> options -up-> Continue -cross-> confirm
  -cross-> load (poll MP active + glb_players) -> spawn.

The scenario wheel eats input during its iris transition: settle 10 s after
every page entry, 8 s between moves (game at ~30 fps wall). Locked scenarios
(Demolition/Protection/...) are still SELECTABLE with menu_unlock_everything
(0x30d2a7) set: grey display, working select. Bot defaults (Snow Guard, Black
Ops, Yakuza) come up Playing:Yes once their config page is confirmed.

Usage: mp_scenario.py --scenario 1 --map 0 --bots 3 --slot 12 [--shots dir]
         [--settle 8] [--page-settle 10] [--startup-settle 30]
         [--press-ms 400] [--nav-ms 800] [--live-timeout 120]
Use `--startup-settle` for a cold/slow title load and `--press-ms 2000` for
confirm buttons on a slow EE interpreter. Menu navigation/back taps have their
own shorter duration; increase `--nav-ms` if needed.
"""

import argparse
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mp_addrs as A
from pine import Pine, WRITE8, WRITE16, WRITE32

PRESS_MS = 400
NAV_MS = 800

def vpad(*words):
    if len(words) >= 3 and words[0] == "press" and words[2] == 400:
        duration = PRESS_MS if words[1] == "cross" else NAV_MS
        words = (*words[:2], duration, *words[3:])
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "nf-vpad.sock"))
    s.sendall((" ".join(map(str, words)) + "\n").encode())
    reply = s.recv(256)
    s.close()
    return reply


def hold(btn, secs=2.0):
    # Use one paired input command so the bridge releases the button reliably.
    return vpad("press", btn, int(secs * 1000))


def shot(path):
    if not path:
        return
    os.system(f"WAYLAND_DISPLAY=wayland-0 spectacle -b -n -o {path} >/dev/null 2>&1")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", type=int, default=1, help="wheel index from Quick Game")
    ap.add_argument("--map", type=int, default=0, help="wheel index from Skyrail")
    ap.add_argument("--direct-map-at-confirm", action="store_true",
                    help="write the chosen MP map ID at Scenario Options/Confirm instead of relying on the map wheel")
    ap.add_argument("--direct-scenario-at-confirm", action="store_true",
                    help="select Arena in the wheel, then write the requested scenario mask at Confirm")
    ap.add_argument("--bots", type=int, default=3)
    ap.add_argument("--slot", type=int, default=12, help="PINE slot for the match-start state")
    ap.add_argument("--bot-chars", default="", help="e.g. 6,5,8: char ids poked into mpbots at confirm (skip wheel)")
    ap.add_argument("--bot-teams", default="", help="e.g. 0,1,0: teams poked alongside --bot-chars")
    ap.add_argument("--shots", default="", help="dir for per-page screenshots")
    ap.add_argument("--press-ms", type=int, default=400,
                    help="duration for scripted confirm taps (raise for slow EE interpreter)")
    ap.add_argument("--nav-ms", type=int, default=800,
                    help="duration for menu navigation taps")
    ap.add_argument("--settle", type=float, default=8.0)
    ap.add_argument("--page-settle", type=float, default=10.0,
                    help="seconds to wait after entering a menu page before confirming it")
    ap.add_argument("--load-slot", type=int, default=50)
    ap.add_argument("--startup-settle", type=float, default=30.0,
                    help="seconds to wait after loading the title-menu savestate")
    ap.add_argument("--live-timeout", type=float, default=120.0,
                    help="seconds to wait for the match player after loading")
    ap.add_argument("--spawn-stabilize", type=int, default=60,
                    help="logic frames after first live player before saving a match-start state")
    ap.add_argument("--weapon-set", type=int, default=None,
                    help="override MP PickupMatrix row (0..10) before match start")
    ap.add_argument("--time-limit-sec", type=float, default=None,
                    help="override the live MP match timer before saving its start state")
    args = ap.parse_args()
    global PRESS_MS, NAV_MS
    PRESS_MS = args.press_ms
    NAV_MS = args.nav_ms

    if args.weapon_set is not None and not 0 <= args.weapon_set <= 10:
        ap.error("--weapon-set must be in 0..10")
    if args.time_limit_sec is not None and args.time_limit_sec <= 0:
        ap.error("--time-limit-sec must be positive")
    if args.spawn_stabilize < 0:
        ap.error("--spawn-stabilize must be non-negative")

    if args.shots:
        os.makedirs(args.shots, exist_ok=True)

    def snap(name):
        if args.shots:
            shot(os.path.join(args.shots, name + ".png"))
            time.sleep(1.0)

    pine = Pine()
    vpad("release")
    before = pine.read32(A.GS_FRAME_START)
    pine.load_state(args.load_slot)
    start = time.monotonic()
    while abs(pine.read32(A.GS_FRAME_START) - before) <= 20 and time.monotonic() - start < 4.0:
        time.sleep(0.05)
    time.sleep(args.startup_settle)
    snap("01-main")

    vpad("press", "down", 400)
    time.sleep(args.settle)
    hold("cross")
    time.sleep(args.page_settle)
    snap("02-join")
    hold("cross")
    time.sleep(args.settle)
    hold("cross")
    time.sleep(args.page_settle)
    hold("cross")
    time.sleep(args.page_settle)
    def poke_roster(team_override=None):
        if not args.bot_chars:
            return
        chars = [int(x) for x in args.bot_chars.split(",")]
        teams = ([team_override] * len(chars) if team_override is not None
                 else ([int(x) for x in args.bot_teams.split(",")] if args.bot_teams else [0] * len(chars)))
        assert len(chars) == len(teams) and len(chars) <= 4, "--bot-chars/_teams: up to 4 paired ids"
        assert team_override is not None or set(teams) == {0, 1} or args.scenario in (0, 1), \
            "team modes need both teams populated"
        stats = pine.read_block(0x26D2F0, 29 * 14)   # default_bot_stats
        writes = [
            (WRITE32, A.MPSETTINGS + A.MPS_HUMANS, 1),
            (WRITE32, A.MPSETTINGS + A.MPS_BOTS, len(chars)),
            (WRITE32, A.MPSETTINGS + A.MPS_PARTICIPANTS, 1 + len(chars)),
            (WRITE32, A.MPSETTINGS + 0x20, 1),
            (WRITE32, A.MPSETTINGS + 0x24, 1),
            (WRITE32, A.MPSETTINGS + 0x28, 1),
            (WRITE32, A.MPSETTINGS + 0x2C, 0),
            (WRITE8, 0x2DEEA8, 1),
            (WRITE8, 0x2DEEA8 + 1, len(chars)),
        ]
        formats = {WRITE8: "<B", WRITE16: "<H", WRITE32: "<I"}
        for k, (ch, tm) in enumerate(zip(chars, teams)):
            base = 0x2DEEAA + k * 0x12
            row = stats[ch * 14:(ch + 1) * 14]
            writes.extend(
                (WRITE16, base + j, struct.unpack_from("<H", row, j)[0])
                for j in range(0, 14, 2)
            )
            writes.extend((
                (WRITE8, base + 0x0E, 1),
                (WRITE8, base + 0x0F, tm),
                (WRITE8, base + 0x10, ch),
                (WRITE8, base + 0x11, 1),
            ))
            slot = A.MPSETTINGS + (4 + k) * A.MP_SLOT_STRIDE
            writes.extend((
                (WRITE32, slot + 0x20, 2),
                (WRITE32, slot + 0x24, ch),
                (WRITE32, slot + 0x28, 1),
                (WRITE32, slot + 0x2C, tm),
            ))
        body = b"".join(
            struct.pack("<BI", op, addr) + struct.pack(formats[op], value)
            for op, addr, value in writes
        )
        pine._transact(body)
        print("poked bot roster", list(zip(chars, teams)), flush=True)
    # Keep the Arena wheel's early validation happy when the requested team mode
    # is committed directly at Confirm; the intended teams are re-poked there.
    poke_roster(0 if args.direct_scenario_at_confirm else None)
    # On a slow interpreter the ready prompt can eat the transition tap. Probe
    # the scenario mask around one additional tap so we do not accidentally
    # select Quick Game when the wheel already opened.
    pine.write(WRITE32, A.MPSETTINGS + A.MPS_SCENARIO_MASK, 0xAAAAAAAA)
    hold("cross")
    time.sleep(args.page_settle)
    if pine.read32(A.MPSETTINGS + A.MPS_SCENARIO_MASK) != 0xAAAAAAAA:
        vpad("press", "triangle", 400)
        time.sleep(args.page_settle)
        pine.write(WRITE32, A.MPSETTINGS + A.MPS_SCENARIO_MASK, 0xAAAAAAAA)

    snap("03-scenario")
    # The wheel opens with Arena highlighted. Quick Game has already been
    # passed on the preceding ready/options page, so do not step down again.
    target_mask = A.MP_SCENARIOS[args.scenario][0]
    wheel_mask = A.MP_SCENARIOS[1][0] if args.direct_scenario_at_confirm else target_mask
    for attempt in range(14):
        pine.write(WRITE8, A.MENU_UNLOCK_EVERYTHING, 1)
        pine.write(WRITE32, A.MPSETTINGS + A.MPS_SCENARIO_MASK, 0xAAAAAAAA)
        vpad("press", "cross", 400)
        time.sleep(args.page_settle)
        m = pine.read32(A.MPSETTINGS + A.MPS_SCENARIO_MASK)
        if m == wheel_mask:
            break
        print(f"  scenario select gave {m:#x}, cycling", flush=True)
        vpad("press", "triangle", 400)
        time.sleep(args.page_settle)
        if m == 0xAAAAAAAA:
            time.sleep(args.page_settle)   # select never fired: page not ready, retry same row
        else:
            vpad("press", "down", 400)
            time.sleep(args.settle)
    else:
        raise SystemExit("scenario wheel never selected mask %#x" % wheel_mask)
    snap("04-scenario-sel")
    want_map = A.MP_MAPS[args.map][0]
    # The map wheel commits its highlighted row on cross. Use a valid ID as
    # the pre-click value so a missed wheel cannot pass an invalid map to load.
    for attempt in range(len(A.MP_MAPS)):
        pine.write(WRITE32, A.MPSETTINGS + A.MPS_MAP, A.MP_MAPS[0][0])
        vpad("press", "cross", 400)
        time.sleep(args.page_settle)
        mp = pine.read32(A.MPSETTINGS + A.MPS_MAP)
        if mp == want_map:
            break
        print(f"  map select gave {mp:#x}, cycling", flush=True)
        vpad("press", "triangle", 400)
        time.sleep(args.page_settle)
        vpad("press", "down", 400)
        time.sleep(args.settle)
    else:
        raise SystemExit("map wheel never selected %#x" % want_map)
    snap("05-map-sel")
    vpad("press", "cross", 400)
    time.sleep(args.page_settle)
    snap("07-character")
    vpad("press", "cross", 400)
    time.sleep(args.page_settle)
    snap("08-handicap")
    vpad("press", "cross", 400)
    time.sleep(args.page_settle)
    snap("09-options")
    if args.bot_chars or args.bots == 0:
        vpad("press", "cross", 400)   # Continue; roster is poked or no bots requested
        time.sleep(args.page_settle)
    else:
        vpad("press", "down", 400)
        time.sleep(args.settle)
        vpad("press", "cross", 400)
        time.sleep(args.page_settle)
        snap("10-bots")
        for _ in range(args.bots):
            vpad("press", "cross", 400)
            time.sleep(args.page_settle)
            vpad("press", "cross", 400)
            time.sleep(args.page_settle)
            vpad("press", "cross", 400)
            time.sleep(args.page_settle)
            vpad("press", "down", 400)
            time.sleep(args.settle)
        snap("11-bots-done")
        vpad("press", "triangle", 400)
        time.sleep(args.page_settle)
        vpad("press", "up", 400)
        time.sleep(args.settle)
        vpad("press", "cross", 400)
        time.sleep(args.page_settle)
    snap("12-confirm")
    if args.direct_map_at_confirm:
        pine.write(WRITE32, A.MPSETTINGS + A.MPS_MAP, A.MP_MAPS[args.map][0])
        print("poked MP map at confirm", A.MP_MAPS[args.map], flush=True)
    if args.direct_scenario_at_confirm:
        pine.write(WRITE32, A.MPSETTINGS + A.MPS_SCENARIO_MASK, target_mask)
        print(f"poked MP scenario at confirm {target_mask:#x}", flush=True)
    poke_roster()   # idempotent re-poke guards menu clobbering
    if args.weapon_set is not None:
        pine.write(WRITE32, A.MPSETTINGS + A.MPS_WEAPON_SET, args.weapon_set)
        print("poked MP weapon set", args.weapon_set, flush=True)
    vpad("press", "cross", 400)
    # Confirm opens the pre-match roster page; start the match from there.
    time.sleep(args.page_settle)
    vpad("press", "cross", 400)
    live = False
    first_live_frame = None
    target_frame = None
    active_seen = False
    missed_frame = None
    deadline = time.monotonic() + args.live_timeout
    while time.monotonic() < deadline:
        time.sleep(0.002 if active_seen else 0.05)
        try:
            frame0, mp, player, done0, done1, frame1 = pine.read_ranges([
                (A.GS_FRAME_START, 4), (A.MPSETTINGS + A.MPS_MP_ACTIVE, 4),
                (A.GLB_PLAYERS, 4), (A.GS_DONE, 4), (A.GS_DONE, 4),
                (A.GS_FRAME_START, 4),
            ])
            frame0, mp, player, done0, done1, frame1 = (
                struct.unpack("<I", value)[0]
                for value in (frame0, mp, player, done0, done1, frame1))
            if frame0 != frame1 or done0 != done1:
                continue
            active_seen = mp == 1 and player != 0
            is_live = (active_seen and pine.read_block(
                player + A.OBJ_TYPE, 1)[0] == 3)
            if not is_live:
                first_live_frame = None
                target_frame = None
                continue
            if first_live_frame is None:
                first_live_frame = frame1
                target_frame = first_live_frame + args.spawn_stabilize
                print("first live player frame", first_live_frame,
                      "target frame", target_frame, flush=True)
            if frame1 < target_frame:
                continue
            if frame1 > target_frame:
                missed_frame = (target_frame, frame1)
                break
            live = True
            if args.time_limit_sec is not None:
                limit = struct.unpack("<I", struct.pack("<f", args.time_limit_sec))[0]
                pine.write(WRITE32, A.MPGAME + A.MPG_LIMIT, limit)
                print("poked live MP time limit (seconds)", args.time_limit_sec, flush=True)
            pine.save_state(args.slot)
            print("live:", live, "saved slot", args.slot, "at frame", frame1,
                  "(first live", first_live_frame, "+", args.spawn_stabilize, ")",
                  flush=True)
            break
        except Exception:
            pass
    if missed_frame:
        raise RuntimeError(
            f"missed exact spawn-stabilize frame {missed_frame[0]} "
            f"(sampled {missed_frame[1]})"
        )
    if not live:
        print("live:", live, flush=True)
    snap("13-spawn")
    pine.close()


if __name__ == "__main__":
    main()
