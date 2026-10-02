"""Minimal PINE (PCSX2 IPC) client: batched EE memory reads/writes.

Protocol: request = u32 total_len | (u8 opcode | args)*; reply = u32 total_len | u8 status | data*.
"""
import os
import socket
import struct

READ8, READ16, READ32, READ64 = 0, 1, 2, 3
WRITE8, WRITE16, WRITE32, WRITE64 = 4, 5, 6, 7
VERSION, SAVESTATE, LOADSTATE, TITLE, ID, UUID, GAMEVER, STATUS = 8, 9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF

_READ_SIZE = {READ8: 1, READ16: 2, READ32: 4, READ64: 8}
_WRITE_FMT = {WRITE8: "<B", WRITE16: "<H", WRITE32: "<I", WRITE64: "<Q"}
# PINE caps a batch well below this; keep requests comfortably inside it.
_MAX_BATCH = 4000


class PineError(RuntimeError):
    pass


class Pine:
    def __init__(self, slot: int = 28011):
        run = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
        path = f"{run}/pcsx2.sock" if slot == 28011 else f"{run}/pcsx2.sock.{slot}"
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)

    def close(self):
        self.sock.close()

    def _recv_exact(self, n: int) -> bytes:
        buf = bytearray()
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise PineError("socket closed")
            buf += chunk
        return bytes(buf)

    def _transact(self, body: bytes) -> bytes:
        self.sock.sendall(struct.pack("<I", len(body) + 4) + body)
        (size,) = struct.unpack("<I", self._recv_exact(4))
        reply = self._recv_exact(size - 4)
        if reply[0] != 0:
            raise PineError(f"PINE command failed (status {reply[0]:#x})")
        return reply[1:]

    def read_block(self, addr: int, size: int) -> bytes:
        """Read `size` bytes of EE memory using batched 64-bit reads."""
        out = bytearray()
        start, end = addr & ~7, addr + size
        ops = [(READ64, a) for a in range(start, end, 8)]
        for i in range(0, len(ops), _MAX_BATCH):
            batch = ops[i : i + _MAX_BATCH]
            data = self._transact(b"".join(struct.pack("<BI", op, a) for op, a in batch))
            out += data
        return bytes(out[addr - start : addr - start + size])

    def read_ranges(self, ranges):
        """Read several (addr, size) ranges in ONE PINE transaction, so they are as close to a
        single instant as the emulator thread allows. Returns a list of bytes."""
        ops = []
        spans = []
        for addr, size in ranges:
            start, end = addr & ~7, addr + size
            spans.append((len(ops), addr - start, size))
            ops += [(READ64, a) for a in range(start, end, 8)]
        if len(ops) > _MAX_BATCH:
            raise PineError("read_ranges: batch too large")
        data = self._transact(b"".join(struct.pack("<BI", op, a) for op, a in ops))
        return [data[i * 8 + skip : i * 8 + skip + size] for i, skip, size in spans]

    def read32(self, addr: int) -> int:
        return struct.unpack("<I", self._transact(struct.pack("<BI", READ32, addr)))[0]

    def write(self, op: int, addr: int, value: int):
        self._transact(struct.pack("<BI", op, addr) + struct.pack(_WRITE_FMT[op], value))

    def _string(self, op: int) -> str:
        data = self._transact(bytes([op]))
        (n,) = struct.unpack_from("<I", data)
        return data[4 : 4 + n].rstrip(b"\0").decode("latin1")

    def game_id(self) -> str:
        return self._string(ID)

    def status(self) -> int:
        """0 running, 1 paused, 2 shutdown."""
        return struct.unpack("<I", self._transact(bytes([STATUS])))[0]

    def save_state(self, slot: int):
        self._transact(bytes([SAVESTATE, slot]))

    def load_state(self, slot: int):
        self._transact(bytes([LOADSTATE, slot]))
