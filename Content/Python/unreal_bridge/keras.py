"""Keras callbacks for MLPluginAPI scripts (Keras 3, any backend; tf.keras works too).

    model.fit(x, y, epochs=5, callbacks=[ue.keras.StopOnRequest(self), ue.keras.ProgressEvents(self)])

StopOnRequest ends fit() after the current batch once api.should_stop is set (StopTraining, script stop).
ProgressEvents sends the logs to the component's OnTrainingProgress after every epoch (and every N batches
if asked), e.g. {"epoch": 2, "loss": 0.08, "accuracy": 0.97}.
"""


def _keras():
    try:
        import keras
        return keras
    except ImportError:
        from tensorflow import keras  # older TF without standalone Keras
        return keras


def _floats(logs):
    out = {}
    for k, v in (logs or {}).items():
        try:
            out[k] = float(v)
        except (TypeError, ValueError):
            pass
    return out


def StopOnRequest(api):
    """A callback that stops training when api.should_stop becomes true."""
    keras = _keras()

    class _StopOnRequest(keras.callbacks.Callback):
        def on_train_batch_end(self, batch, logs=None):
            if api.should_stop:
                self.model.stop_training = True

        def on_epoch_end(self, epoch, logs=None):
            if api.should_stop:
                self.model.stop_training = True

    return _StopOnRequest()


def ProgressEvents(api, every_n_batches=0):
    """A callback that reports progress: epoch logs, plus batch logs every N batches when N > 0."""
    keras = _keras()

    class _ProgressEvents(keras.callbacks.Callback):
        def __init__(self):
            super().__init__()
            self.epoch = 0

        def on_epoch_begin(self, epoch, logs=None):
            self.epoch = epoch

        def on_train_batch_end(self, batch, logs=None):
            if every_n_batches and batch % every_n_batches == 0:
                api.report_progress({"epoch": self.epoch, "batch": batch, **_floats(logs)})

        def on_epoch_end(self, epoch, logs=None):
            api.report_progress({"epoch": epoch, "epoch_end": True, **_floats(logs)})

    return _ProgressEvents()
