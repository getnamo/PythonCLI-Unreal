"""Tensors in and out. Call Python Function "stats" with a tensor (e.g. Make Tensor From Floats) -> JSON stats;
"normalize" returns a float32 tensor. Uses numpy when the venv has it (Project Settings > Python CLI >
Requirements: numpy), else unreal_bridge.Tensor.
"""
import unreal_bridge as ue

try:
    import numpy as np
except ImportError:
    np = None


def stats(t):
    if np is not None:
        a = np.asarray(t, dtype=np.float64)
        return {"shape": list(a.shape), "mean": float(a.mean()), "min": float(a.min()), "max": float(a.max())}
    v = t.tolist()
    return {"shape": list(t.shape), "mean": sum(v) / len(v), "min": min(v), "max": max(v)}


def normalize(t):
    if np is not None:
        a = np.asarray(t, dtype=np.float32)
        span = float(a.max() - a.min()) or 1.0
        return (a - a.min()) / span
    v = t.tolist()
    lo, hi = min(v), max(v)
    span = (hi - lo) or 1.0
    return ue.Tensor.from_list([(x - lo) / span for x in v], "float32", t.shape)
