# PythonCLI - Unreal

Run Python in Unreal Engine with a portable interpreter and a per-project venv via [CLISystem](https://github.com/getnamo/CLISystem-Unreal/) plugin. 


Scripts run **out of process**: e.g. a crash in a native ML library never takes the editor down, and the GIL never touches the game thread. Components talk to their scripts with events in both directions (JSON plus raw binary and tensors) and call Python functions with replies.

It's the base for the [TensorFlow-Unreal](https://github.com/getnamo/TensorFlow-Unreal) 2.0 refactor, and for PyTorch workflows.

Status: **0.2.0 (beta)**. Win64 tested; Linux x86_64 tested for the Python side.

| Piece | What it is |
|---|---|
| Interpreter | [python-build-standalone](https://github.com/astral-sh/python-build-standalone) CPython 3.12, bundled per platform |
| Environment | a venv managed by [uv](https://github.com/astral-sh/uv), at `<Project>/Intermediate/PythonCLI/venv` by default |
| Transport | framed stdin/stdout through [CLISystem](https://github.com/getnamo/CLISystem-Unreal) 1.4+ (blocking reader, async writes) |
| Python API | `import unreal_bridge as ue` |

## Setup

1. Put `PythonCLI-Unreal` and [CLISystem-Unreal](https://github.com/getnamo/CLISystem-Unreal) (1.4.0 or newer) in your project's `Plugins/`.
2. When cloning from git, fetch the bundled runtime. It's pinned and SHA-256 verified; release archives already include it.
   - Windows: `powershell -ExecutionPolicy Bypass -File Plugins/PythonCLI-Unreal/Tools/FetchRuntime.ps1`
   - Linux: `Plugins/PythonCLI-Unreal/Tools/fetch_runtime.sh`
3. Start the editor. The venv is created in the background, which takes about a second when there are no requirements.

## Quick start

`Content/Scripts/my_agent.py`:

```python
import unreal_bridge as ue

def on_start():                     # optional: after the script loads
    ue.log("ready", ue.args)

@ue.on("observation")               # Unreal: EmitEvent("observation", "[...]")
def observe(obs):
    ue.emit("action", {"move": 1})  # Unreal: OnEvent("action", ...)

def add(a, b):                      # Unreal: Call Python Function "add" "[2, 3]" -> 5
    return a + b

def on_stop():                      # optional: before it unloads (stop, reload, EndPlay)
    pass
```

In Unreal:

1. Add a **Python** component and set `Script Params > Script` to `my_agent.py`. It starts on BeginPlay.
2. Send to the script with **Emit Event** / **Emit Event With Tensors**. Handle `ue.emit` with **On Event**.
3. Use **Call Python Function** (an async node with On Success / On Failure) for request/response.
4. Use **On Script Started / Stopped / Error** and **On Log** to follow the script's lifecycle and output.

The bundled examples need no copying: `Script = "examples/echo.py"` or `"examples/tensor_stats.py"`. Scripts are looked up under `ScriptRoot` (`Content/Scripts/`) first, then in the plugin's `Content/Python/`.

## Data

- **Arguments** are a JSON array: `EmitEvent("x", "[1, \"two\", {\"k\": 3}]")` calls `handler(1, "two", {"k": 3})`. A single JSON value is wrapped into a one-element array.
- **Tensors** (`FPyTensor`: DType, Shape, Data) travel as raw little-endian bytes, never as JSON.
  - In Python they are numpy arrays, zero-copy over the received frame, when numpy is installed. Otherwise they are `ue.Tensor`.
  - A tensor or array returned to Unreal shows up in `Result.Tensors` / `Data.Tensors`. In the JSON, binary values appear as `{"_tensor": i, "dtype", "shape"}` or `{"_bin": i}` placeholders indexing those arrays.
  - Make tensors with **Make Tensor From Floats / Bytes** and read them with **Tensor To Floats**.
- **bytes** sent from Python arrive as UInt8 tensors.
- torch tensors are converted through `.detach().cpu().numpy()`.

## Processes

- **Shared (default, `bUseSharedProcess`):** one Python process for the whole engine (`UPythonSubsystem`). It lives across PIE sessions, so `import torch` / `import tensorflow` and model loads are paid once per editor session.
  - Each component's scripts are isolated: their own module instance and their own ordered worker thread.
  - A component's scripts are unloaded when the component ends.
  - **Preload Modules** imports packages as soon as the shared process starts.
- **Dedicated (`bUseSharedProcess` off):** the component gets its own process. Use it to isolate crashes or to work in parallel without sharing a GIL.
- **Ordering:** everything sent to one script runs in order on that script's worker thread.
- **Long work:** check `ue.should_stop()`, so that StopScript, Reload and EndPlay can finish. For parallel work, use `ue.run_in_background(fn)`.
- **Errors:** an exception in a handler or call is reported, with its traceback, to the call's On Failure, to On Log, and to the Output Log. A native crash ends the process: pending calls fail, On Script Error fires, and the crash dump appears in the Output Log. The next Start Script brings the process back.

## Streams

For data that flows every frame, add a stream component next to the Python component. Its script handles the frames with `@ue.on_stream("name")`:

| Component | Python gets |
|---|---|
| **Python Render Target Stream** | a render target (for example a Scene Capture 2D's), read back asynchronously on the GPU, without a `ReadPixels` stall: `(H, W, C)` arrays, uint8 BGRA for RTF_RGBA8 (`frame.rgb()` gives RGB), float16 for RTF_RGBA16f, float32 for RTF_R32f |
| **Python Audio Stream** | microphone, submix (the game's audio) or pushed samples, as `(frames, channels)` float32 chunks of `ChunkSeconds`, with `frame.meta["sample_rate"]` |
| **Python Observation Stream** | rows of named float features (Set Feature, Commit Row): `(rows, features)` float32; `frame.rows()` gives dicts |
| **Python Stream** | any tensor you push (Push Tensor) |

```python
@ue.on_stream("camera")
def see(frame):
    rgb = frame.rgb()            # zero-copy view of Unreal's shared memory, valid until this returns
    x = torch.from_numpy(frame.copy()).cuda(non_blocking=True)

@ue.on_stream("agent")           # Lockstep: Unreal's Step waits for the return value
def act(frame):
    obs = frame.rows()[-1]       # {"x": ..., "v": ...}
    return {"force": policy(obs)}
```

- **Slots:** each stream has a few slots (Slots). Unreal writes a slot and sends it, and the slot comes back when the handler returns. Frames of 32 KB or more live in a shared-memory region; smaller ones travel inline. Both are the same in Python.
- **Policies:**
  - **Latest** (render targets): when Python falls behind, Unreal drops new frames and Python skips stale ones, so handlers always see the freshest frame.
  - **Queue** (observations, audio): every frame arrives, in order. Observation producers wait up to MaxWaitSeconds for a slot. Render-target and audio producers never wait; they drop and count it.
  - **Lockstep** (ML tests): **Step** sends the observation and blocks the game thread until the handler returns its action. With a fixed timestep this gives a deterministic `step()` loop. In Blueprint, read the action from the result's JSON with **Get Json Number** (`"force"`, `"action.move"`, `"values.2"`).
- **Iterating instead:** `for frame in ue.stream("camera"):` in a `ue.run_in_background` thread.
- **Stats:** Get Stream Stats reports sent, dropped, acked, in flight and the ACK latency.
- **Measured (Win64, i9-7980XE):**
  - 1080p RGBA8 through shared memory: about 2,200 fps (18 GB/s, including Unreal filling each frame and Python checking it).
  - 256-byte inline frames: about 13,900/s.
  - Lockstep step round trip: p50 0.3 ms.
  - Render-target readback: ready about 2 frames after capture.
- **Example:** `examples/streams.py` handles all four kinds.

## Environment

Configure in **Project Settings > Plugins > Python CLI**:

| Setting | |
|---|---|
| Requirements | pip requirement lines (`numpy`, `torch==2.14.*`, …). A `Content/Scripts/requirements.txt` is installed too. Changes apply in the background. |
| Index Url / Extra Index Urls / Torch Backend | passed to `uv pip install` (`--torch-backend cu130` picks PyTorch's CUDA build) |
| Venv Directory | default `Intermediate/PythonCLI/venv`. It can be regenerated at any time, and uv's global cache makes that fast (a 4.5 GB TF + torch env rebuilds in about 12 s). |
| Environment Variables | set in the Python process before any import (`CUDA_VISIBLE_DEVICES`, `TF_CPP_MIN_LOG_LEVEL`, `KERAS_BACKEND`, …) |
| Preload Modules / Start Shared Process On Startup | warm the shared process |

How it behaves:

- **Up-to-date check:** setup is skipped when nothing changed. A hash of the Python version, requirements and index settings is stored in the venv.
- **Additive installs:** a removed requirement stays installed until you delete the venv.
- **The venv is an ordinary venv:** point VS Code or PyCharm at it, or `uv pip install` into it by hand.
- **Interpreter:** processes always run the bundled interpreter with `-I` (isolated from any system Python and `PYTHONPATH`) and add the venv's site-packages. Moving the project never breaks the environment.
- **Windows long paths:** without long-path support, keep the venv path short. TensorFlow installs paths 163 characters deep inside site-packages, and setup warns when the configured path is too long.
- **Other plugins:** they can add requirements with `FPythonEnvironment::RegisterRequirementsFile(Path)`.

## Python API (`unreal_bridge`)

| | |
|---|---|
| `@ue.on("name")` / `@ue.on` | handle an Unreal event (and calls of that name) |
| `ue.emit(name, *args)` | event to the owning component (On Event); args are JSON values, bytes, or numpy/`ue.Tensor` arrays |
| module-level functions | callable from Unreal (Call Python Function / `CallFunction`) |
| `on_start()`, `on_stop()` | lifecycle hooks |
| `ue.args`, `ue.owner`, `ue.script_path`, `ue.script_dir`, `ue.project_dir`, `ue.content_dir` | context |
| `ue.should_stop()`, `ue.stopping` | cooperative stop |
| `ue.run_in_background(fn, *args)` | thread that belongs to the script (emit/log work from it) |
| `ue.log / ue.warning / ue.error`, `print` | output to On Log and the Output Log |
| `@ue.on_stream("name")` / `@ue.on_stream` | handle a stream's frames; for Lockstep streams the return value is the step's result |
| `ue.stream(name, timeout=None)` | iterate a stream's frames instead (each stays valid until the next) |
| `StreamFrame` | `.array`, `.rgb()`, `.rows()`, `.copy()`, `.seq`, `.time`, `.frame`, `.layout`, `.meta`, `.info`, `.reply(value)`, `.release()` |
| `ue.Tensor` | tensor type used without numpy (`.data`, `.dtype`, `.shape`, `.tolist()`, `Tensor.from_list`) |

## C++

```cpp
Python->CallFunction(TEXT("predict"), TEXT("[]"), { FPyTensor::FromFloats(Pixels, { 64, 64, 3 }) },
    [](const FPythonCallResult& R) { if (R.bSuccess) { /* R.Result (FJsonValue), R.Tensors */ } });
Python->BindEventNative(TEXT("action"), [](const FPythonEventData& E) { /* E.Args, E.Tensors */ });

// Streams from any thread (render thread, audio thread): FPythonStream::Open, then AcquireSlot / GetSlotData /
// Commit, or Push; Step for lockstep.
```

## Tests

- Engine-free protocol tests drive `runtime.py` over frames exactly as Unreal does, streams included (41 checks):
  `ThirdParty/python/Win64/python.exe -I Content/Python/tests/harness.py [--site <venv>/Lib/site-packages]`
  On Linux: `ThirdParty/python/Linux/bin/python3.12 -I Content/Python/tests/harness.py`
- In-engine tests (`PythonCLI.*`, 21 of them):
  `UnrealEditor-Cmd.exe <Project>.uproject -ExecCmds="Automation RunTests PythonCLI; Quit" -unattended -nullrhi -nopause`
  - `PythonCLI.Environment.Requirements` installs numpy, so it needs network access or a warm uv cache.
  - `PythonCLI.Streams.RenderTarget` needs a real RHI: run it with `-RenderOffscreen` instead of `-nullrhi`.
  - `PythonCLI.Streams.Microphone` opens your microphone, so it only runs with `-PythonCLIMicTest`.

## Protocol

Frames (same layout as CLISystem's `FCLIFrameCodec`, magic `PUE\x01`): `MAGIC | type u8 | header_len u32 | JSON header | binary_len u32 | binary`.

| # | Type | Direction | Header |
|---|---|---|---|
| 1 | LOG | py → UE | `owner?, script?, level, msg` |
| 2 | RAWLOG | py → UE | native stdout/stderr (binary = text) |
| 3 | EVENT | both | `owner, script, name, args[, id]` + binary table |
| 4 | REPLY | py → UE | `owner, script, id \| sid, ok, result \| error, traceback` + binary table |
| 5 | CONTROL | UE → py | `cmd`: `start`, `stop`, `reload`, `stopOwner`, `exit`, `ping`, `openStream`, `closeStream` |
| 6 | STATUS | py → UE | `status`: `ready`, `preloaded`, `started`, `stopped`, `error`, `exiting`, `pong`, `streamError` |
| 7 | STREAM | UE → py | `owner, stream, seq, slot, t, frame, dtype, shape, nbytes, layout?, meta?, sid?` + inline data (shared-memory streams: none) |
| 8 | ACK | py → UE | `owner, stream, slot, seq`: the slot is Unreal's again |

`openStream` carries `stream, slots, slot_bytes, policy, shm` (a `{GUID}` region name, or null for inline) and `info`. Unreal handles ACKs and lockstep replies (`sid`) on its pipe thread, so they arrive while the game thread is blocked in a step.

`runtime.py` keeps the protocol on a private copy of stdout and redirects fd 1/2 into a capture pipe. Output from native code therefore can't corrupt frames; it arrives as RAWLOG. A hard crash's `faulthandler` dump arrives as stray text, which is logged.

## Roadmap

- Render targets GPU-to-GPU (D3D12 shared handle → CUDA → DLPack) to skip the readback.
- The ML component (`MLPluginAPI` / `TFPluginAPI`) and TensorFlow-Unreal 2.0.
- Linux editor validation, packaging modes, releases.
- Torch-Unreal.
