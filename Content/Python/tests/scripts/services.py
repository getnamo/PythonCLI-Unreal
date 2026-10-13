"""Fixture for PythonCLI 0.5: deferred replies, jobs, streams bound to a function (harness + PythonCLI.Service.*)."""
import concurrent.futures
import queue
import sysconfig
import threading
import time

import unreal_bridge as ue

pool = concurrent.futures.ThreadPoolExecutor(4)
held = queue.SimpleQueue()


def slow_square(x, delay):
    """Deferred: answered from a pool thread after delay seconds (replies may overtake each other)."""
    reply = ue.defer()

    def work():
        time.sleep(delay)
        reply.resolve(x * x)
    pool.submit(work)
    return "ignored: the deferred reply wins"


def future_double(x):
    return pool.submit(lambda: x * 2)


def future_fail():
    def boom():
        raise ValueError("future failed on purpose")
    return pool.submit(boom)


def deferred_reject():
    reply = ue.defer()
    threading.Thread(target=lambda: reply.reject("rejected on purpose"), daemon=True).start()


def raise_after_defer():
    ue.defer()
    raise RuntimeError("raised after defer")


def hold(x):
    """Answered later by release()."""
    held.put((x, ue.defer()))


def release():
    n = 0
    while True:
        try:
            x, reply = held.get_nowait()
        except queue.Empty:
            return n
        reply.resolve(x + 1000)
        n += 1


def defer_outside_call():
    """ue.defer() on another thread than the call's: an error (that thread isn't handling a call)."""
    result = {}
    t = threading.Thread(target=lambda: result.setdefault("e", _try_defer()))
    t.start()
    t.join()
    return result["e"]


def _try_defer():
    try:
        ue.defer()
        return "no error"
    except RuntimeError as e:
        return str(e)


def platlib():
    return {"platlib": sysconfig.get_paths()["platlib"], "purelib": sysconfig.get_path("purelib")}


def process_frame(frame):
    """A stream bound to this function (stream info "call")."""
    return {"sum": float(sum(frame.array.tolist())) if hasattr(frame.array, "tolist") else None, "name": frame.info.get("name")}


@ue.job("count")
def count(job, steps: int = 10, delay: float = 0.01, fail_at: int = -1):
    total = 0
    for i in job.range(steps):
        if i == fail_at:
            raise ValueError("job failed on purpose")
        total += i
        job.metrics(step=i, value=i * 0.5)
        job.save_state(total=total)
        if job.wait(delay):
            break
    return {"total": total, "resumed": job.resumed, "start_state": job.state.get("range")}


@ue.job
def tracked(job, items: int = 20):
    seen = 0
    for _ in job.track(range(items)):
        seen += 1
        time.sleep(0.005)
    return seen


@ue.job("raises_stop")
def raises_stop(job):
    while True:
        job.check()
        time.sleep(0.01)
