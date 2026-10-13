"""Jobs: long-running work (training, evaluation, data processing) that Unreal starts, follows and stops.

    import unreal_bridge as ue

    @ue.job("train")                                  # Unreal: StartJob("train", {"epochs": 3})
    def train(job, epochs: int = 5, lr: float = 1e-3):
        for epoch in job.range(epochs):               # progress; ends early (cleanly) when the job is stopped
            for batch in job.track(loader):           # progress within the epoch; also checks for stop
                ...
            job.metrics(epoch=epoch, loss=loss)       # -> OnMetrics in Unreal, <run>/metrics.jsonl
            job.save_state(best=best)                 # resumable: StartJob(..., resume) gets job.state back
        return {"accuracy": acc}                      # -> OnCompleted

A job runs on its own thread, so the script keeps answering calls and streams meanwhile. Each start creates
a run folder (<runs>/<job>/<timestamp>/) with config.json, metrics.jsonl, state.json and result.json; with
resume, the job continues the latest run of the same name, and job.range() picks up where it stopped.
Typed parameters (int, float, bool, str) are converted from Unreal's JSON.

Protocol (UPythonService::StartJob): calls _job_start(id, name, params, options), _job_stop(id),
_job_list(); events "_job" {id, kind: started|progress|metrics|log|completed|stopped|failed, ...}.
"""
import datetime
import inspect
import json
import os
import threading
import time
import traceback

from . import _host

__all__ = ["job", "Job", "JobStopped"]

PROGRESS_INTERVAL = 0.05  # at most 20 progress events a second


class JobStopped(Exception):
    """Raise (or let job.check() raise) to end a job early as 'stopped' rather than 'failed'."""


def job(name=None, **options):
    """Register a job function fn(job, **params) with the calling script. Use as @ue.job("name") or bare
    @ue.job (the function's name). options: runs_dir (default: the one Unreal passes)."""
    def register(fn, job_name=None):
        ctx = _host.current_context()
        if ctx is None:
            raise RuntimeError("@ue.job: not in a script started by Unreal")
        spec = _JobSpec(job_name or fn.__name__, fn, options)
        ctx.job_specs[spec.name] = spec
        fn.__ue_job__ = spec
        return fn
    if callable(name):
        return register(name)
    return lambda fn: register(fn, name)


class _JobSpec:
    def __init__(self, name, fn, options):
        self.name = name
        self.fn = fn
        self.options = options
        self.params = []
        try:
            for p in list(inspect.signature(fn).parameters.values())[1:]:
                if p.kind in (p.VAR_POSITIONAL, p.VAR_KEYWORD):
                    continue
                kind = p.annotation if p.annotation is not inspect.Parameter.empty else None
                self.params.append({"name": p.name, "type": getattr(kind, "__name__", str(kind)) if kind is not None else "",
                                    "default": _jsonable(p.default) if p.default is not inspect.Parameter.empty else None,
                                    "required": p.default is inspect.Parameter.empty})
        except (TypeError, ValueError):
            pass

    def describe(self):
        return {"name": self.name, "params": self.params, "doc": inspect.getdoc(self.fn) or ""}

    def convert(self, params):
        """Unreal's JSON values -> the function's parameter types; unknown names are an error."""
        sig = inspect.signature(self.fn)
        accepted = list(sig.parameters.values())[1:]
        names = {p.name: p for p in accepted if p.kind not in (p.VAR_POSITIONAL, p.VAR_KEYWORD)}
        takes_any = any(p.kind == p.VAR_KEYWORD for p in accepted)
        out = {}
        for key, value in (params or {}).items():
            p = names.get(key)
            if p is None:
                if takes_any:
                    out[key] = value
                    continue
                raise TypeError(f"job '{self.name}' has no parameter '{key}' (it takes {', '.join(names) or 'none'})")
            kind = p.annotation
            try:
                if kind is bool and isinstance(value, str):
                    value = value.strip().lower() in ("1", "true", "yes", "on")
                elif kind in (int, float, bool, str) and value is not None:
                    value = kind(value)
            except (TypeError, ValueError) as e:
                raise TypeError(f"job '{self.name}': parameter '{key}' should be {kind.__name__}: {e}") from None
            out[key] = value
        return out


