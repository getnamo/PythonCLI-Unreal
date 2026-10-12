"""Harness fixture: exercises the unreal_bridge API."""
import os
import threading
import time

import unreal_bridge as ue

count = 0
seen = []


def on_start():
    ue.emit("started_with", ue.args)


def on_stop():
    ue.emit("bye", count)


@ue.on("ping")
def ping(*args):
    ue.emit("pong", *args)


@ue.on("track")
def track(i):
    global count
    count += 1
    seen.append(i)


def get_count():
    return {"count": count, "ordered": seen == sorted(seen)}


def add(a, b):
    return a + b


def divide(a, b):
    return a / b


def echo(*args):
    return list(args)


def double(t):
    if hasattr(t, "dtype") and not isinstance(t, ue.Tensor):  # numpy
        return t * 2
    return ue.Tensor.from_list([v * 2 for v in t.tolist()], t.dtype, t.shape)


def say(text):
    print(f"printed: {text}")
    os.write(1, f"native: {text}\n".encode())
    return True


def emit_from_threads():
    ue.run_in_background(lambda: ue.emit("from_thread", "run_in_background"))
    threading.Thread(target=_plain_thread).start()
    return True


def _plain_thread():
    ue.emit("from_thread", "plain")


def long_job():
    t = time.time()
    while not ue.should_stop() and time.time() - t < 10:
        time.sleep(0.01)
    return "stopped early" if ue.should_stop() else "ran out"


def die():
    import faulthandler
    faulthandler._read_null()  # a real access violation (CRT signals like _sigsegv are per-thread on Windows)


def pid():
    return os.getpid()


def numpy_version():
    import numpy
    return numpy.__version__
