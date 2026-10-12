"""PythonCLI bridge process. Unreal launches this with the bundled interpreter:

    python -I -u -X utf8 runtime.py --config <runtime_config.json>

stdin/stdout carry frames (see unreal_bridge/_frames.py). Before anything else, the frame stream gets a
private copy of stdout and fd 1/fd 2 are pointed at a capture pipe, so output from native code (C
extensions, TensorFlow/CUDA logs, printf) arrives in Unreal as RAWLOG frames instead of corrupting frames.
"""
import json
import os
import sys
import threading
import time

_t0 = time.perf_counter()
_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _here)  # -I leaves the script's own directory off sys.path

from unreal_bridge import _frames as F  # noqa: E402  (stdlib-only; safe before site dirs are added)


def _protect_stdio():
    proto_fd = os.dup(1)
    read_fd, write_fd = os.pipe()
    os.dup2(write_fd, 1)
    os.dup2(write_fd, 2)
    os.close(write_fd)
    writer = F.FrameWriter(proto_fd)

    def capture():
        while True:
            try:
                chunk = os.read(read_fd, 65536)
            except OSError:
                return
            if not chunk:
                return
            writer.write(F.RAWLOG, {"stream": "native"}, chunk)

    threading.Thread(target=capture, name="native-output", daemon=True).start()
    import faulthandler
    faulthandler.enable(file=proto_fd)  # a hard crash's dump goes raw onto the stream; Unreal logs stray text
    return writer


def _exit_with_parent():
    if sys.platform.startswith("linux"):
        try:
            import ctypes
            import signal
            ctypes.CDLL("libc.so.6", use_errno=True).prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG
        except Exception:  # noqa: BLE001
            pass


def main():
    writer = _protect_stdio()
    _exit_with_parent()

    config = {}
    if "--config" in sys.argv:
        with open(sys.argv[sys.argv.index("--config") + 1], encoding="utf-8") as f:
            config = json.load(f)

    os.environ.update({k: str(v) for k, v in config.get("env", {}).items()})
    import site
    for d in config.get("site_dirs", []):
        if os.path.isdir(d):
            site.addsitedir(d)

    from unreal_bridge import _host, _values
    host = _host.Host(writer, config)
    host.status("ready", python=sys.version.split()[0], executable=sys.executable, pid=os.getpid(),
                numpy=_values.numpy_version(), numpy_error=_values.numpy_error(), startup_ms=round((time.perf_counter() - _t0) * 1000, 1))

    preload = config.get("preload", [])
    if preload:
        def run_preload():
            import importlib
            done, failed, t = [], {}, time.perf_counter()
            for name in preload:
                try:
                    importlib.import_module(name)
                    done.append(name)
                except Exception as e:  # noqa: BLE001
                    failed[name] = f"{type(e).__name__}: {e}"
            host.status("preloaded", modules=done, failed=failed, seconds=round(time.perf_counter() - t, 2))
        threading.Thread(target=run_preload, name="preload", daemon=True).start()

    host.run()


if __name__ == "__main__":
    main()
