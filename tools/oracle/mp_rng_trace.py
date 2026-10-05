"""Temporary EE entry trampolines for capturing original MP RNG call sites.

The hooks duplicate each function's first two instructions, then jump to the
original body. Events are written to a bounded EE RAM ring and consumed by the
PINE recorder; uninstall restores the four entry words.
"""

import time
import struct

from pine import WRITE32, WRITE64

RING_BASE = 0x01FE0000
RING_CAPACITY = 1024
RING_HEADER_SIZE = 16
CODE_BASE = 0x01FE5000

# (name, EE entry, event kind, result class, exact original first two words)
FUNCTIONS = (
    ("Rand_Random", 0x001E36F0, 1, "i", (0x8F828A30, 0x8F858A34)),
    ("Rand_Rand", 0x001E3738, 2, "i", (0x8F828A34, 0x8F858A3C)),
    ("Rand_FRand", 0x001E3780, 3, "f", (0x8F858A30, 0x8F848A34)),
    ("Rand_FRand_MVar2", 0x001E38B0, 4, "f", (0x8F858A30, 0x46006046)),
)

GS_FRAME = 0x002A379C


def _i(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def _r(rs, rt, rd, shamt, funct):
    return (rs << 21) | (rt << 16) | (rd << 11) | (shamt << 6) | funct


def _j(op, addr):
    return (op << 26) | ((addr >> 2) & 0x03FFFFFF)


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

    def finish(self):
        for at, name in self.branches:
            if name not in self.labels:
                raise ValueError(f"undefined assembly label {name}")
            offset = self.labels[name] - at - 1
            if not -0x8000 <= offset <= 0x7FFF:
                raise ValueError("branch target outside MIPS16 range")
            self.words[at] = (self.words[at] & 0xFFFF0000) | (offset & 0xFFFF)
        return self.words


def _trampoline(entry, kind, result_class, first_words, stub):
    a = _Assembler()
    # Preserve the caller's return address, then execute the instructions
    # displaced by the entry jump before calling the untouched function body.
    a.emit(_i(0x09, 29, 29, -16))       # addiu sp,sp,-16
    a.emit(_i(0x2B, 29, 31, 12))        # sw ra,12(sp)
    a.emit(first_words[0])
    a.emit(first_words[1])
    a.emit(_j(0x03, entry + 8))          # jal entry+8
    a.emit(0)                            # nop, original jal delay slot

    # Ring base and producer/consumer distance.
    a.emit(_i(0x0F, 0, 8, RING_BASE >> 16))
    a.emit(_i(0x0D, 8, 8, RING_BASE))
    a.emit(_i(0x23, 8, 9, 0))            # lw t1,0(t0): producer head
    a.emit(_i(0x23, 8, 10, 4))           # lw t2,4(t0): consumer tail
    a.emit(_r(9, 10, 11, 0, 0x23))       # subu t3,t1,t2
    a.emit(_i(0x0B, 11, 12, RING_CAPACITY))  # sltiu t4,t3,capacity
    a.branch(0x05, 12, 0, "not_full")   # bnez t4,not_full
    a.emit(0)
    a.emit(_i(0x23, 8, 11, 8))           # lw t3,8(t0): overflow count
    a.emit(_i(0x09, 11, 11, 1))          # addiu t3,t3,1
    a.emit(_i(0x2B, 8, 11, 8))           # sw t3,8(t0)
    a.label("not_full")

    # Slot is (head & (capacity-1))*16, after the 16-byte header.
    a.emit(_i(0x0C, 9, 10, RING_CAPACITY - 1))  # andi t2,t1,mask
    a.emit(_r(0, 10, 10, 4, 0x00))       # sll t2,t2,4
    a.emit(_r(8, 10, 10, 0, 0x21))       # addu t2,t0,t2
    a.emit(_i(0x09, 10, 10, RING_HEADER_SIZE))  # addiu t2,t2,header
    a.emit(_i(0x0F, 0, 11, GS_FRAME >> 16))
    a.emit(_i(0x0D, 11, 11, GS_FRAME))
    a.emit(_i(0x23, 11, 11, 0))          # lw t3,GS_FRAME
    a.emit(_i(0x2B, 10, 11, 0))          # sw t3,0(t2): frame
    a.emit(_i(0x23, 29, 11, 12))         # lw t3,12(sp): original caller ra
    a.emit(_i(0x2B, 10, 11, 4))          # sw t3,4(t2): caller ra
    if result_class == "i":
        a.emit(_i(0x2B, 10, 2, 8))       # sw v0,8(t2): result bits
    else:
        a.emit((0x11 << 26) | (11 << 16))  # mfc1 t3,f0
        a.emit(_i(0x2B, 10, 11, 8))      # sw t3,8(t2): float result bits
    a.emit(_i(0x09, 0, 11, kind))        # addiu t3,zero,kind
    a.emit(_i(0x2B, 10, 11, 12))         # sw t3,12(t2): kind
    a.emit(_i(0x09, 9, 9, 1))             # addiu t1,t1,1
    a.emit(_i(0x2B, 8, 9, 0))             # sw t1,0(t0): producer head
    a.emit(_i(0x23, 29, 31, 12))         # lw ra,12(sp)
    a.emit(0x03E00008)                    # jr ra
    a.emit(_i(0x09, 29, 29, 16))         # addiu sp,sp,16 (delay slot)
    words = a.finish()
    return stub, words


def _write_ops(pine, operations):
    import struct

    pine._transact(b"".join(
        struct.pack("<BI", op, addr) + struct.pack(fmt, value)
        for op, addr, fmt, value in operations
    ))


def install(pine):
    """Install all four hooks; return metadata for `read_events`/`uninstall`."""
    code_probe = pine.read_block(CODE_BASE, 0x400)
    ring_probe = pine.read_block(
        RING_BASE, RING_HEADER_SIZE + 16 * RING_CAPACITY)
    if any(code_probe) or any(ring_probe):
        raise RuntimeError(
            "refusing RNG hook: reserved RAM is not zero "
            f"(code nonzero={sum(bool(x) for x in code_probe)}, "
            f"ring nonzero={sum(bool(x) for x in ring_probe)})")
    patches = []
    code_ops = []
    functions = []
    for index, (name, entry, kind, result_class, first_words) in enumerate(FUNCTIONS):
        original = pine.read_block(entry, 8)
        expected = struct.pack("<II", *first_words)
        if original != expected:
            raise RuntimeError(
                f"{name} entry mismatch at {entry:#x}: got {original.hex()}, "
                f"expected {expected.hex()}; refusing to patch")
        stub = CODE_BASE + index * 0x100
        _, words = _trampoline(entry, kind, result_class, first_words, stub)
        for word_index, word in enumerate(words):
            code_ops.append((WRITE32, stub + 4 * word_index, "<I", word))
        patches.append((WRITE32, entry, "<I", _j(0x02, stub)))
        patches.append((WRITE32, entry + 4, "<I", 0))
        functions.append({"name": name, "entry": entry, "kind": kind,
                          "result": result_class, "stub": stub,
                          "original": original})

    # Initialize metadata and clear the complete ring before any entry can log.
    clear_ops = [(WRITE64, RING_BASE + offset, "<Q", 0)
                 for offset in range(0, RING_HEADER_SIZE + 16 * RING_CAPACITY, 8)]
    _write_ops(pine, clear_ops)
    _write_ops(pine, code_ops + patches)
    return {"functions": functions, "cursor": 0, "overflow": 0,
            "ring_base": RING_BASE, "capacity": RING_CAPACITY,
            "code_base": CODE_BASE, "installed": True}


def attach(pine):
    """Attach to pnach hooks and rehydrate trampoline code after savestate loads."""
    functions = []
    for index, (name, entry, kind, result_class, first_words) in enumerate(FUNCTIONS):
        stub = CODE_BASE + index * 0x100
        _, words = _trampoline(entry, kind, result_class, first_words, stub)
        expected_code = struct.pack("<" + "I" * len(words), *words)
        actual_entry = pine.read_block(entry, 8)
        expected_entry = struct.pack("<II", _j(0x02, stub), 0)
        if actual_entry != expected_entry:
            raise RuntimeError(
                f"{name} pnach entry mismatch at {entry:#x}: "
                f"got {actual_entry.hex()}, expected {expected_entry.hex()}")
        actual_code = pine.read_block(stub, len(expected_code))
        if actual_code != expected_code:
            # Startup-only code patches are overwritten by savestate restore,
            # while the per-frame entry jump remains active.
            _write_ops(pine, [
                (WRITE32, stub + 4*word_index, "<I", word)
                for word_index, word in enumerate(words)
            ])
            actual_code = pine.read_block(stub, len(expected_code))
            if actual_code != expected_code:
                raise RuntimeError(
                    f"{name} pnach trampoline restore failed at {stub:#x}")
        functions.append({"name": name, "entry": entry, "kind": kind,
                          "result": result_class, "stub": stub,
                          "original": struct.pack("<II", *first_words)})

    def counters():
        frame, ring = pine.read_ranges([(GS_FRAME, 4), (RING_BASE, 12)])
        head, _, overflow = struct.unpack("<III", ring)
        return struct.unpack("<I", frame)[0], head, overflow

    frame, head, overflow = counters()
    stable_frames = 0
    deadline = time.monotonic() + 5.0
    while stable_frames < 3 and time.monotonic() < deadline:
        time.sleep(0.02)
        next_frame, next_head, next_overflow = counters()
        deltas = ((next_frame - frame) & 0xFFFFFFFF,
                  (next_head - head) & 0xFFFFFFFF,
                  (next_overflow - overflow) & 0xFFFFFFFF)
        if 0 < deltas[0] < 0x80000000 and all(
                delta < 0x80000000 for delta in deltas[1:]):
            stable_frames += deltas[0]
        elif any(delta >= 0x80000000 for delta in deltas):
            # Ignore empty polls; only a counter rewind suggests a savestate
            # transition still in progress.
            stable_frames = 0
        frame, head, overflow = next_frame, next_head, next_overflow
    if stable_frames < 3:
        raise RuntimeError("RNG pnach counters did not settle after savestate load")

    _, head, overflow = counters()
    pine.write(WRITE32, RING_BASE + 4, head)
    return {"functions": functions, "cursor": head, "overflow": overflow,
            "ring_base": RING_BASE, "capacity": RING_CAPACITY,
            "code_base": CODE_BASE, "installed": True, "preinstalled": True}

def read_events(pine, state):
    """Return completed events since the last call, with loss accounting."""
    base, capacity = state["ring_base"], state["capacity"]
    head, tail, overflow = struct.unpack("<III", pine.read_block(base, 12))
    cursor = state["cursor"]
    available = (head - cursor) & 0xFFFFFFFF
    lost = max(0, available - capacity)
    if lost:
        cursor = (head - capacity) & 0xFFFFFFFF
        available = capacity
    events = []
    if available:
        start = cursor & (capacity - 1)
        first_count = min(available, capacity - start)
        chunks = [pine.read_block(base + RING_HEADER_SIZE + start * 16,
                                  first_count * 16)]
        if available > first_count:
            chunks.append(pine.read_block(base + RING_HEADER_SIZE,
                                          (available - first_count) * 16))
        data = b"".join(chunks)
        for offset in range(0, len(data), 16):
            frame, caller, result, kind = struct.unpack_from("<IIII", data, offset)
            function = next((f[0] for f in FUNCTIONS if f[2] == kind), None)
            events.append({"frame": frame, "kind": function or f"unknown:{kind}",
                           "caller_ra": caller, "callsite": (caller - 8) & 0xFFFFFFFF,
                           "result_u32": result, "result_hex": f"0x{result:08x}"})
    # A reread detects overwrites during the payload read. Keep the newest
    # capacity events; the caller records this gap rather than inventing rows.
    head_after = pine.read32(base)
    raced = max(0, ((head_after - cursor) & 0xFFFFFFFF) - capacity)
    lost += raced
    new_cursor = head
    pine.write(WRITE32, base + 4, new_cursor)
    delta_overflow = (overflow - state["overflow"]) & 0xFFFFFFFF
    state["cursor"] = new_cursor
    state["overflow"] = overflow
    return events, {"overflow_total": overflow, "overflow_delta": delta_overflow,
                    "lost_events": lost, "producer_head": head,
                    "consumer_tail": tail}


def uninstall(pine, state):
    """Restore entry words and clear the private code/ring scratch window."""
    if not state or not state.get("installed"):
        return
    operations = []
    for function in state["functions"]:
        first, second = struct.unpack("<II", function["original"])
        operations.append((WRITE32, function["entry"], "<I", first))
        operations.append((WRITE32, function["entry"] + 4, "<I", second))
    _write_ops(pine, operations)
    clear_ops = [
        (WRITE64, CODE_BASE + offset, "<Q", 0)
        for offset in range(0, 0x400, 8)
    ]
    clear_ops.extend(
        (WRITE64, RING_BASE + offset, "<Q", 0)
        for offset in range(0, RING_HEADER_SIZE + 16 * RING_CAPACITY, 8)
    )
    for start in range(0, len(clear_ops), 256):
        _write_ops(pine, clear_ops[start:start + 256])
    state["installed"] = False
