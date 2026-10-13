"""The ML plugin API: what a Python ML component (UPythonMLComponent, UTensorFlowComponent, ...) talks to.

    import unreal_bridge as ue
    from unreal_bridge.ml import MLPluginAPI

    class Classifier(MLPluginAPI):
        def on_setup(self):                    # once, before anything else (load or build a model)
            ...
        def on_json_input(self, data):         # SendJsonInput -> OnInputResults
            return {"prediction": 3}
        def on_tensor_input(self, x):          # SendTensorInput: a numpy array, zero-copy
            ...
        def on_begin_training(self):           # BeginTraining: runs on its own thread
            for epoch in range(10):
                if self.should_stop:           # StopTraining, script stop/reload, EndPlay
                    break
                self.report_progress({"epoch": epoch})   # -> OnTrainingProgress
            return {"accuracy": 0.99}          # -> OnTrainingComplete
        def on_stop_training(self): ...        # optional, called by StopTraining

    def get_api():
        return Classifier.get_instance()

Both snake_case (ml-remote-server) and camelCase (TFPluginAPI, onJsonInput, shouldStop, callEvent,
getApi) work. call_event(name, data) reaches the component's OnCustomEvent.
"""
import json
import traceback

from . import _host

__all__ = ["MLPluginAPI", "TFPluginAPI"]


class MLPluginAPI:
    """Subclass and override the on_* methods you need; get_api() returns the instance Unreal uses."""

    @classmethod
    def get_instance(cls):
        return cls()

    getInstance = get_instance

    def __init__(self):
        self._stop_requested = False
        self.should_retrain = False
        self.should_train_on_start = True  # kept for old scripts; the component's bTrainOnBeginPlay decides
        self.stored = {}

    # -- state ---------------------------------------------------------------------------------------
    @property
    def should_stop(self):
        """True once StopTraining was called (until the next BeginTraining), or the script is stopping."""
        if self._stop_requested:
            return True
        ctx = _host.current_context()
        return ctx is not None and ctx.stopping.is_set()

    @should_stop.setter
    def should_stop(self, value):
        self._stop_requested = bool(value)

    shouldStop = should_stop

    @property
    def shouldRetrain(self):
        return self.should_retrain

    @shouldRetrain.setter
    def shouldRetrain(self, value):
        self.should_retrain = value

    # -- to Unreal -----------------------------------------------------------------------------------
    def call_event(self, event, data=None, use_json=False):
        """OnCustomEvent(event, data) on the component. Strings arrive as they are, other values as JSON."""
        _emit("_ml_event", str(event), _jsonable(data))

    def callEvent(self, event, data=None, useJson=False):
        self.call_event(event, data, useJson)

    def report_progress(self, data):
        """OnTrainingProgress(json) on the component, e.g. {"epoch": 3, "loss": 0.12}."""
        _emit("_ml_progress", _jsonable(data))

    def reportProgress(self, data):
        self.report_progress(data)

    # -- hooks (override) ----------------------------------------------------------------------------
    def on_setup(self):
        pass

    def on_json_input(self, json_input):
        raise NotImplementedError(f"{type(self).__name__} doesn't implement on_json_input")

    def on_float_array_input(self, float_array_input):
        raise NotImplementedError(f"{type(self).__name__} doesn't implement on_float_array_input")

    def on_tensor_input(self, tensor):
        raise NotImplementedError(f"{type(self).__name__} doesn't implement on_tensor_input")

    def on_begin_training(self):
        pass

    def on_stop_training(self):
        pass

    def on_save_model(self, model_name):
        raise NotImplementedError(f"{type(self).__name__} doesn't implement on_save_model")

    def on_load_model(self, model_name):
        raise NotImplementedError(f"{type(self).__name__} doesn't implement on_load_model")


class TFPluginAPI(MLPluginAPI):
    """The TensorFlow-Unreal 1.x name, camelCase hooks (onSetup, onJsonInput, onBeginTraining, ...).
    snake_case hooks work here too."""


# hook name -> camelCase alias (old TFPluginAPI scripts)
_ALIASES = {
    "on_setup": "onSetup",
    "on_json_input": "onJsonInput",
    "on_float_array_input": "onFloatArrayInput",
    "on_tensor_input": "onTensorInput",
    "on_begin_training": "onBeginTraining",
    "on_stop_training": "onStopTraining",
    "on_save_model": "onSaveModel",
    "on_load_model": "onLoadModel",
}


def _hook(api, name):
    """The script's implementation of a hook: its snake_case override, else its camelCase one, else the
    default."""
    cls = type(api)
    own = getattr(cls, name, None)
    if own is not None and own is not getattr(MLPluginAPI, name, None):
        return getattr(api, name)
    camel = _ALIASES.get(name)
    if camel and callable(getattr(api, camel, None)):
        return getattr(api, camel)
    return getattr(api, name)


