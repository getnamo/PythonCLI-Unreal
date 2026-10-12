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

HERE = os.path.dirname(os.path.abspath(__file__))
PYDIR = os.path.dirname(HERE)
sys.path.insert(0, PYDIR)
from unreal_bridge import _frames as F  # noqa: E402

FIXTURE = os.path.join(HERE, "scripts", "fixture.py")
BROKEN = os.path.join(HERE, "scripts", "broken.py")
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
                self.frames.extend(frames)
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
