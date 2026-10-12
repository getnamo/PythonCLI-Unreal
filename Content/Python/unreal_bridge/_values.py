"""Converting event arguments between JSON + binary table and Python values.

On the wire, bytes are {"_bin": i} and tensors are {"_tensor": i, "dtype": "float32", "shape": [2, 3]}
(row-major, little-endian), where i indexes the frame's binary table. Tensors arrive as numpy arrays
when numpy is installed (zero-copy views over the frame), else as unreal_bridge.Tensor.
"""
import array
import sys

_np = None
_np_checked = False
_np_error = None


def _numpy():
    """numpy if it's installed (imported on first use, after the runtime has added the venv's site dirs)."""
    global _np, _np_checked, _np_error
    if not _np_checked:
        _np_checked = True
        try:
            import numpy
            _np = numpy
        except ModuleNotFoundError:
            _np = None  # optional: tensors fall back to unreal_bridge.Tensor
        except Exception as e:  # noqa: BLE001 - installed but broken: worth reporting
            _np, _np_error = None, f"{type(e).__name__}: {e}"
    return _np


def numpy_error():
    _numpy()
    return _np_error

DTYPES = ("float32", "float64", "float16", "int8", "uint8", "int16", "uint16", "int32", "uint32", "int64", "uint64", "bool")
_ITEMSIZE = {"float32": 4, "float64": 8, "float16": 2, "int8": 1, "uint8": 1, "int16": 2, "uint16": 2,
             "int32": 4, "uint32": 4, "int64": 8, "uint64": 8, "bool": 1}
_TYPECODE = {"float32": "f", "float64": "d", "int8": "b", "uint8": "B", "int16": "h", "uint16": "H",
             "int32": "i", "uint32": "I", "int64": "q", "uint64": "Q", "bool": "B"}


class Tensor:
    """Minimal tensor used when numpy isn't installed: raw little-endian bytes plus dtype and shape."""

    __slots__ = ("data", "dtype", "shape")

    def __init__(self, data, dtype="float32", shape=None):
        if dtype not in _ITEMSIZE:
            raise ValueError(f"unsupported dtype {dtype!r}")
        self.data = memoryview(data).cast("B")
        self.dtype = dtype
        count = len(self.data) // _ITEMSIZE[dtype]
        self.shape = tuple(shape) if shape is not None else (count,)

    @classmethod
    def from_list(cls, values, dtype="float32", shape=None):
        return cls(array.array(_TYPECODE[dtype], values).tobytes(), dtype, shape)

    @property
    def nbytes(self):
        return len(self.data)

    def tolist(self):
        """Flat list of values (float16 isn't supported without numpy)."""
        a = array.array(_TYPECODE[self.dtype])
        a.frombytes(bytes(self.data))
        if sys.byteorder != "little":
            a.byteswap()
        return a.tolist()

    def __repr__(self):
        return f"Tensor(dtype={self.dtype}, shape={self.shape}, nbytes={self.nbytes})"


def decode(value, buffers):
    """Replace placeholders in a decoded JSON value with bytes / tensors."""
    if isinstance(value, dict):
        if len(value) == 1 and isinstance(value.get("_bin"), int):
            return buffers[value["_bin"]]
        if isinstance(value.get("_tensor"), int) and "dtype" in value:
            return _make_tensor(buffers[value["_tensor"]], value["dtype"], value.get("shape"))
        return {k: decode(v, buffers) for k, v in value.items()}
    if isinstance(value, list):
        return [decode(v, buffers) for v in value]
    return value


def _make_tensor(buf, dtype, shape):
    _numpy()
    if _np is not None:
        arr = _np.frombuffer(buf, dtype=_np.dtype(dtype).newbyteorder("<"))
        return arr.reshape(shape) if shape is not None else arr
    return Tensor(buf, dtype, shape)


def encode(value, buffers):
    """Replace bytes / tensors with placeholders, appending their data to `buffers`."""
    _numpy()
    if isinstance(value, (bytes, bytearray, memoryview)):
        buffers.append(bytes(value))
        return {"_bin": len(buffers) - 1}
    if isinstance(value, Tensor):
        buffers.append(bytes(value.data))
        return {"_tensor": len(buffers) - 1, "dtype": value.dtype, "shape": list(value.shape)}
    if _np is not None and isinstance(value, _np.ndarray):
        name = value.dtype.name
        if name not in _ITEMSIZE:
            raise TypeError(f"cannot send numpy dtype {name} to Unreal (use one of {', '.join(DTYPES)})")
        arr = _np.ascontiguousarray(value, dtype=value.dtype.newbyteorder("<"))
        buffers.append(arr.tobytes())
        return {"_tensor": len(buffers) - 1, "dtype": name, "shape": list(arr.shape)}
    if _np is not None and isinstance(value, _np.generic):
        return value.item()
    if isinstance(value, dict):
        return {str(k): encode(v, buffers) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [encode(v, buffers) for v in value]
    if hasattr(value, "__torch_function__") and hasattr(value, "detach"):  # torch.Tensor, without importing torch
        return encode(value.detach().cpu().numpy(), buffers)
    return value


def numpy_version():
    np = _numpy()
    return np.__version__ if np is not None else None
