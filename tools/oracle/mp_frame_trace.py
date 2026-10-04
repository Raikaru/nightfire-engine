"""End-of-Game_Run state snapshots for phase-locked MP recording.

The static PCSX2 pnach hook copies recorder-selected EE ranges into a bounded
ring after Game_Run returns from logic updates. The recorder installs a compact
source/size table with PINE and drains only fully published snapshots.
"""

import struct
import time

from pine import WRITE32

ENTRY = 0x001C98BC
ENTRY_ORIGINAL = (0x03E00008, 0x27BD0010)  # jr ra; addiu sp,sp,16
CODE_BASE = 0x01FE6000
CONFIG_BASE = 0x01FE8000
MAX_DESCRIPTORS = 512
RING_BASE = 0x01FEA000
RING_END = 0x02000000
RING_HEADER_SIZE = 16
SNAPSHOT_HEADER_SIZE = 16
GS_DONE = 0x002A3798
GS_FRAME = 0x002A379C
GS_FRAME_START = 0x002A37A4


def _i(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def _r(rs, rt, rd, shamt, funct):
    return (rs << 21) | (rt << 16) | (rd << 11) | (shamt << 6) | funct


def _load32(a, reg, value):
    a.emit(_i(0x0F, 0, reg, value >> 16))
    a.emit(_i(0x0D, reg, reg, value))


class _Assembler:
    def __init__(self):
        self.words = []
        self.labels = {}
        self.branches = []

    def emit(self, word):
        self.words.append(word & 0xFFFFFFFF)

    def label(self, name):
        if name in self.labels:
            raise ValueError(f"duplicate assembly label {name}")
        self.labels[name] = len(self.words)

    def branch(self, op, rs, rt, label):
        self.branches.append((len(self.words), label))
        self.emit(_i(op, rs, rt, 0))
        self.emit(0)

    def finish(self):
        for at, name in self.branches:
            if name not in self.labels:
                raise ValueError(f"undefined assembly label {name}")
            offset = self.labels[name] - at - 1
            if not -0x8000 <= offset <= 0x7FFF:
                raise ValueError("branch target outside MIPS16 range")
            self.words[at] = (self.words[at] & 0xFFFF0000) | (offset & 0xFFFF)
        return self.words


def frame_hook_words():
    """Build the R5900 hook body; uses only caller-saved T registers."""
    a = _Assembler()
    _load32(a, 8, CONFIG_BASE)  # t0 = configuration
    a.emit(_i(0x23, 8, 9, 0))   # t1 = descriptor count
    a.branch(0x04, 9, 0, "exit")
    a.emit(_i(0x23, 8, 10, 4))  # t2 = payload bytes
    a.emit(_i(0x23, 8, 11, 8))  # t3 = ring capacity
    a.branch(0x04, 11, 0, "drop")
    a.emit(_i(0x23, 8, 12, 12)) # t4 = slot size
    a.emit(_i(0x09, 10, 13, SNAPSHOT_HEADER_SIZE))
    a.emit(_r(12, 13, 13, 0, 0x2B))  # sltu t5,slot_size,header+payload
    a.branch(0x05, 13, 0, "drop")
    _load32(a, 14, RING_BASE)  # t6 = ring header
    _load32(a, 25, GS_DONE)
    a.emit(_i(0x23, 25, 25, 0))
    a.emit(_i(0x23, 14, 13, 12))
    a.emit(_r(25, 13, 13, 0, 0x23))
    a.branch(0x04, 13, 0, "exit")
    a.emit(_i(0x23, 14, 15, 0)) # t7 = head
    a.emit(_i(0x23, 14, 24, 4)) # t8 = tail
    a.emit(_r(15, 24, 25, 0, 0x23)) # t9 = head-tail
    a.emit(_r(25, 11, 25, 0, 0x2B)) # t9 = available < capacity
    a.branch(0x04, 25, 0, "drop")
    a.emit(_i(0x09, 11, 25, -1)) # t9 = capacity-1
    a.emit(_r(15, 25, 25, 0, 0x24)) # t9 = head & (capacity-1)
    a.emit(_r(25, 12, 0, 0, 0x19)) # multu t9,slot_size
    a.emit(0)
    a.emit(0)
    a.emit(0)
    a.emit(0)
    a.emit(_r(0, 0, 25, 0, 0x12)) # mflo t9
    a.emit(_i(0x09, 14, 14, RING_HEADER_SIZE))
    a.emit(_r(14, 25, 14, 0, 0x21)) # t6 = slot address
    _load32(a, 25, GS_DONE)
    a.emit(_i(0x23, 25, 25, 0))
    a.emit(_i(0x2B, 14, 25, 0))
    _load32(a, 25, GS_FRAME_START)
    a.emit(_i(0x23, 25, 25, 0))
    a.emit(_i(0x2B, 14, 25, 4))
    _load32(a, 25, GS_FRAME)
    a.emit(_i(0x23, 25, 25, 0))
    a.emit(_i(0x2B, 14, 25, 8))
    a.emit(_i(0x2B, 14, 10, 12))
    a.emit(_i(0x09, 14, 15, SNAPSHOT_HEADER_SIZE)) # t7 = output cursor
    a.emit(_i(0x09, 8, 24, 16)) # t8 = descriptor table
    a.emit(_i(0x23, 8, 9, 0)) # t1 = descriptor count
    a.label("descriptor")
    a.branch(0x04, 9, 0, "publish")
    a.emit(_i(0x23, 24, 25, 0)) # t9 = source
    a.emit(_i(0x23, 24, 12, 4)) # t4 = byte length
    a.emit(_i(0x09, 24, 24, 8))
    a.label("byte")
    a.branch(0x04, 12, 0, "next_descriptor")
    a.emit(_i(0x24, 25, 13, 0)) # lbu t5,0(t9)
    a.emit(_i(0x28, 15, 13, 0)) # sb t5,0(t7)
    a.emit(_i(0x09, 25, 25, 1))
    a.emit(_i(0x09, 15, 15, 1))
    a.emit(_i(0x09, 12, 12, -1))
    a.branch(0x05, 12, 0, "byte")
    a.label("next_descriptor")
    a.emit(_i(0x09, 9, 9, -1))
    a.branch(0x05, 9, 0, "descriptor")
    a.label("publish")
    _load32(a, 14, RING_BASE)
    a.emit(_i(0x23, 14, 15, 0))
    a.emit(_i(0x09, 15, 15, 1))
    a.emit(_i(0x2B, 14, 15, 0))
    _load32(a, 25, GS_DONE)
    a.emit(_i(0x23, 25, 25, 0))
    a.emit(_i(0x2B, 14, 25, 12))
    a.branch(0x04, 0, 0, "exit")
    a.label("drop")
    _load32(a, 14, RING_BASE)
    a.emit(_i(0x23, 14, 15, 8))
    a.emit(_i(0x09, 15, 15, 1))
    a.emit(_i(0x2B, 14, 15, 8))
    a.label("exit")
    a.emit(0x03E00008)  # jr ra
    a.emit(0)
    return a.finish()


def pnach_lines():
    """Return static patch lines for ACTION.ELF's end-of-Game_Run hook."""
    # The target and CODE_BASE share the top four J-format address bits.
    jump = 0x08000000 | ((CODE_BASE >> 2) & 0x03FFFFFF)
    code = b"".join(struct.pack("<I", word) for word in frame_hook_words())
    lines = [f"patch=1,EE,{CODE_BASE:08X},bytes,{code.hex().upper()}"]
    lines += [f"patch=1,EE,{ENTRY:08X},word,{jump:08X}",
              f"patch=0,EE,{CONFIG_BASE:08X},word,00000000",
              f"patch=0,EE,{CONFIG_BASE+4:08X},word,00000000",
              f"patch=0,EE,{CONFIG_BASE+8:08X},word,00000000",
              f"patch=0,EE,{CONFIG_BASE+12:08X},word,00000000",
              f"patch=0,EE,{RING_BASE:08X},word,00000000",
              f"patch=0,EE,{RING_BASE+4:08X},word,00000000",
              f"patch=0,EE,{RING_BASE+8:08X},word,00000000",
              f"patch=0,EE,{RING_BASE+12:08X},word,00000000"]
    return lines


def _write_ops(pine, operations):
    pine._transact(b"".join(
        struct.pack("<BI", op, addr) + struct.pack(fmt, value)
        for op, addr, fmt, value in operations))


def jump_word():
    return 0x08000000 | ((CODE_BASE >> 2) & 0x03FFFFFF)


def verify(pine):
    expected_entry = struct.pack("<II", jump_word(), ENTRY_ORIGINAL[1])
    if pine.read_block(ENTRY, 8) != expected_entry:
        raise RuntimeError("Game_Run snapshot hook entry is not installed")
    expected_code = struct.pack("<" + "I" * len(frame_hook_words()), *frame_hook_words())
    if pine.read_block(CODE_BASE, len(expected_code)) != expected_code:
        raise RuntimeError("Game_Run snapshot hook code differs from the checked-in build")


def ensure(pine):
    """Attach to the pnach hook, restoring its code after a savestate load."""
    expected = struct.pack("<II", jump_word(), ENTRY_ORIGINAL[1])
    original = struct.pack("<II", *ENTRY_ORIGINAL)
    current = pine.read_block(ENTRY, 8)
    if current not in (original, expected):
        raise RuntimeError(
            f"cannot install Game_Run snapshot hook: entry bytes are {current.hex()}")
    words = frame_hook_words()
    code = struct.pack("<" + "I" * len(words), *words)
    if current == expected and pine.read_block(CODE_BASE, len(code)) == code:
        return
    ops = []
    if current == expected:
        # A loaded savestate can restore the code segment but leave the
        # per-frame pnach jump active. Disable sampling while rehydrating code.
        ops.append((WRITE32, CONFIG_BASE, "<I", 0))
    ops.extend((WRITE32, CODE_BASE + 4*i, "<I", word)
               for i, word in enumerate(words))
    if current == original:
        ops.append((WRITE32, ENTRY, "<I", jump_word()))
    for start in range(0, len(ops), 256):
        _write_ops(pine, ops[start:start + 256])
    verify(pine)



def configure(pine, ranges):
    """Install range descriptors and reset the ring; ranges are (EE addr,size)."""
    ensure(pine)
    if not ranges or len(ranges) > MAX_DESCRIPTORS:
        raise ValueError(f"frame hook needs 1..{MAX_DESCRIPTORS} ranges")
    payload_size = sum(size for _, size in ranges)
    slot_size = SNAPSHOT_HEADER_SIZE + payload_size
    ring_bytes = RING_END - RING_BASE - RING_HEADER_SIZE
    if slot_size > ring_bytes:
        raise ValueError(
            f"frame snapshot is {slot_size} bytes; ring holds {ring_bytes} bytes")
    capacity = 1
    while capacity * 2 * slot_size <= ring_bytes:
        capacity *= 2
    _write_ops(pine, [(WRITE32, CONFIG_BASE, "<I", 0),
                      (WRITE32, RING_BASE, "<I", 0),
                      (WRITE32, RING_BASE + 4, "<I", 0),
                      (WRITE32, RING_BASE + 8, "<I", 0),
                      (WRITE32, RING_BASE + 12, "<I", 0),
                      (WRITE32, CONFIG_BASE + 4, "<I", payload_size),
                      (WRITE32, CONFIG_BASE + 8, "<I", capacity),
                      (WRITE32, CONFIG_BASE + 12, "<I", slot_size)])
    descriptors = [(WRITE32, CONFIG_BASE + 16 + 8*i + off, "<I", value)
                   for i, (addr, size) in enumerate(ranges)
                   for off, value in ((0, addr), (4, size))]
    for start in range(0, len(descriptors), 256):
        _write_ops(pine, descriptors[start:start + 256])
    _write_ops(pine, [(WRITE32, CONFIG_BASE, "<I", len(ranges))])
    return {"capacity": capacity, "slot_size": slot_size,
            "payload_size": payload_size, "cursor": 0, "overflow": 0}


def read_next(pine, state, timeout=2.0):
    """Return (header, chunks) for the oldest published sample, or None."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        head, tail, overflow = struct.unpack("<III", pine.read_block(RING_BASE, 12))
        available = (head - tail) & 0xFFFFFFFF
        if available:
            if available > state["capacity"]:
                raise RuntimeError(
                    f"frame snapshot ring overrun: head={head} tail={tail} "
                    f"capacity={state['capacity']}")
            slot = RING_BASE + RING_HEADER_SIZE + (tail & (state["capacity"] - 1)) * state["slot_size"]
            raw = pine.read_block(slot, state["slot_size"])
            done, frame_start, timer_frame, payload_size = struct.unpack_from("<4I", raw)
            if payload_size != state["payload_size"]:
                raise RuntimeError(
                    f"frame snapshot size changed: {payload_size} != {state['payload_size']}")
            sample = raw[SNAPSHOT_HEADER_SIZE:SNAPSHOT_HEADER_SIZE + payload_size]
            chunks = []
            offset = 0
            for size in state["sizes"]:
                chunks.append(sample[offset:offset + size])
                offset += size
            if offset != payload_size:
                raise RuntimeError("frame snapshot descriptor length mismatch")
            pine.write(WRITE32, RING_BASE + 4, (tail + 1) & 0xFFFFFFFF)
            delta = (overflow - state["overflow"]) & 0xFFFFFFFF
            state["overflow"] = overflow
            return {"done": done, "frame": frame_start, "timer_frame": timer_frame,
                    "head": head, "tail": tail, "overflow_delta": delta}, chunks
        time.sleep(0.001)
    return None


def configure_ranges(pine, ranges):
    state = configure(pine, ranges)
    state["sizes"] = [size for _, size in ranges]
    return state
