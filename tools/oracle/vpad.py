"""Virtual Xbox 360 pad (uinput) driven over a unix socket, so PCSX2 gets
input via SDL regardless of window focus.

Run:   python3 vpad.py            (holds the device; serves $XDG_RUNTIME_DIR/nf-vpad.sock)
Client lines (one per command, replies "ok" / "err <msg>"):
    press <button> [ms]      tap a button (default 120 ms)
    down <button> | up <button>
    axis <LX|LY|RX|RY> <-1.0..1.0>
    trig <LT|RT> <0.0..1.0>
    release                   neutral state
Buttons: cross circle square triangle start select l1 r1 l3 r3 up down left right
"""
import os
import socket
import threading
import time

from evdev import AbsInfo, UInput, ecodes as e

BUTTONS = {
    "cross": e.BTN_SOUTH, "circle": e.BTN_EAST, "square": e.BTN_WEST, "triangle": e.BTN_NORTH,
    "start": e.BTN_START, "select": e.BTN_SELECT, "l1": e.BTN_TL, "r1": e.BTN_TR,
    "l3": e.BTN_THUMBL, "r3": e.BTN_THUMBR,
}
DPAD = {"up": (e.ABS_HAT0Y, -1), "down": (e.ABS_HAT0Y, 1), "left": (e.ABS_HAT0X, -1), "right": (e.ABS_HAT0X, 1)}
STICKS = {"LX": e.ABS_X, "LY": e.ABS_Y, "RX": e.ABS_RX, "RY": e.ABS_RY}
TRIGGERS = {"LT": e.ABS_Z, "RT": e.ABS_RZ}

STICK = AbsInfo(0, -32768, 32767, 16, 128, 0)
TRIG = AbsInfo(0, 0, 255, 0, 0, 0)
HAT = AbsInfo(0, -1, 1, 0, 0, 0)

CAPS = {
    e.EV_KEY: list(BUTTONS.values()) + [e.BTN_MODE],
    e.EV_ABS: [(c, STICK) for c in STICKS.values()]
    + [(c, TRIG) for c in TRIGGERS.values()]
    + [(e.ABS_HAT0X, HAT), (e.ABS_HAT0Y, HAT)],
}


class Pad:
    def __init__(self):
        # Xbox 360 VID/PID so SDL applies its built-in gamepad mapping.
        self.ui = UInput(CAPS, name="Microsoft X-Box 360 pad", vendor=0x045E, product=0x028E, version=0x110, bustype=e.BUS_USB)
        self.lock = threading.Lock()

    def _emit(self, typ, code, value):
        with self.lock:
            self.ui.write(typ, code, value)
            self.ui.syn()

    def button(self, name, pressed):
        if name in DPAD:
            code, v = DPAD[name]
            self._emit(e.EV_ABS, code, v if pressed else 0)
        else:
            self._emit(e.EV_KEY, BUTTONS[name], 1 if pressed else 0)

    def axis(self, name, value):
        self._emit(e.EV_ABS, STICKS[name], max(-32768, min(32767, int(value * 32767))))

    def trigger(self, name, value):
        self._emit(e.EV_ABS, TRIGGERS[name], max(0, min(255, int(value * 255))))

    def release(self):
        for b in BUTTONS:
            self.button(b, False)
        for d in ("up", "left"):
            self.button(d, False)
        for s in STICKS:
            self.axis(s, 0.0)
        for t in TRIGGERS:
            self.trigger(t, 0.0)


def handle(pad, line):
    parts = line.split()
    cmd, args = parts[0], parts[1:]
    if cmd == "press":
        pad.button(args[0], True)
        time.sleep((int(args[1]) if len(args) > 1 else 120) / 1000)
        pad.button(args[0], False)
    elif cmd in ("down", "up") and args:
        pad.button(args[0], cmd == "down")
    elif cmd == "axis":
        pad.axis(args[0], float(args[1]))
    elif cmd == "trig":
        pad.trigger(args[0], float(args[1]))
    elif cmd == "release":
        pad.release()
    else:
        raise ValueError(f"unknown command: {line}")


def serve(pad, path):
    if os.path.exists(path):
        os.unlink(path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(4)
    print(f"vpad ready on {path}", flush=True)

    def client(conn):
        with conn, conn.makefile("rw") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    handle(pad, line)
                    f.write("ok\n")
                except Exception as ex:  # report to client, keep serving
                    f.write(f"err {ex}\n")
                f.flush()

    while True:
        conn, _ = srv.accept()
        threading.Thread(target=client, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    serve(Pad(), os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "nf-vpad.sock"))