def _method(api, name):
    """A custom function by name, trying the snake_case/camelCase spelling too."""
    fn = getattr(api, name, None)
    if callable(fn):
        return fn
    alt = _ALIASES.get(name) or next((k for k, v in _ALIASES.items() if v == name), None)
    fn = getattr(api, alt, None) if alt else None
    if callable(fn):
        return fn
    raise AttributeError(f"{type(api).__name__} has no function '{name}'")


def _emit(name, *args):
    ctx = _host.current_context()
    if ctx is None:
        raise RuntimeError("unreal_bridge.ml: not called from a script started by Unreal")
    ctx.host.emit(ctx, name, args)


def _jsonable(value, depth=0):
    """Something Unreal can receive: JSON values, bytes and arrays pass; numpy scalars become numbers; other
    objects (a Keras model in a training summary) become their repr."""
    if value is None or isinstance(value, (bool, int, float, str, bytes, bytearray)):
        return value
    if hasattr(value, "item") and getattr(value, "ndim", None) == 0:  # numpy scalar / 0-d array
        return value.item()
    if hasattr(value, "dtype") and hasattr(value, "shape"):  # arrays / ue.Tensor: sent as tensors
        return value
    if depth < 8 and isinstance(value, dict):
        return {str(k): _jsonable(v, depth + 1) for k, v in value.items()}
    if depth < 8 and isinstance(value, (list, tuple)):
        return [_jsonable(v, depth + 1) for v in value]
    try:
        json.dumps(value)
        return value
    except (TypeError, ValueError):
        text = repr(value)
        return text if len(text) <= 200 else text[:197] + "..."


class Adapter:
    """Runs a script's ML API for its component: the reserved _ml_* calls land here (on the script's worker
    thread); training runs on a background thread of the script."""

    def __init__(self, ctx):
        self.ctx = ctx
        self.api = None
        self.set_up = False
        self.training = None

    def get_api(self):
        if self.api is None:
            module = self.ctx.module
            factory = getattr(module, "get_api", None) or getattr(module, "getApi", None)
            if not callable(factory):
                raise RuntimeError(f"{self.ctx.script} has no get_api() (or getApi()) returning its MLPluginAPI instance")
            api = factory()
            if not isinstance(api, MLPluginAPI):
                raise TypeError(f"get_api() returned {type(api).__name__}; subclass unreal_bridge.ml.MLPluginAPI (or TFPluginAPI)")
            self.api = api
        if not self.set_up:
            self.set_up = True
            _hook(self.api, "on_setup")()
        return self.api

    # -- the reserved calls (UPythonMLComponent) ------------------------------------------------------
    def _ml_setup(self):
        self.get_api()
        return {"ready": True}

    def _ml_json_input(self, data, function=None):
        api = self.get_api()
        fn = _method(api, function) if function else _hook(api, "on_json_input")
        return fn(data)

    def _ml_float_input(self, values, function=None):
        api = self.get_api()
        if hasattr(values, "tolist"):
            values = values.tolist()  # the old API took a list of floats
        fn = _method(api, function) if function else _hook(api, "on_float_array_input")
        return fn(values)

    def _ml_tensor_input(self, tensor, function=None):
        api = self.get_api()
        fn = _method(api, function) if function else _hook(api, "on_tensor_input")
        return fn(tensor)

    def _ml_custom(self, function, *args):
        return _method(self.get_api(), function)(*args)

    def _ml_save_model(self, name):
        return _hook(self.get_api(), "on_save_model")(name)

    def _ml_load_model(self, name):
        return _hook(self.get_api(), "on_load_model")(name)

    def _ml_begin_training(self):
        api = self.get_api()
        if self.training is not None and self.training.is_alive():
            return {"started": False, "reason": "already training"}
        api._stop_requested = False
        ctx = self.ctx

        def train():
            try:
                ctx.host.emit(ctx, "_ml_training_started", ())  # from this thread: always before its completion
                result = _hook(api, "on_begin_training")()
                ctx.host.emit(ctx, "_ml_training_complete", (_jsonable(result), bool(api.should_stop)))
            except Exception as e:  # noqa: BLE001
                tb = traceback.format_exc()
                ctx.host.log("error", f"{ctx.script}: training failed:\n{tb}", ctx)
                ctx.host.emit(ctx, "_ml_training_error", (f"{type(e).__name__}: {e}", tb))

        self.training = ctx.run_in_background(train)
        return {"started": True}

    def _ml_stop_training(self):
        api = self.get_api()
        api._stop_requested = True
        _hook(api, "on_stop_training")()
        return {"stopped": True, "training": self.training is not None and self.training.is_alive()}
