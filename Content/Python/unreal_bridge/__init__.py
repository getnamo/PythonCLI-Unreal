"""unreal_bridge: the API Python scripts use to talk to the Unreal component that started them.

    import unreal_bridge as ue

    @ue.on("observation")              # Unreal: EmitEvent("observation", ...)
    def observe(obs):
        ue.emit("action", policy(obs))  # Unreal: OnEvent("action", ...)

    def add(a, b):                      # Unreal: CallFunction("add", "[2,3]") -> 5
        return a + b

    @ue.on_stream("camera")            # Unreal: a Python Render Target Stream named "camera"
    def see(frame):
        frame.array                     # (H, W, 4) uint8 view of the shared-memory slot; frame.rgb()

    def on_start(): ...                 # optional: after the script loads
    def on_stop(): ...                  # optional: before it unloads (stop, reload, exit)

Everything for a script runs in order on that script's own worker thread. Long work should check
ue.should_stop() (or ue.stopping) so stop/reload can finish. Bytes and numpy arrays (or ue.Tensor without
numpy) pass through as binary, without JSON encoding.
"""
import os

from . import _host as _h
from . import _streams
from ._host import current_context as _ctx
from ._streams import StreamFrame
from ._values import Tensor, numpy_version
from .ml import MLPluginAPI, TFPluginAPI

__all__ = ["on", "on_stream", "stream", "emit", "log", "warning", "error", "args", "should_stop", "stopping",
           "run_in_background", "script_path", "script_dir", "project_dir", "content_dir", "owner", "Tensor",
           "StreamFrame", "numpy_version", "content_path", "MLPluginAPI", "TFPluginAPI", "keras"]


def _require():
    ctx = _ctx()
    if ctx is None:
        raise RuntimeError("unreal_bridge: not called from a script started by Unreal")
    return ctx


def on(name=None):
    """Register a handler for an Unreal event (and for CallFunction of that name). Use as @ue.on("name") or
    bare @ue.on (the function's own name)."""
    def register(fn, event=None):
        ctx = _require()
        ctx.handlers.setdefault(event or fn.__name__, []).append(fn)
        return fn
    if callable(name):
        return register(name)
    return lambda fn: register(fn, name)


def on_stream(name=None):
    """Register a handler for an Unreal stream (render target, audio, observations): fn(frame) is called
    on the script's worker thread with a StreamFrame. frame.array is a view of Unreal's slot, valid until
    the handler returns. For Lockstep streams the return value is the step's result in Unreal.
    Use as @ue.on_stream("name") or bare @ue.on_stream (the function's own name)."""
    def register(fn, stream_name=None):
        ctx = _require()
        ctx.stream_handlers.setdefault(stream_name or fn.__name__, []).append(fn)
        return fn
    if callable(name):
        return register(name)
    return lambda fn: register(fn, name)


def stream(name, timeout=None):
    """Iterate a stream's frames instead of handling them with @ue.on_stream (typically from
    ue.run_in_background). Each frame stays valid until the next one is requested; the loop ends when the
    script stops, or after `timeout` seconds without a frame. Lockstep frames: frame.reply(result)."""
    return _streams.iterate(_require(), name, timeout)


def emit(name, *args):
    """Send an event to the owning Unreal component (OnEvent). Args: JSON values, bytes, arrays/tensors."""
    ctx = _require()
    ctx.host.emit(ctx, name, args)


def log(*parts):
    _h._host.log("info", " ".join(str(p) for p in parts), _ctx())


def warning(*parts):
    _h._host.log("warning", " ".join(str(p) for p in parts), _ctx())


def error(*parts):
    _h._host.log("error", " ".join(str(p) for p in parts), _ctx())


def should_stop():
    """True once Unreal has asked this script to stop (StopScript, reload, EndPlay, exit)."""
    ctx = _ctx()
    return ctx is not None and ctx.stopping.is_set()


def content_path(*parts):
    """A path under the project's Content directory, e.g. ue.content_path("Models", "mnist.keras")."""
    return os.path.join(_h._host.config.get("content_dir", ""), *parts)


def run_in_background(fn, *args, **kwargs):
    """Run fn on a new thread that belongs to this script (emit/log work from it; check should_stop())."""
    return _require().run_in_background(fn, *args, **kwargs)


def __getattr__(name):
    # Per-script values, resolved from the calling script.
    if name == "args":
        return list(_require().args)
    if name == "stopping":
        return _require().stopping
    if name == "owner":
        return _require().owner
    if name == "script_path":
        return _require().path
    if name == "script_dir":
        return os.path.dirname(_require().path)
    if name in ("project_dir", "content_dir"):
        return _h._host.config.get(name, "")
    if name == "keras":  # ue.keras.StopOnRequest / ProgressEvents; imports Keras only when used
        import importlib
        return importlib.import_module(".keras", __name__)
    raise AttributeError(f"module 'unreal_bridge' has no attribute '{name}'")
