"""Streams: render targets, audio and observations from Unreal, delivered as frames.

Unreal opens a stream for an owner (component) with a fixed number of slots. Each slot is a credit: Unreal
writes a slot (into a shared-memory region, or inline in the frame for small data) and sends a STREAM
frame; the slot is Unreal's again once Python sends its ACK, which happens when the handler returns (or
the iterator moves on). Frame arrays are zero-copy views of the slot, so copy what you keep.

Policies:
  latest    live data: Unreal drops new data while every slot is in use, and Python skips frames that
            a newer frame has already superseded (handlers always see the freshest data)
  queue     every frame is delivered in order; Unreal waits (bounded) for a free slot
  lockstep  Unreal waits for the handler's return value (the "action") before its game step continues
"""
import os
import queue
import threading

from . import _values

_release_lock = threading.Lock()


class _SharedRegion:
    """A named shared-memory region created by Unreal (FPlatformMemory::MapNamedSharedMemoryRegion)."""

    # Opened directly rather than with multiprocessing.shared_memory: importing that costs ~55 ms (stalling
    # the frames behind the first open), and on POSIX 3.12 its resource tracker would adopt (and later
    # unlink) Unreal's segment and spawn a helper process.
    def __init__(self, name, size):
        import mmap
        self.name = name
        if os.name == "nt":
            import _winapi
            # Hold a handle to Unreal's mapping while mapping it by name: mmap with a tagname alone would
            # silently create a new, empty mapping if Unreal's were gone
            handle = _winapi.OpenFileMapping(_winapi.FILE_MAP_WRITE, False, name)
            try:
                self._mm = mmap.mmap(-1, size, tagname=name)
            finally:
                _winapi.CloseHandle(handle)
        else:
            import _posixshmem
            fd = _posixshmem.shm_open("/" + name, os.O_RDWR, mode=0o600)  # Unreal maps "/" + name too
            try:
                self._mm = mmap.mmap(fd, size)
            finally:
                os.close(fd)
        self.buf = memoryview(self._mm)

    def close(self):
        """False while frame arrays still reference the region (try again later)."""
        try:
            self.buf.release()
            self._mm.close()
            return True
        except BufferError:
            return False


class StreamState:
    """One open stream (reader thread creates it; workers read it)."""

    def __init__(self, owner, script, name, header):
        self.owner = owner
        self.script = script
        self.name = name
        self.policy = header.get("policy", "latest")
        self.slots = int(header.get("slots", 1))
        self.slot_bytes = int(header.get("slot_bytes", 0))
        self.info = header.get("info") or {}
        self.region = _SharedRegion(header["shm"], self.slots * self.slot_bytes) if header.get("shm") else None
        self.latest_seq = -1
        self.received = 0
        self.skipped = 0
        self.closed = False

    def close(self):
        self.closed = True
        return self.region is None or self.region.close()


