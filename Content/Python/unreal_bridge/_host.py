"""Script host: loads scripts for each Unreal owner, runs each on its own ordered worker thread, and routes
frames between them and Unreal. One host per Python process; a shared process hosts many owners.
"""
import importlib.util
import os
import queue
import re
import sys
import threading
import time
import traceback

from . import _frames as F
from . import _streams
from . import _values

_tls = threading.local()
_host = None  # the running Host (set by Host.__init__)


def current_context():
    """The ScriptContext of the calling code: the worker/background thread's script, else the nearest frame
    whose module globals belong to a script (covers threads a script starts itself)."""
    ctx = getattr(_tls, "ctx", None)
    if ctx is not None:
        return ctx
    f = sys._getframe(1)
    while f is not None:
        ctx = f.f_globals.get("__ue_context__")
        if ctx is not None:
            return ctx
        f = f.f_back
    return None


class ScriptContext:
    _counter = 0

    def __init__(self, host, owner, script, path, args):
        self.host = host
        self.owner = owner
        self.script = script
        self.path = path
        self.args = list(args or [])
        self.handlers = {}
        self.stream_handlers = {}  # stream name -> [fn(frame)]
        self.readers = {}  # stream name -> StreamReader (ue.stream iterators)
        self.ml = None  # ml.Adapter, created by the first _ml_* call (UPythonMLComponent)
        self.module = None
        self.queue = queue.SimpleQueue()
        self.stopping = threading.Event()
        self.stopped = threading.Event()
        self.background = []
        ScriptContext._counter += 1
        safe = re.sub(r"\W", "_", f"{owner}_{os.path.splitext(os.path.basename(path))[0]}")
        self.module_name = f"ue_script_{safe}_{ScriptContext._counter}"
        self.thread = threading.Thread(target=self._run, name=f"py:{owner}:{script}", daemon=True)

    # -- lifecycle (worker thread) -------------------------------------------------------------------
    def _run(self):
        _tls.ctx = self
        if not self._load():
            self.stopped.set()
            self.host._forget(self)
            return
        while True:
            task = self.queue.get()
            if task is None:
                break
            try:
                task()
            except Exception:  # noqa: BLE001 - a task already reports its own errors; this is a safety net
                self.host.log("error", traceback.format_exc(), self)
        self._unload()

    def _load(self):
        try:
            directory = os.path.dirname(self.path)
            if directory and directory not in sys.path:
                sys.path.insert(0, directory)
            spec = importlib.util.spec_from_file_location(self.module_name, self.path)
            if spec is None or spec.loader is None:
                raise ImportError(f"cannot load {self.path}")
            module = importlib.util.module_from_spec(spec)
            module.__ue_context__ = self
            sys.modules[self.module_name] = module
            self.module = module
            spec.loader.exec_module(module)
            hook = getattr(module, "on_start", None)
            if callable(hook):
                hook()
        except BaseException as e:  # noqa: BLE001 - report SystemExit/KeyboardInterrupt from scripts too
            sys.modules.pop(self.module_name, None)
            self.host.status("error", self, error=f"{type(e).__name__}: {e}", traceback=traceback.format_exc())
            self.host.log("error", f"{self.script} failed to start:\n{traceback.format_exc()}", self)
            return False
        self.host.status("started", self)
        return True

    def _unload(self):
        training = self.ml.training if self.ml is not None else None
        if training is not None and training.is_alive():
            training.join(10)  # should_stop is already true: let fit() finish its batch first
        hook = getattr(self.module, "on_stop", None) or getattr(self.module, "dispose", None)
        if callable(hook):
            try:
                hook()
            except Exception:  # noqa: BLE001
                self.host.log("error", f"{self.script} on_stop failed:\n{traceback.format_exc()}", self)
        self.handlers.clear()
        self.stream_handlers.clear()
        for reader in list(self.readers.values()):
            reader.drain()
        sys.modules.pop(self.module_name, None)
        self.module = None
        self.stopped.set()
        self.host._forget(self)
        self.host.status("stopped", self)

    # -- called from the host's reader thread --------------------------------------------------------
    def start(self):
        self.thread.start()

    def stop(self):
        if not self.stopping.is_set():
            self.stopping.set()
            self.queue.put(None)

    def submit(self, fn):
        if not self.stopping.is_set():
            self.queue.put(fn)
            return True
        return False

    # -- dispatch (worker thread) --------------------------------------------------------------------
    def find_callable(self, name):
        if name.startswith("_ml_"):  # the ML component's calls (unreal_bridge.ml)
            if self.ml is None:
                from . import ml
                self.ml = ml.Adapter(self)
            fn = getattr(self.ml, name, None)
            return fn if callable(fn) else None
        handlers = self.handlers.get(name)
        if handlers:
            return handlers[0]
        fn = getattr(self.module, name, None) if self.module is not None else None
        return fn if callable(fn) else None

    def dispatch_event(self, name, args):
        handlers = list(self.handlers.get(name, ()))
        if not handlers:
            fn = self.find_callable(name)
            handlers = [fn] if fn else []
        for h in handlers:
            try:
                h(*args)
            except Exception:  # noqa: BLE001
                self.host.log("error", f"{self.script}: handler for '{name}' failed:\n{traceback.format_exc()}", self)

    def dispatch_call(self, call_id, name, args):
        fn = self.find_callable(name)
        if fn is None:
            self.host.reply(self.owner, self.script, call_id, ok=False, error=f"{self.script} has no function or handler '{name}'")
            return
        try:
            result = fn(*args)
        except Exception as e:  # noqa: BLE001
            tb = traceback.format_exc()
            self.host.log("error", f"{self.script}: call '{name}' failed:\n{tb}", self)
            self.host.reply(self.owner, self.script, call_id, ok=False, error=f"{type(e).__name__}: {e}", traceback=tb)
            return
        self.host.reply(self.owner, self.script, call_id, ok=True, result=result)

    def dispatch_stream(self, frame):
        stream = frame.stream
        try:
            if stream.closed:
                return
            if frame._sid is None and stream.policy == "latest" and frame.seq < stream.latest_seq:
                stream.skipped += 1  # a newer frame is already on its way: skip to it
                return
            reader = self.readers.get(stream.name)
            if reader is not None:
                reader.put(frame)
                frame = None  # the iterator releases it
                return
            handlers = list(self.stream_handlers.get(stream.name, ()))
            if not handlers:
                if frame._sid is not None:
                    frame.fail(f"{self.script} has no @ue.on_stream('{stream.name}') handler")
                return
            result = None
            for i, h in enumerate(handlers):
                r = h(frame)
                if i == 0:
                    result = r
            if frame._sid is not None:
                frame.reply(result)
        except Exception as e:  # noqa: BLE001
            tb = traceback.format_exc()
            self.host.log("error", f"{self.script}: stream handler for '{stream.name}' failed:\n{tb}", self)
            if frame is not None:
                frame.fail(f"{type(e).__name__}: {e}", tb)
        finally:
            if frame is not None:
                frame.release()

    def add_reader(self, name):
        reader = _streams.StreamReader(name)
        self.readers[name] = reader
        return reader

    def remove_reader(self, name, reader):
        if self.readers.get(name) is reader:
            del self.readers[name]
        reader.drain()

    # -- script helpers ------------------------------------------------------------------------------
    def run_in_background(self, fn, *args, **kwargs):
        def body():
            _tls.ctx = self
            try:
                fn(*args, **kwargs)
            except Exception:  # noqa: BLE001
                self.host.log("error", f"{self.script}: background task failed:\n{traceback.format_exc()}", self)
        t = threading.Thread(target=body, name=f"py:{self.owner}:{self.script}:bg", daemon=True)
        self.background.append(t)
        t.start()
        return t


