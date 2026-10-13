"""Frame codec shared with the C++ side (CLISystem FCLIFrameCodec, magic 'PUE\\x01').

    MAGIC[4] | type u8 | header_len u32 LE | header (UTF-8 JSON) | binary_len u32 LE | binary

EVENT/REPLY binaries are a binary table: count u32 | count x (len u32 | bytes). RAWLOG binaries are text.
STREAM binaries are the frame's raw data (inline streams), or empty (shared-memory streams).
"""
import json
import os
import struct
import threading

MAGIC = b"PUE\x01"
LOG, RAWLOG, EVENT, REPLY, CONTROL, STATUS, STREAM, ACK = 1, 2, 3, 4, 5, 6, 7, 8
MAX_FRAME_BYTES = 512 * 1024 * 1024

_HEAD = struct.Struct("<4sBI")
_U32 = struct.Struct("<I")


def encode(ftype, header, binary=b""):
    h = json.dumps(header, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return b"".join((_HEAD.pack(MAGIC, ftype, len(h)), h, _U32.pack(len(binary)), binary))


def build_table(buffers):
    parts = [_U32.pack(len(buffers))]
    for b in buffers:
        parts.append(_U32.pack(len(b)))
        parts.append(b)
    return b"".join(parts)


def parse_table(binary):
    """binary: bytearray/memoryview. Returns memoryview slices (no copies)."""
    view = memoryview(binary)
    if len(view) < 4:
        return []
    (count,) = _U32.unpack_from(view, 0)
    pos, out = 4, []
    for _ in range(count):
        (n,) = _U32.unpack_from(view, pos)
        pos += 4
        out.append(view[pos:pos + n])
        pos += n
    return out


class FrameWriter:
    """Thread-safe framed writer on a raw fd."""

    def __init__(self, fd):
        self.fd = fd
        self.lock = threading.Lock()
        self.closed = False

    def write(self, ftype, header, binary=b""):
        data = memoryview(encode(ftype, header, binary))
        with self.lock:
            if self.closed:
                return
            try:
                while data:
                    n = os.write(self.fd, data)
                    data = data[n:]
            except OSError:
                self.closed = True  # UE side is gone; the reader loop will see EOF and shut down


class FrameDecoder:
    """Incremental decoder. Each frame's binary is a fresh bytearray (writable, so numpy views over it are too)."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, chunk):
        self.buf += chunk
        frames, stray = [], bytearray()
        pos = 0
        n = len(self.buf)
        while pos < n:
            i = self.buf.find(MAGIC, pos)
            if i < 0:
                keep = 0
                for k in (3, 2, 1):
                    if n - pos >= k and self.buf[n - k:n] == MAGIC[:k]:
                        keep = k
                        break
                stray += self.buf[pos:n - keep]
                pos = n - keep
                break
            if i > pos:
                stray += self.buf[pos:i]
                pos = i
            if n - pos < 9:
                break
            _, ftype, hlen = _HEAD.unpack_from(self.buf, pos)
            if hlen + 13 > MAX_FRAME_BYTES:
                stray += self.buf[pos:pos + 1]
                pos += 1
                continue
            if n - pos < 9 + hlen + 4:
                break
            (blen,) = _U32.unpack_from(self.buf, pos + 9 + hlen)
            end = pos + 13 + hlen + blen
            if end - pos > MAX_FRAME_BYTES:
                stray += self.buf[pos:pos + 1]
                pos += 1
                continue
            if n < end:
                break
            header = json.loads(bytes(self.buf[pos + 9:pos + 9 + hlen]).decode("utf-8")) if hlen else {}
            frames.append((ftype, header, bytearray(self.buf[pos + 13 + hlen:end])))
            pos = end
        del self.buf[:pos]
        return frames, bytes(stray)