class Job:
    """A running job (the first argument of a job function)."""

    def __init__(self, ctx, job_id, spec, params, run_dir, resumed, state):
        self.id = job_id
        self.name = spec.name
        self.params = params
        self.run_dir = run_dir
        self.resumed = resumed
        self.state = state  # what save_state() stored (the resumed run's, on resume)
        self.result = None
        self._ctx = ctx
        self._spec = spec
        self._stop = threading.Event()
        self._last_progress = 0.0
        self._progress = 0.0
        self._span = (0.0, 1.0)
        self._range_key = None
        self._lock = threading.Lock()
        self.thread = None

    # -- state ---------------------------------------------------------------------------------------
    @property
    def should_stop(self):
        """True once Unreal stopped the job (or the script is stopping)."""
        return self._stop.is_set() or self._ctx.stopping.is_set()

    def stop(self):
        self._stop.set()

    def check(self):
        """Raise JobStopped if the job should stop (for deep loops that can't return easily)."""
        if self.should_stop:
            raise JobStopped()

    def wait(self, seconds):
        """Sleep, waking early (True) if the job is stopped."""
        return self._stop.wait(seconds) or self._ctx.stopping.is_set()

    # -- reporting -----------------------------------------------------------------------------------
    @property
    def fraction(self):
        """The job's progress so far, 0..1."""
        return self._progress

    def progress(self, fraction=None, message=None, force=False):
        """0..1 (of the whole job; None keeps it, e.g. to send just a message); rate-limited to 20 events a second
        unless force (or a message is given)."""
        if fraction is not None:
            self._progress = max(0.0, min(1.0, float(fraction)))
        force = force or message is not None
        now = time.monotonic()
        if force or now - self._last_progress >= PROGRESS_INTERVAL or self._progress >= 1.0:
            self._last_progress = now
            event = {"progress": round(self._progress, 5)}
            if message is not None:
                event["message"] = str(message)
            self._emit("progress", **event)

    def metrics(self, step=None, **values):
        """Report values (numbers, or tensors/arrays with one element). Appended to <run>/metrics.jsonl with
        a time and step, and sent to Unreal (OnMetrics)."""
        clean = {k: _number(v) for k, v in values.items()}
        record = {"time": round(time.time(), 3), "step": step, **clean} if step is not None else {"time": round(time.time(), 3), **clean}
        with self._lock:
            with open(os.path.join(self.run_dir, "metrics.jsonl"), "a", encoding="utf-8") as f:
                f.write(json.dumps(record) + "\n")
        self._emit("metrics", step=step, metrics=clean)

    def log(self, *parts):
        """A line in Unreal's log, attributed to the job."""
        self._ctx.host.log("info", f"[{self.name}] " + " ".join(str(p) for p in parts), self._ctx)

    def save_state(self, **values):
        """Merge values into job.state and write <run>/state.json (what resume restores)."""
        self.state.update(values)
        tmp = os.path.join(self.run_dir, "state.json.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(_jsonable(self.state), f, indent=1)
        os.replace(tmp, os.path.join(self.run_dir, "state.json"))

    # -- loops ---------------------------------------------------------------------------------------
    def range(self, stop, start=None, key="range"):
        """Like range(start or 0, stop), but: progress follows it; it ends early when the job is stopped; and
        its position is saved in job.state[key] after each step, so a resumed job continues from there."""
        stop = int(stop)
        first = int(start) if start is not None else int(self.state.get(key, 0)) if self.resumed else 0
        outer = self._span
        self._range_key = key
        try:
            for i in range(first, stop):
                if self.should_stop:
                    return
                lo = outer[0] + (outer[1] - outer[0]) * i / max(stop, 1)
                hi = outer[0] + (outer[1] - outer[0]) * (i + 1) / max(stop, 1)
                self._span = (lo, hi)
                self.progress(lo)
                yield i
                if self.should_stop:
                    return  # the step was cut short: resume repeats it
                self.save_state(**{key: i + 1})
            self.progress(outer[1], force=True)
        finally:
            self._span = outer

    def track(self, iterable, total=None):
        """Yield from iterable, with progress inside the current job.range step (or the whole job), ending
        early when the job is stopped."""
        if total is None:
            try:
                total = len(iterable)
            except TypeError:
                total = None
        lo, hi = self._span
        for k, item in enumerate(iterable):
            if self.should_stop:
                return
            if total:
                self.progress(lo + (hi - lo) * k / total)
            yield item

    # -- internals -----------------------------------------------------------------------------------
    def _emit(self, kind, **fields):
        self._ctx.host.emit(self._ctx, "_job", [{"id": self.id, "kind": kind, **fields}])

    def _run(self):
        _host._tls.ctx = self._ctx
        status, extra = "completed", {}
        try:
            self._emit("started", run_dir=self.run_dir, resumed=self.resumed, params=_jsonable(self.params))
            result = self._spec.fn(self, **self.params)
            self.result = _jsonable(result)
            if self.should_stop:
                status = "stopped"
            extra = {"result": self.result}
        except JobStopped:
            status, extra = "stopped", {"result": self.result}
        except BaseException as e:  # noqa: BLE001 - a job's failure must reach Unreal whatever it is
            tb = traceback.format_exc()
            status, extra = "failed", {"error": f"{type(e).__name__}: {e}", "traceback": tb}
            self._ctx.host.log("error", f"job '{self.name}' failed:\n{tb}", self._ctx)
        finally:
            self._ctx.running_jobs.pop(self.id, None)
        try:
            with open(os.path.join(self.run_dir, "result.json"), "w", encoding="utf-8") as f:
                json.dump({"status": status, "finished": _now(), **{k: v for k, v in extra.items() if k != "traceback"}}, f, indent=1, default=str)
        except (OSError, TypeError):
            pass
        self._emit(status, **extra)


class Adapter:
    """The script side of the job protocol (the reserved _job_* calls)."""

    def __init__(self, ctx):
        self.ctx = ctx

    def _job_list(self):
        return [s.describe() for s in self.ctx.job_specs.values()]

    def _job_start(self, job_id, name, params=None, options=None):
        options = options or {}
        spec = self.ctx.job_specs.get(name)
        if spec is None:
            known = ", ".join(self.ctx.job_specs) or "none"
            raise KeyError(f"{self.ctx.script} has no job '{name}' (jobs: {known})")
        for running in self.ctx.running_jobs.values():
            if running.name == name:
                raise RuntimeError(f"job '{name}' is already running ({running.id}); stop it first")
        converted = spec.convert(params)
        runs = spec.options.get("runs_dir") or options.get("runs_dir") or os.path.join(_host._host.config.get("project_dir", "."), "Saved", "PythonCLI", "Runs")
        run_dir, resumed, state = _open_run(os.path.join(runs, _safe(name)), bool(options.get("resume")))
        with open(os.path.join(run_dir, "config.json" if not resumed else f"config-resume-{_stamp()}.json"), "w", encoding="utf-8") as f:
            json.dump({"job": name, "id": job_id, "script": self.ctx.path, "params": _jsonable(converted), "started": _now(), "resumed": resumed}, f, indent=1)
        j = Job(self.ctx, job_id, spec, converted, run_dir, resumed, state)
        self.ctx.running_jobs[job_id] = j
        j.thread = threading.Thread(target=j._run, name=f"py:{self.ctx.owner}:job:{name}", daemon=True)
        j.thread.start()
        return {"id": job_id, "run_dir": run_dir, "resumed": resumed}

    def _job_stop(self, job_id):
        j = self.ctx.running_jobs.get(job_id)
        if j is None:
            return False
        j.stop()
        return True


def stop_all(ctx, timeout=10.0):
    """Script unload: stop running jobs and give them time to end."""
    jobs = list(ctx.running_jobs.values())
    for j in jobs:
        j.stop()
    deadline = time.monotonic() + timeout
    for j in jobs:
        if j.thread is not None and j.thread is not threading.current_thread():
            j.thread.join(max(0.0, deadline - time.monotonic()))


def _open_run(job_dir, resume):
    if resume and os.path.isdir(job_dir):
        runs = sorted((d for d in os.listdir(job_dir) if os.path.isdir(os.path.join(job_dir, d))), reverse=True)
        for d in runs:
            path = os.path.join(job_dir, d)
            state_file = os.path.join(path, "state.json")
            if os.path.isfile(state_file):
                try:
                    with open(state_file, encoding="utf-8") as f:
                        return path, True, json.load(f)
                except (OSError, ValueError):
                    continue
    base = os.path.join(job_dir, _stamp())
    path, n = base, 1
    while os.path.exists(path):
        n += 1
        path = f"{base}-{n}"
    os.makedirs(path)
    return path, False, {}


def _stamp():
    return datetime.datetime.now().strftime("%Y%m%d-%H%M%S")


def _now():
    return datetime.datetime.now().isoformat(timespec="seconds")


def _safe(name):
    return "".join(c if c.isalnum() or c in "-_." else "_" for c in name) or "job"


def _number(v):
    if hasattr(v, "item") and callable(v.item):  # torch / numpy scalars and one-element tensors
        try:
            return v.item()
        except (ValueError, RuntimeError):
            pass
    if isinstance(v, (int, float, bool)) or v is None:
        return v
    try:
        return float(v)
    except (TypeError, ValueError):
        return str(v)


def _jsonable(v):
    if isinstance(v, dict):
        return {str(k): _jsonable(x) for k, x in v.items()}
    if isinstance(v, (list, tuple)):
        return [_jsonable(x) for x in v]
    if isinstance(v, (str, int, float, bool)) or v is None:
        return v
    if hasattr(v, "item") and callable(v.item):
        try:
            return v.item()
        except (ValueError, RuntimeError):
            pass
    if hasattr(v, "tolist"):
        return v.tolist()
    return str(v)
