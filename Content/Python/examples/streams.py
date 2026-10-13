"""Streams from Unreal. Put this script on a Python component (Script = "examples/streams.py") and add any of,
on the same actor:

  Python Render Target Stream  (StreamName "camera")        a Scene Capture's render target
  Python Audio Stream          (StreamName "audio")         microphone or a submix
  Python Observation Stream    (StreamName "observations")  Features e.g. ["Pos.x", "Pos.y", "Pos.z", "Speed"]
  Python Observation Stream    (StreamName "agent", Policy Lockstep, Features ["x", "v"]): call Step in
                                Blueprint each tick and read the action with Get Json Number "force"

Frame arrays are views of Unreal's memory, valid until the handler returns: copy what you keep.
"""
import time

import unreal_bridge as ue

try:
    import numpy as np
except ImportError:
    np = None

_last_report = {}


def _every(name, seconds):
    now = time.monotonic()
    if now - _last_report.get(name, 0.0) >= seconds:
        _last_report[name] = now
        return True
    return False


@ue.on_stream("camera")
def camera(frame):
    # frame.rgb(): (H, W, 3) view in RGB order whatever the render target's layout (BGRA for RTF_RGBA8)
    if np is not None and _every("camera", 1.0):
        mean = frame.rgb().reshape(-1, 3).mean(axis=0)
        ue.emit("camera_mean", [round(float(c), 1) for c in mean])
        # torch: torch.from_numpy(frame.copy()).cuda(non_blocking=True)


@ue.on_stream("audio")
def audio(frame):
    # (frames, channels) float32 at frame.meta["sample_rate"]
    if np is not None and _every("audio", 1.0):
        rms = float(np.sqrt(np.mean(np.square(frame.array))))
        ue.emit("audio_level", rms, frame.meta.get("sample_rate"))


@ue.on_stream("observations")
def observations(frame):
    # (rows, features) float32; frame.rows() gives dicts by feature name
    if _every("observations", 2.0):
        ue.log("observation", frame.rows()[-1])


@ue.on_stream("agent")
def agent(frame):
    # Lockstep: Unreal's Step waits for this return value
    row = frame.rows()[-1]
    return {"force": -2.0 * row.get("x", 0.0) - 0.5 * row.get("v", 0.0)}