class _FramedText:
    """sys.stdout/sys.stderr replacement: complete lines become LOG frames attributed to the calling script."""

    def __init__(self, host, level, stream):
        self.host = host
        self.level = level
        self.stream = stream
        self.local = threading.local()
        self.encoding = "utf-8"
        self.errors = "replace"

    def write(self, s):
        if not s:
            return 0
        buf = getattr(self.local, "buf", "") + s
        if "\n" in buf:
            lines, buf = buf.rsplit("\n", 1)
            self.host.log(self.level, lines, current_context(), stream=self.stream)
        self.local.buf = buf
        return len(s)

    def flush(self):
        buf = getattr(self.local, "buf", "")
        if buf:
            self.local.buf = ""
            self.host.log(self.level, buf, current_context(), stream=self.stream)

    def isatty(self):
        return False

    def writable(self):
        return True

    def fileno(self):
        raise OSError("unreal_bridge stdout has no file descriptor")


class Host:
    def __init__(self, writer, config):
        global _host
        _host = self
        self.writer = writer
        self.config = config
        self.lock = threading.Lock()
        self.scripts = {}  # (owner, script) -> ScriptContext
        self.streams = {}  # (owner, stream) -> StreamState
        self.closing_streams = []  # closed, but frame arrays still referenced their memory
        self.exiting = False
        sys.stdout = _FramedText(self, "info", "stdout")
        sys.stderr = _FramedText(self, "warning", "stderr")

    # -- outgoing ------------------------------------------------------------------------------------
    def log(self, level, msg, ctx=None, stream=None):
        header = {"level": level, "msg": msg}
        if ctx is not None:
            header["owner"], header["script"] = ctx.owner, ctx.script
        if stream:
            header["stream"] = stream
        self.writer.write(F.LOG, header)

    def status(self, status, ctx=None, **fields):
        header = {"status": status, **fields}
        if ctx is not None:
            header["owner"], header["script"] = ctx.owner, ctx.script
        self.writer.write(F.STATUS, header)

    def emit(self, ctx, name, args):
        buffers = []
        encoded = _values.encode(list(args), buffers)
        header = {"owner": ctx.owner, "script": ctx.script, "name": name, "args": encoded}
        self.writer.write(F.EVENT, header, F.build_table(buffers) if buffers else b"")

    def reply(self, owner, script, call_id, ok, result=None, error=None, traceback=None):
        buffers = []
        header = {"owner": owner, "script": script, "id": call_id, "ok": ok}
        if ok:
            try:
                header["result"] = _values.encode(result, buffers)
            except Exception as e:  # noqa: BLE001
                header.update(ok=False, error=f"result could not be sent to Unreal: {type(e).__name__}: {e}")
                buffers = []
        else:
            header["error"] = error
            if traceback:
                header["traceback"] = traceback
        try:
            self.writer.write(F.REPLY, header, F.build_table(buffers) if buffers else b"")
        except TypeError as e:  # result isn't JSON serializable
            self.writer.write(F.REPLY, {"owner": owner, "script": script, "id": call_id, "ok": False,
                                        "error": f"result is not JSON serializable: {e}"})

    def stream_ack(self, frame):
        st = frame.stream
        self.writer.write(F.ACK, {"owner": st.owner, "stream": st.name, "slot": frame.slot, "seq": frame.seq})

    def stream_reply(self, frame, ok, result=None, error=None, traceback=None):
        st = frame.stream
        buffers = []
        header = {"owner": st.owner, "script": st.script, "stream": st.name, "sid": frame._sid, "ok": ok}
        if ok:
            try:
                header["result"] = _values.encode(result, buffers)
                self.writer.write(F.REPLY, header, F.build_table(buffers) if buffers else b"")
                return
            except Exception as e:  # noqa: BLE001 - not JSON serializable, unsupported dtype, ...
                header.pop("result", None)
                header.update(ok=False, error=f"step result could not be sent to Unreal: {type(e).__name__}: {e}")
        else:
            header["error"] = error
            if traceback:
                header["traceback"] = traceback
        self.writer.write(F.REPLY, header)

    # -- bookkeeping ---------------------------------------------------------------------------------
    def _forget(self, ctx):
        with self.lock:
            if self.scripts.get((ctx.owner, ctx.script)) is ctx:
                del self.scripts[(ctx.owner, ctx.script)]

    def _owned(self, owner):
        with self.lock:
            return [c for (o, _), c in self.scripts.items() if o == owner]

    # -- incoming (reader thread) --------------------------------------------------------------------
    def handle(self, ftype, header, binary):
        if ftype == F.STREAM:
            self._stream(header, binary)
        elif ftype == F.CONTROL:
            self._control(header)
        elif ftype == F.EVENT:
            self._event(header, binary)

    def _control(self, h):
        cmd = h.get("cmd")
        owner, script = h.get("owner", ""), h.get("script", "")
        if cmd == "start":
            self._start(owner, script, h.get("path", script), h.get("args"))
        elif cmd == "stop":
            with self.lock:
                ctx = self.scripts.get((owner, script))
            if ctx:
                ctx.stop()
            else:
                self.status("stopped", None, owner=owner, script=script)
        elif cmd == "reload":
            with self.lock:
                ctx = self.scripts.get((owner, script))
            path, args = (ctx.path, ctx.args) if ctx else (h.get("path", script), h.get("args"))
            self._start(owner, script, h.get("path", path), h.get("args", args))
        elif cmd == "stopOwner":
            for ctx in self._owned(owner):
                ctx.stop()
            with self.lock:
                names = [n for (o, n) in self.streams if o == owner]
            for name in names:
                self._close_stream(owner, name)
        elif cmd == "openStream":
            self._open_stream(owner, script, h)
        elif cmd == "closeStream":
            self._close_stream(owner, h.get("stream", ""))
        elif cmd == "ping":
            self.status("pong", None, id=h.get("id"))
        elif cmd == "exit":
            self.shutdown(h.get("grace", 300) / 1000.0)

    def _start(self, owner, script, path, args):
        # The new instance is registered at once (events queue up for it) and starts once any previous
        # instance has finished its on_stop, without blocking this reader thread.
        ctx = ScriptContext(self, owner, script, path, args)
        with self.lock:
            old = self.scripts.get((owner, script))
            self.scripts[(owner, script)] = ctx
        if old is None:
            ctx.start()
            return
        old.stop()

        def start_after_old():
            old.stopped.wait(10)
            ctx.start()
        threading.Thread(target=start_after_old, name=f"py:{owner}:{script}:restart", daemon=True).start()

    def _open_stream(self, owner, script, h):
        name = h.get("stream", "")
        self._close_stream(owner, name)
        try:
            st = _streams.StreamState(owner, script, name, h)
        except Exception as e:  # noqa: BLE001
            self.log("error", f"stream '{name}' could not be opened: {type(e).__name__}: {e}")
            self.writer.write(F.STATUS, {"status": "streamError", "owner": owner, "stream": name, "error": f"{type(e).__name__}: {e}"})
            return
        with self.lock:
            self.streams[(owner, name)] = st

    def _close_stream(self, owner, name):
        with self.lock:
            st = self.streams.pop((owner, name), None)
            if st is not None and not st.close():
                self.closing_streams.append(st)
            self.closing_streams = [c for c in self.closing_streams if not c.close()]

    def _stream(self, h, binary):
        owner, name = h.get("owner", ""), h.get("stream", "")
        with self.lock:
            st = self.streams.get((owner, name))
            ctx = None
            if st is not None:
                ctx = self.scripts.get((owner, st.script))
                if ctx is None:
                    ctx = next((c for (o, _), c in self.scripts.items() if o == owner), None)
        if st is None:
            # Unknown stream (closed meanwhile): still hand the slot back, and fail a waiting step
            self.writer.write(F.ACK, {"owner": owner, "stream": name, "slot": h.get("slot", 0), "seq": h.get("seq", 0)})
            if h.get("sid") is not None:
                self.writer.write(F.REPLY, {"owner": owner, "stream": name, "sid": h["sid"], "ok": False, "error": f"stream '{name}' is not open"})
            return
        st.received += 1
        if h.get("seq", 0) > st.latest_seq:
            st.latest_seq = h.get("seq", 0)
        frame = _streams.StreamFrame(self, st, h, binary)
        if ctx is None or not ctx.submit(lambda: ctx.dispatch_stream(frame)):
            frame.fail(f"no script is running for stream '{name}'")
            frame.release()

    def _event(self, h, binary):
        owner, script = h.get("owner", ""), h.get("script", "")
        name, call_id = h.get("name", ""), h.get("id")
        buffers = F.parse_table(binary) if binary else []
        try:
            args = _values.decode(h.get("args", []), buffers)
        except Exception as e:  # noqa: BLE001
            if call_id is not None:
                self.reply(owner, script, call_id, ok=False, error=f"bad arguments: {e}")
            return
        if not isinstance(args, list):
            args = [args]
        with self.lock:
            if script:
                targets = [self.scripts[(owner, script)]] if (owner, script) in self.scripts else []
            else:
                targets = [c for (o, _), c in self.scripts.items() if o == owner]
        if call_id is not None:
            if len(targets) != 1:
                why = f"script '{script}' is not running" if script else f"{len(targets)} scripts running; name one"
                self.reply(owner, script, call_id, ok=False, error=f"call '{name}': {why}")
                return
            ctx = targets[0]
            if not ctx.submit(lambda: ctx.dispatch_call(call_id, name, args)):
                self.reply(owner, script, call_id, ok=False, error=f"call '{name}': script '{ctx.script}' is stopping")
            return
        for ctx in targets:
            ctx.submit(lambda c=ctx: c.dispatch_event(name, args))

    def shutdown(self, grace):
        if self.exiting:
            return
        self.exiting = True
        with self.lock:
            contexts = list(self.scripts.values())
        for ctx in contexts:
            ctx.stop()
        deadline = time.monotonic() + grace
        for ctx in contexts:
            ctx.stopped.wait(max(0.0, deadline - time.monotonic()))
        for stream in (sys.stdout, sys.stderr):
            try:
                stream.flush()
            except Exception:  # noqa: BLE001
                pass
        self.status("exiting")
        time.sleep(0.02)
        os._exit(0)

    # -- main loop -----------------------------------------------------------------------------------
    def run(self, stdin_fd=0):
        decoder = F.FrameDecoder()
        while True:
            try:
                chunk = os.read(stdin_fd, 1 << 20)
            except OSError:
                chunk = b""
            if not chunk:
                self.shutdown(0.2)  # Unreal closed our stdin (it stopped or died)
                return
            frames, stray = decoder.feed(chunk)
            if stray:
                self.log("warning", f"ignored {len(stray)} non-frame byte(s) on stdin")
            for ftype, header, binary in frames:
                try:
                    self.handle(ftype, header, binary)
                except Exception:  # noqa: BLE001
                    self.log("error", f"bridge failed handling a frame:\n{traceback.format_exc()}")