class StreamFrame:
    """One frame of a stream.

    array   numpy array (or unreal_bridge.Tensor without numpy), a view of Unreal's slot: valid until the
            handler returns / the iterator advances. Use .copy() to keep it.
    seq     Unreal's frame counter for this stream (gaps mean Unreal dropped frames: policy 'latest')
    time    Unreal FPlatformTime::Seconds() when the data was captured
    frame   Unreal's GFrameCounter at capture
    layout  channel layout for images ("BGRA", "RGBA", "R", ...), else ""
    meta    per-frame values from Unreal (sample_rate, rows, ...); info: the stream's fixed description
    """

    __slots__ = ("stream", "seq", "time", "frame", "slot", "dtype", "shape", "layout", "meta", "array",
                 "_host", "_sid", "_released", "_replied")

    def __init__(self, host, stream, header, binary):
        self._host = host
        self.stream = stream
        self.seq = header.get("seq", 0)
        self.time = header.get("t", 0.0)
        self.frame = header.get("frame", 0)
        self.slot = header.get("slot", 0)
        self.dtype = header.get("dtype", "uint8")
        self.shape = tuple(header.get("shape") or ())
        self.layout = header.get("layout", "")
        self.meta = header.get("meta") or {}
        self._sid = header.get("sid")
        self._released = False
        self._replied = False
        nbytes = int(header.get("nbytes", 0))
        if stream.region is not None:
            off = self.slot * stream.slot_bytes
            data = stream.region.buf[off:off + nbytes]
        else:
            data = memoryview(binary)[:nbytes] if binary else memoryview(b"")
        self.array = _values.make_tensor(data, self.dtype, list(self.shape) if self.shape else None)

    @property
    def name(self):
        return self.stream.name

    @property
    def info(self):
        return self.stream.info

    @property
    def lockstep(self):
        return self._sid is not None

    def rgb(self):
        """Image frames: the colour channels as RGB (a view for numpy arrays; channel order from layout)."""
        a = self.array
        if self.layout == "BGRA":
            return a[..., 2::-1]
        if self.layout in ("RGBA", "RGB"):
            return a[..., :3]
        return a

    def copy(self):
        """A copy that stays valid after the frame is released."""
        a = self.array
        if hasattr(a, "copy"):
            return a.copy()
        return _values.Tensor(bytes(a.data), a.dtype, a.shape)

    def rows(self):
        """Observation frames: a list of {feature: value} dicts, one per row."""
        names = self.stream.info.get("features") or []
        values = self.array.tolist()
        if values and not isinstance(values[0], list):  # ue.Tensor.tolist() is flat
            width = len(names) or 1
            values = [values[i:i + width] for i in range(0, len(values), width)]
        return [dict(zip(names, row)) for row in values]

    def reply(self, value):
        """Lockstep frames: send the step's result (handlers can just return it instead). This also releases
        the frame, since Unreal writes its next step into the slot."""
        if self._sid is not None:
            self._finish(True, value)

    def fail(self, error, tb=None):
        if self._sid is not None:
            self._finish(False, error=error, tb=tb)

    def release(self):
        """Give the slot back to Unreal (automatic when a handler returns or the iterator moves on). An
        unanswered lockstep frame answers None."""
        self._finish(True, None)

    def _finish(self, ok, value=None, error=None, tb=None):
        with _release_lock:
            ack = not self._released
            answer = self._sid is not None and not self._replied
            self._released = True
            self._replied = True
        if ack:  # slot first: when Unreal's step returns, its next step finds the slot free
            self.array = None
            self._host.stream_ack(self)
        if answer:
            self._host.stream_reply(self, ok, value, error=error, traceback=tb)

    def __repr__(self):
        return f"StreamFrame({self.stream.name!r}, seq={self.seq}, dtype={self.dtype}, shape={self.shape}{', ' + self.layout if self.layout else ''})"


class StreamReader:
    """Frames for `for frame in ue.stream(name)` (instead of @ue.on_stream handlers)."""

    def __init__(self, name):
        self.name = name
        self.queue = queue.SimpleQueue()

    def put(self, frame):
        self.queue.put(frame)

    def drain(self):
        while True:
            try:
                self.queue.get_nowait().release()
            except queue.Empty:
                return


def iterate(ctx, name, timeout=None):
    """Generator behind ue.stream(): yields frames until the script stops (or `timeout` s pass without one)."""
    reader = ctx.add_reader(name)
    current = None
    waited = 0.0
    try:
        while not ctx.stopping.is_set():
            try:
                frame = reader.queue.get(timeout=0.1)
            except queue.Empty:
                waited += 0.1
                if timeout is not None and waited >= timeout:
                    return
                continue
            waited = 0.0
            if frame.stream.policy == "latest":  # skip to the newest frame that's already here
                while True:
                    try:
                        newer = reader.queue.get_nowait()
                    except queue.Empty:
                        break
                    frame.stream.skipped += 1
                    frame.release()
                    frame = newer
            if current is not None:
                current.release()
            current = frame
            yield frame
    finally:
        if current is not None:
            current.release()
        ctx.remove_reader(name, reader)
