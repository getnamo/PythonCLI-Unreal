"""Stream fixture for the harness and the PythonCLI.Streams.* tests."""
import struct
import time

import unreal_bridge as ue

stats = {}
sleep_seconds = 0.0
rng = 12345


def _stats(name):
    return stats.setdefault(name, {"frames": 0, "bad": 0, "out_of_order": 0, "last_seq": -1, "seqs": [],
                                   "sum": 0.0, "samples": 0, "crossings": 0, "prev": 0.0})


def _track(name, frame):
    s = _stats(name)
    s["frames"] += 1
    if frame.seq <= s["last_seq"]:
        s["out_of_order"] += 1
    s["last_seq"] = frame.seq
    s["seqs"].append(frame.seq)
    return s


def _raw(a):
    return memoryview(a.data).cast("B")


@ue.on_stream("frames")
def frames(frame):
    """Synthetic frames: every uint32 equals the frame's seq (checks first/middle/last; all of it every 50th)."""
    s = _track("frames", frame)
    raw = _raw(frame.array)
    n = len(raw) // 4
    want = frame.seq & 0xFFFFFFFF
    ok = n > 0 and all(struct.unpack_from("<I", raw, i * 4)[0] == want for i in (0, n // 2, n - 1))
    if ok and frame.seq % 50 == 0 and not isinstance(frame.array, ue.Tensor):
        ok = bool((frame.array.reshape(-1).view("<u4") == want).all())
    if not ok:
        s["bad"] += 1
    if sleep_seconds:
        time.sleep(sleep_seconds)


@ue.on_stream("obs")
def obs(frame):
    s = _track("obs", frame)
    rows = frame.rows()
    s["rows"] = s.get("rows", 0) + len(rows)
    s["features"] = list(frame.info.get("features", []))
    s["sum"] += sum(sum(r.values()) for r in rows)
    s["last_row"] = rows[-1] if rows else {}


@ue.on_stream("step")
def step(frame):
    """Lockstep policy: a deterministic controller with a seeded LCG perturbation. Returns the action."""
    global rng
    _track("step", frame)
    row = frame.rows()[0]
    rng = (rng * 1103515245 + 12345) & 0x7FFFFFFF
    noise = (rng / 0x7FFFFFFF - 0.5) * 0.1
    force = -2.0 * row["x"] - 0.5 * row["v"] + noise
    return {"step": int(row["step"]), "force": force}


@ue.on_stream("audio")
def audio(frame):
    """Audio chunks (frames, channels) float32: count samples and zero crossings (channel 0)."""
    s = _track("audio", frame)
    s["sample_rate"] = frame.meta.get("sample_rate", frame.info.get("sample_rate", 0))
    s["channels"] = frame.shape[1] if len(frame.shape) > 1 else 1
    values = frame.array.tolist()
    if values and isinstance(values[0], list):
        values = [v[0] for v in values]
    elif s["channels"] > 1:
        values = values[::s["channels"]]
    prev = s["prev"]
    for v in values:
        if (prev < 0.0) != (v < 0.0):
            s["crossings"] += 1
        prev = v
        s["sum"] += abs(v)
    s["prev"] = prev
    s["samples"] += len(values)


@ue.on_stream("camera")
def camera(frame):
    s = _track("camera", frame)
    rgb = frame.rgb()
    h, w = frame.shape[0], frame.shape[1]
    if isinstance(rgb, ue.Tensor):
        raw = _raw(frame.array)
        first = list(raw[0:4])
        s["first"] = [first[2], first[1], first[0]] if frame.layout == "BGRA" else first[:3]
    else:
        s["first"] = [int(v) for v in rgb[0, 0]]
        s["center"] = [int(v) for v in rgb[h // 2, w // 2]]
    s["layout"] = frame.layout
    s["shape"] = list(frame.shape)
    s["dtype"] = frame.dtype


def start_iterating(name, timeout=5.0):
    def body():
        for frame in ue.stream(name, timeout=timeout):
            _track(name, frame)
            if frame.lockstep:
                frame.reply({"seq": frame.seq})
        _stats(name)["iterator_done"] = True
    ue.run_in_background(body)
    return True


def set_sleep(seconds):
    global sleep_seconds
    sleep_seconds = seconds
    return True


def reset(seed=12345):
    global rng
    stats.clear()
    rng = seed
    return True


def get_stats(name):
    s = dict(_stats(name))
    if s["samples"] and s.get("sample_rate"):
        s["frequency"] = s["crossings"] / 2.0 / (s["samples"] / s["sample_rate"])
    s.pop("prev", None)
    return s


def die():
    import faulthandler
    faulthandler._read_null()
