"""MP match setup driver: main menu (PINE slot 1) -> configured MP match via vpad.

Learned flow (2026-10-02, verified live):
  main menu -down-> Multiplayer -cross-> P_MPJOIN -cross x2-> scenario wheel
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
         [--settle 8] [--confirm-only]
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


def vpad(*words):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "nf-vpad.sock"))
    s.sendall((" ".join(map(str, words)) + "\n").encode())
    reply = s.recv(256)
    s.close()
    return reply
def hold(btn, secs=2.0):
    # The join page eats 400 ms taps; a 2 s hold registers reliably.
    import time as _t
    vpad("down", btn)
    _t.sleep(secs)
    return vpad("up", btn)


def shot(path):
    if not path:
        return
    os.system(f"WAYLAND_DISPLAY=wayland-0 spectacle -b -n -o {path} >/dev/null 2>&1")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", type=int, default=1, help="wheel index from Quick Game")
    ap.add_argument("--map", type=int, default=0, help="wheel index from Skyrail")
    ap.add_argument("--bots", type=int, default=3)
    ap.add_argument("--slot", type=int, default=12, help="PINE slot for the match-start state")
    ap.add_argument("--bot-chars", default="", help="e.g. 6,5,8: char ids poked into mpbots at confirm (skip wheel)")
    ap.add_argument("--bot-teams", default="", help="e.g. 0,1,0: teams poked alongside --bot-chars")
    ap.add_argument("--shots", default="", help="dir for per-page screenshots")
    ap.add_argument("--settle", type=float, default=8.0)
    ap.add_argument("--load-slot", type=int, default=1)
    args = ap.parse_args()

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
    time.sleep(12.0)   # cold menus eat the first inputs; 6 s proved flaky 1/3
    snap("01-main")

    vpad("press", "down", 400)
    time.sleep(args.settle)
    hold("cross")
    time.sleep(10.0)
    snap("02-join")
    hold("cross")
    time.sleep(args.settle)
    hold("cross")
    time.sleep(10.0)
    hold("cross")   # confirm ready -> scenario page (four holds total)
    time.sleep(10.0)

    snap("03-scenario")
    # Cycle-until-match: select, read what the wheel gave, step down on mismatch.
    # (Wheel position is unobservable; triangle-back preserves it. A Quick-Game
    # select auto-setups past the map page, so the map loop tolerates that too.)
    want_mask = A.MP_SCENARIOS[args.scenario][0]
    for attempt in range(14):
        pine.write(WRITE8, A.MENU_UNLOCK_EVERYTHING, 1)
        pine.write(WRITE32, A.MPSETTINGS + A.MPS_SCENARIO_MASK, 0xAAAAAAAA)
        vpad("press", "cross", 400)
        time.sleep(10.0)
        m = pine.read32(A.MPSETTINGS + A.MPS_SCENARIO_MASK)
        if m == want_mask:
            break
        print(f"  scenario select gave {m:#x}, cycling", flush=True)
        vpad("press", "triangle", 400)
        time.sleep(10.0)
        if m == 0xAAAAAAAA:
            time.sleep(10.0)   # select never fired: page not ready, retry same row
        else:
            vpad("press", "down", 400)
            time.sleep(args.settle)
    else:
        raise SystemExit("scenario wheel never selected mask %#x" % want_mask)
    snap("04-scenario-sel")
    want_map = A.MP_MAPS[args.map][0]
    for attempt in range(9):
        pine.write(WRITE32, A.MPSETTINGS + A.MPS_MAP, 0xBBBBBBBB)
        vpad("press", "cross", 400)
        time.sleep(10.0)
        mp = pine.read32(A.MPSETTINGS + A.MPS_MAP)
        if mp == want_map:
            break
        print(f"  map select gave {mp:#x}, cycling", flush=True)
        vpad("press", "triangle", 400)
        time.sleep(10.0)
        if mp == 0xBBBBBBBB:
            time.sleep(10.0)
        else:
            vpad("press", "down", 400)
            time.sleep(args.settle)
    else:
        raise SystemExit("map wheel never selected %#x" % want_map)
    snap("05-map-sel")
    def poke_roster():
        if not args.bot_chars:
            return
        chars = [int(x) for x in args.bot_chars.split(",")]
        teams = [int(x) for x in args.bot_teams.split(",")] if args.bot_teams else [0] * len(chars)
        assert len(chars) == len(teams) and len(chars) <= 4, "--bot-chars/_teams: up to 4 paired ids"
        assert set(teams) == {0, 1} or args.scenario in (0, 1), "team modes need both teams populated"
        stats = pine.read_block(0x26D2F0, 29 * 14)   # default_bot_stats
        pine.write(WRITE8, 0x2DEEA8, 1)
        pine.write(WRITE8, 0x2DEEA8 + 1, len(chars))
        for k, (ch, tm) in enumerate(zip(chars, teams)):
            base = 0x2DEEAA + k * 0x12
            row = stats[ch * 14:(ch + 1) * 14]
            for j in range(0, 14, 2):
                pine.write(WRITE16, base + j, struct.unpack_from("<H", row, j)[0])
            pine.write(WRITE8, base + 0x0E, 1)
            pine.write(WRITE8, base + 0x0F, tm)
            pine.write(WRITE8, base + 0x10, ch)
            pine.write(WRITE8, base + 0x11, 1)
        print("poked bot roster", list(zip(chars, teams)), flush=True)
    poke_roster()   # early: options/confirm pages see the intended teams+count
    vpad("press", "cross", 400)
    time.sleep(10.0)
    snap("07-character")
    vpad("press", "cross", 400)
    time.sleep(10.0)
    snap("08-handicap")
    vpad("press", "cross", 400)
    time.sleep(10.0)
    snap("09-options")
    if args.bot_chars:
        vpad("press", "cross", 400)   # Continue directly; roster already poked
        time.sleep(10.0)
    else:
        vpad("press", "down", 400)
        time.sleep(args.settle)
        vpad("press", "cross", 400)
        time.sleep(10.0)
        snap("10-bots")
        for _ in range(args.bots):
            vpad("press", "cross", 400)
            time.sleep(10.0)
            vpad("press", "cross", 400)
            time.sleep(10.0)
            vpad("press", "cross", 400)
            time.sleep(10.0)
            vpad("press", "down", 400)
            time.sleep(args.settle)
        snap("11-bots-done")
        vpad("press", "triangle", 400)
        time.sleep(10.0)
        vpad("press", "up", 400)
        time.sleep(args.settle)
        vpad("press", "cross", 400)
        time.sleep(10.0)
    snap("12-confirm")
    poke_roster()   # idempotent re-poke guards menu clobbering
    vpad("press", "cross", 400)
    # wait for the match to go live
    live = False
    for _ in range(30):
        time.sleep(8)
        try:
            mp = struct.unpack("<I", pine.read_block(A.MPSETTINGS + A.MPS_MP_ACTIVE, 4))[0]
            pl = struct.unpack("<I", pine.read_block(A.GLB_PLAYERS, 4))[0]
            if mp == 1 and pl != 0:
                live = True
                break
        except Exception:
            pass
    print("live:", live)
    snap("13-spawn")
    if live:
        time.sleep(5.0)
        pine.save_state(args.slot)
        print("saved slot", args.slot)
    pine.close()


if __name__ == "__main__":
    main()
