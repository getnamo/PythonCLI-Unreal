"""Engine-free protocol tests: drives runtime.py the way Unreal does (framed stdio, stderr merged into stdout
like CLISystem) and checks the bridge's behavior.

    ThirdParty/python/Win64/python.exe -I Content/Python/tests/harness.py [--site <venv site-packages>]
    ThirdParty/python/Linux/bin/python3.12 -I Content/Python/tests/harness.py
"""
import json
import os
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
PYDIR = os.path.dirname(HERE)
sys.path.insert(0, PYDIR)
from unreal_bridge import _frames as F  # noqa: E402

FIXTURE = os.path.join(HERE, "scripts", "fixture.py")
BROKEN = os.path.join(HERE, "scripts", "broken.py")
STREAMS = os.path.join(HERE, "scripts", "streams.py")
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'} - {name}{(': ' + str(detail)) if detail and not ok else ''}", flush=True)


class Bridge:
    def __init__(self, site_dirs=(), preload=()):
        cfg = {"site_dirs": list(site_dirs), "env": {"PYTHONCLI_TEST": "1"}, "preload": list(preload),
               "project_dir": PYDIR, "content_dir": PYDIR}
        fd, self.cfg_path = tempfile.mkstemp(suffix=".json")
        with os.fdopen(fd, "w") as f:
            json.dump(cfg, f)
        self.p = subprocess.Popen([sys.executable, "-I", "-u", "-X", "utf8", os.path.join(PYDIR, "runtime.py"), "--config", self.cfg_path],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        self.frames, self.stray = [], bytearray()
        self.acks = {}  # (owner, stream) -> [slot, ...] not yet taken by a Producer
        self.sids = {}  # lockstep step id -> REPLY header
        self.cv = threading.Condition()
        self.next_id = 0
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        dec = F.FrameDecoder()
        while True:
            chunk = self.p.stdout.read(1 << 16)
            if not chunk:
                with self.cv:
                    self.stray += dec.buf
                    self.cv.notify_all()
                return
            frames, stray = dec.feed(chunk)
            with self.cv:
                for f in frames:
                    if f[0] == F.ACK:  # stream credits, kept apart (there are many)
                        self.acks.setdefault((f[1]["owner"], f[1]["stream"]), []).append(f[1]["slot"])
                    elif f[0] == F.REPLY and "sid" in f[1]:
                        self.sids[f[1]["sid"]] = f[1]
                    else:
                        self.frames.append(f)
                self.stray += stray
                self.cv.notify_all()

    def send(self, ftype, header, binary=b""):
        self.p.stdin.write(F.encode(ftype, header, binary))
        self.p.stdin.flush()

    def control(self, **h):
        self.send(F.CONTROL, h)

    def event(self, owner, script, name, args, buffers=(), call=False):
        h = {"owner": owner, "script": script, "name": name, "args": args}
        if call:
            self.next_id += 1
            h["id"] = self.next_id
        self.send(F.EVENT, h, F.build_table(list(buffers)) if buffers else b"")
        return h.get("id")

    def wait(self, pred, timeout=10.0):
        end = time.time() + timeout
        with self.cv:
            while True:
                for f in self.frames:
                    if pred(*f):
                        return f
                left = end - time.time()
                if left <= 0:
                    return None
                self.cv.wait(left)

    def status(self, status, owner=None, script=None, timeout=10.0):
        return self.wait(lambda t, h, b: t == F.STATUS and h.get("status") == status
                         and (owner is None or h.get("owner") == owner) and (script is None or h.get("script") == script), timeout)

    def ev(self, name, owner=None, timeout=10.0, after=0):
        return self.wait(lambda t, h, b: t == F.EVENT and h.get("name") == name and (owner is None or h.get("owner") == owner), timeout)

    def reply(self, call_id, timeout=10.0):
        return self.wait(lambda t, h, b: t == F.REPLY and h.get("id") == call_id, timeout)

    def call(self, owner, script, name, args, buffers=(), timeout=10.0):
        return self.reply(self.event(owner, script, name, args, buffers, call=True), timeout)

    def clear(self):
        with self.cv:
            self.frames.clear()

    def close(self):
        try:
            os.unlink(self.cfg_path)
        except OSError:
            pass


def table_entries(binary):
    return [bytes(m) for m in F.parse_table(binary)] if binary else []


class Producer:
    """Plays Unreal's side of a stream: one credit per slot, data in shared memory (created here, as Unreal
    creates it) or inline in the frame."""

    def __init__(self, bridge, owner, script, name, slots, slot_bytes, policy="queue", shm=True, info=None):
        self.b, self.owner, self.name = bridge, owner, name
        self.slots, self.slot_bytes, self.policy = slots, slot_bytes, policy
        self.free = list(range(slots))
        self.seq = 0
        self.dropped = 0
        self.region = None
        shm_name = None
        if shm:
            from multiprocessing import shared_memory
            shm_name = "{%s}" % str(uuid.uuid4()).upper()
            self.region = shared_memory.SharedMemory(name=shm_name, create=True, size=slots * slot_bytes)
        bridge.control(cmd="openStream", owner=owner, script=script, stream=name, slots=slots, slot_bytes=slot_bytes,
                       policy=policy, shm=shm_name, info=info or {})

    def _collect(self):
        acked = self.b.acks.pop((self.owner, self.name), [])
        self.free.extend(acked)

    def acquire(self, timeout=10.0):
        """A free slot, or None: 'latest' doesn't wait (the frame is dropped); others wait up to timeout."""
        end = time.time() + timeout
        with self.b.cv:
            while True:
                self._collect()
                if self.free:
                    return self.free.pop(0)
                if self.policy == "latest" or time.time() >= end:
                    self.dropped += 1
                    return None
                self.b.cv.wait(end - time.time())

    def push(self, data, dtype, shape, layout="", meta=None, sid=None, timeout=10.0):
        slot = self.acquire(timeout)
        if slot is None:
            return None
        h = {"owner": self.owner, "stream": self.name, "seq": self.seq, "slot": slot, "t": time.perf_counter(), "frame": self.seq,
             "dtype": dtype, "shape": list(shape), "nbytes": len(data), "layout": layout, "meta": meta or {}}
        if sid is not None:
            h["sid"] = sid
        binary = b""
        if self.region is not None:
            off = slot * self.slot_bytes
            self.region.buf[off:off + len(data)] = data
        else:
            binary = data
        self.b.send(F.STREAM, h, binary)
        self.seq += 1
        return h["seq"]

    def step(self, data, dtype, shape, sid, timeout=10.0):
        """Lockstep: push and wait for the handler's result."""
        if self.push(data, dtype, shape, sid=sid, timeout=timeout) is None:
            return None
        end = time.time() + timeout
        with self.b.cv:
            while sid not in self.b.sids:
                left = end - time.time()
                if left <= 0:
                    return None
                self.b.cv.wait(left)
            return self.b.sids.pop(sid)

    def drain(self, timeout=10.0):
        """Wait until every slot is back."""
        end = time.time() + timeout
        with self.b.cv:
            while True:
                self._collect()
                if len(self.free) == self.slots or time.time() >= end:
                    return len(self.free) == self.slots
                self.b.cv.wait(max(0.0, end - time.time()))

    def close(self):
        self.b.control(cmd="closeStream", owner=self.owner, stream=self.name)
        if self.region is not None:
            self.region.close()
            try:
                self.region.unlink()
            except FileNotFoundError:
                pass


def floats(values):
    return struct.pack(f"<{len(values)}f", *values)


def stream_tests(b, numpy):
    S = "streams.py"
    b.control(cmd="start", owner="s1", script=S, path=STREAMS)
    check("stream fixture started", b.status("started", "s1", S) is not None)

    # shared-memory frames, queue policy: every frame delivered in order, contents intact, all credits back
    n, slot_bytes = 200, 1 << 20
    p = Producer(b, "s1", S, "frames", 4, slot_bytes, policy="queue")
    t0 = time.time()
    for i in range(n):
        p.push(struct.pack("<I", i) * (slot_bytes // 4), "uint32", [slot_bytes // 4])
    ok_back = p.drain()
    dt = time.time() - t0
    r = b.call("s1", S, "get_stats", ["frames"])
    st = r[1]["result"] if r else {}
    check("shm stream: every frame delivered in order, contents intact", st.get("frames") == n and st.get("bad") == 0 and st.get("out_of_order") == 0, st and {k: st[k] for k in ("frames", "bad", "out_of_order")})
    check("shm stream: every slot acked back", ok_back)
    print(f"     shm stream: {n / dt:.0f} frames/s of 1 MB ({n * slot_bytes / dt / 1e9:.2f} GB/s incl. producer fill + verify)")

    # latest policy: a slow handler sees only the freshest frames, in order, and the newest one always arrives
    b.call("s1", S, "set_sleep", [0.02])
    b.call("s1", S, "reset", [])
    p.close()
    p = Producer(b, "s1", S, "frames", 3, 4096, policy="latest")
    sent = []
    for i in range(60):
        seq = p.push(struct.pack("<I", p.seq) * 1024, "uint32", [1024])
        if seq is not None:
            sent.append(seq)
        time.sleep(0.002)
    p.drain()
    b.call("s1", S, "set_sleep", [0.0])
    r = b.call("s1", S, "get_stats", ["frames"])
    st = r[1]["result"] if r else {}
    seqs = st.get("seqs", [])
    check("latest: dropped and skipped frames under load", p.dropped > 0 and len(seqs) < len(sent), (p.dropped, len(seqs), len(sent)))
    check("latest: delivered frames are in order and the newest arrived", seqs == sorted(seqs) and seqs and seqs[-1] == sent[-1], (seqs[-3:], sent[-1:]))
    p.close()

    # inline observations: (rows, features) float32 with feature names
    feats = ["a", "b", "c"]
    p = Producer(b, "s1", S, "obs", 2, 4 * 3 * 4, policy="queue", shm=False, info={"features": feats})
    total = 0.0
    for i in range(50):
        rows = [float(i), 1.0, -0.5] * 4
        total += sum(rows)
        p.push(floats(rows), "float32", [4, 3])
    p.drain()
    r = b.call("s1", S, "get_stats", ["obs"])
    st = r[1]["result"] if r else {}
    check("inline observation rows arrive with feature names", st.get("rows") == 200 and st.get("features") == feats and abs(st.get("sum", 0) - total) < 1e-3
          and st.get("last_row") == {"a": 49.0, "b": 1.0, "c": -0.5}, st and {k: st.get(k) for k in ("rows", "features", "sum", "last_row")})
    p.close()

    # lockstep: deterministic step() round trips; the stream survives a script reload
    def run_episode(steps=200):
        p = Producer(b, "s1", S, "step", 1, 12, policy="lockstep", shm=False, info={"features": ["step", "x", "v"]})
        x, v, dt = 1.0, 0.0, 1.0 / 60.0
        trajectory, rtt, mismatched = [], [], 0
        for k in range(steps):
            t = time.perf_counter()
            r = p.step(floats([k, x, v]), "float32", [1, 3], sid=k)
            rtt.append((time.perf_counter() - t) * 1e6)
            if r is None or not r.get("ok"):
                return None, rtt, r
            action = r["result"]
            mismatched += action["step"] != k
            v += action["force"] * dt
            x += v * dt
            trajectory.append((x, v))
        p.close()
        return trajectory, rtt, mismatched

    b.call("s1", S, "reset", [777])
    run1, rtt, mism = run_episode()
    check("lockstep: 200 steps, each action answers its own step", run1 is not None and mism == 0, mism)
    b.control(cmd="reload", owner="s1", script=S)
    b.status("started", "s1", S)
    b.call("s1", S, "reset", [777])
    run2, _, _ = run_episode()
    check("lockstep: identical trajectory after a script reload (deterministic)", run1 is not None and run1 == run2, (run1 and run1[-1], run2 and run2[-1]))
    rtt.sort()
    if rtt:
        print(f"     lockstep step round trip: p50 {rtt[len(rtt) // 2]:.0f} us, p99 {rtt[int(len(rtt) * 0.99)]:.0f} us (engine-free)")

    # iterator API
    b.call("s1", S, "start_iterating", ["iter", 3.0])
    time.sleep(0.1)
    p = Producer(b, "s1", S, "iter", 2, 64, policy="queue", shm=True)
    for i in range(20):
        p.push(bytes(64), "uint8", [64])
    check("ue.stream() iterator: slots come back", p.drain())
    r = b.call("s1", S, "get_stats", ["iter"])
    st = r[1]["result"] if r else {}
    check("ue.stream() iterator receives every frame in order", st.get("frames") == 20 and st.get("out_of_order") == 0, st and {k: st.get(k) for k in ("frames", "out_of_order")})
    p.close()

    # errors: lockstep without a handler; frames for a closed stream still hand their slot back
    p = Producer(b, "s1", S, "nobody", 1, 4, policy="lockstep", shm=False)
    r = p.step(floats([1.0]), "float32", [1], sid=10001)
    check("lockstep step without a handler is an error reply", r is not None and not r["ok"] and "on_stream" in r.get("error", ""), r)
    p.close()
    with b.cv:
        b.acks.pop(("s1", "nobody"), None)
    b.send(F.STREAM, {"owner": "s1", "stream": "nobody", "seq": 5, "slot": 0, "dtype": "float32", "shape": [1], "nbytes": 4, "sid": 10002}, floats([1.0]))
    with b.cv:
        b.cv.wait_for(lambda: 10002 in b.sids, 5)
        r = b.sids.pop(10002, None)
        acked = b.acks.pop(("s1", "nobody"), [])
    check("a frame for a closed stream is acked and its step fails", r is not None and not r["ok"] and acked == [0], (r, acked))

    b.control(cmd="stopOwner", owner="s1")
    b.status("stopped", "s1", S)


def main():
    site = []
    if "--site" in sys.argv:
        site = [sys.argv[sys.argv.index("--site") + 1]]

    b = Bridge(site_dirs=site)
    ready = b.status("ready", timeout=20)
    check("ready status", ready is not None)
    if ready is None:
        print(b.stray.decode(errors="replace"))
        sys.exit(1)
    numpy = ready[1].get("numpy")
    print(f"     python {ready[1].get('python')}, numpy {numpy}, startup {ready[1].get('startup_ms')} ms")

    # start + on_start + args
    b.control(cmd="start", owner="o1", script="fixture.py", path=FIXTURE, args=["a", "b"])
    check("script started", b.status("started", "o1", "fixture.py") is not None)
    f = b.ev("started_with", "o1")
    check("on_start ran with args", f is not None and f[1]["args"] == [["a", "b"]], f and f[1])

    # events + calls
    b.event("o1", "fixture.py", "ping", [1, "two", {"three": 3}])
    f = b.ev("pong", "o1")
    check("event handler round trip", f is not None and f[1]["args"] == [1, "two", {"three": 3}], f and f[1])
    r = b.call("o1", "fixture.py", "add", [2, 3])
    check("call returns result", r is not None and r[1]["ok"] and r[1]["result"] == 5, r and r[1])
    r = b.call("o1", "fixture.py", "divide", [1, 0])
    check("call error has traceback", r is not None and not r[1]["ok"] and "ZeroDivisionError" in r[1].get("traceback", ""), r and r[1])
    r = b.call("o1", "fixture.py", "nope", [])
    check("missing function is an error reply", r is not None and not r[1]["ok"] and "no function" in r[1]["error"], r and r[1])
    r = b.call("o1", "missing.py", "add", [1, 2])
    check("call to a script that isn't running is an error reply", r is not None and not r[1]["ok"], r and r[1])

    # binary + tensors
    blob = os.urandom(1 << 20)
    r = b.call("o1", "fixture.py", "echo", [{"_bin": 0}], [blob])
    got = table_entries(r[2]) if r else []
    check("1 MB binary round trip", r is not None and r[1]["ok"] and r[1]["result"] == [{"_bin": 0}] and got == [blob], r and r[1])
    values = [1.5, -2.0, 3.25, 4.0, 0.5, 6.0]
    r = b.call("o1", "fixture.py", "double", [{"_tensor": 0, "dtype": "float32", "shape": [2, 3]}], [struct.pack("<6f", *values)])
    ok = r is not None and r[1]["ok"]
    if ok:
        desc = r[1]["result"]
        out = struct.unpack("<6f", table_entries(r[2])[desc["_tensor"]])
        ok = desc["dtype"] == "float32" and desc["shape"] == [2, 3] and list(out) == [v * 2 for v in values]
    check(f"tensor round trip ({'numpy' if numpy else 'ue.Tensor'})", ok, r and r[1])

    # output attribution
    b.clear()
    b.call("o1", "fixture.py", "say", ["hi"])
    log = b.wait(lambda t, h, x: t == F.LOG and "printed: hi" in h.get("msg", ""))
    check("print() becomes a LOG frame attributed to the script", log is not None and log[1].get("owner") == "o1" and log[1].get("script") == "fixture.py", log and log[1])
    raw = b.wait(lambda t, h, x: t == F.RAWLOG and b"native: hi" in x)
    check("native fd write becomes a RAWLOG frame", raw is not None)
    check("no stray bytes on the frame stream", len(b.stray) == 0, bytes(b.stray[:200]))

    # threads
    b.clear()
    b.call("o1", "fixture.py", "emit_from_threads", [])
    got = {b.wait(lambda t, h, x, k=k: t == F.EVENT and h.get("name") == "from_thread" and h["args"] == [k]) is not None for k in ("run_in_background", "plain")}
    check("emit from run_in_background and plain threads", got == {True})

    # ordering
    for i in range(1000):
        b.event("o1", "fixture.py", "track", [i])
    r = b.call("o1", "fixture.py", "get_count", [])
    check("1000 events processed in order", r is not None and r[1]["result"] == {"count": 1000, "ordered": True}, r and r[1])

    # owners are isolated
    b.control(cmd="start", owner="o2", script="fixture.py", path=FIXTURE)
    b.status("started", "o2")
    r = b.call("o2", "fixture.py", "get_count", [])
    check("second owner has its own module state", r is not None and r[1]["result"]["count"] == 0, r and r[1])

    # load failure
    b.control(cmd="start", owner="o1", script="broken.py", path=BROKEN)
    f = b.status("error", "o1", "broken.py")
    check("load error reported with traceback", f is not None and "broken on purpose" in f[1].get("traceback", ""), f and f[1])

    # reload resets module state
    b.clear()
    b.control(cmd="reload", owner="o1", script="fixture.py")
    f_bye = b.ev("bye", "o1")
    check("reload ran on_stop of the old instance", f_bye is not None and f_bye[1]["args"] == [1000], f_bye and f_bye[1])
    check("reload started a fresh instance", b.status("started", "o1", "fixture.py") is not None)
    r = b.call("o1", "fixture.py", "get_count", [])
    check("reloaded module state is fresh", r is not None and r[1]["result"]["count"] == 0, r and r[1])

    # cooperative stop
    b.clear()
    cid = b.event("o1", "fixture.py", "long_job", [], call=True)
    time.sleep(0.2)
    t0 = time.time()
    b.control(cmd="stop", owner="o1", script="fixture.py")
    r = b.reply(cid)
    check("stop makes should_stop() true for running work", r is not None and r[1]["result"] == "stopped early", r and r[1])
    check("stop finishes promptly", b.status("stopped", "o1", "fixture.py") is not None and time.time() - t0 < 2.0)
    r = b.call("o1", "fixture.py", "add", [1, 1])
    check("call after stop is an error reply", r is not None and not r[1]["ok"], r and r[1])

    # stopOwner
    b.clear()
    b.control(cmd="stopOwner", owner="o2")
    check("stopOwner stops that owner's scripts", b.status("stopped", "o2", "fixture.py") is not None)

    # throughput
    b.control(cmd="start", owner="o3", script="fixture.py", path=FIXTURE)
    b.status("started", "o3")
    blob = os.urandom(1 << 20)
    t0 = time.time()
    for _ in range(100):
        r = b.call("o3", "fixture.py", "echo", [{"_bin": 0}], [blob])
    dt = time.time() - t0
    check("100 x 1 MB call round trips", r is not None and r[1]["ok"])
    print(f"     {100 / dt:.0f} round trips/s, {200 / dt:.0f} MB/s both ways (engine-free, Python harness)")

    # streams
    stream_tests(b, numpy)

    # exit
    b.clear()
    b.control(cmd="exit", grace=500)
    f = b.status("exiting")
    check("exit runs on_stop and reports exiting", f is not None and b.ev("bye", "o3") is not None)
    try:
        code = b.p.wait(5)
    except subprocess.TimeoutExpired:
        code = None
    check("process exits with code 0", code == 0, code)
    b.close()

    # stdin EOF (Unreal died) shuts the bridge down
    b = Bridge(site_dirs=site)
    b.status("ready", timeout=20)
    b.p.stdin.close()
    try:
        code = b.p.wait(5)
    except subprocess.TimeoutExpired:
        code = None
    check("stdin EOF shuts the bridge down", code == 0, code)
    b.close()

    # a hard crash surfaces as stray text (faulthandler dump)
    b = Bridge(site_dirs=site)
    b.status("ready", timeout=20)
    b.control(cmd="start", owner="o1", script="fixture.py", path=FIXTURE)
    b.status("started", "o1")
    b.event("o1", "fixture.py", "die", [], call=True)
    try:
        code = b.p.wait(10)
    except subprocess.TimeoutExpired:
        code = None
    time.sleep(0.2)
    text = bytes(b.stray).decode(errors="replace")
    check("hard crash dump arrives as stray text", "fatal" in text.lower() and code not in (0, None), (code, text[:200]))
    b.close()

    print(f"\n{sum(results)}/{len(results)} passed")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
