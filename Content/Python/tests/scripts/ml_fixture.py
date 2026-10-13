"""ML API fixture (snake_case, no ML framework needed) for the harness and PythonCLI.ML.* tests."""
import time

import unreal_bridge as ue
from unreal_bridge.ml import MLPluginAPI


class Fixture(MLPluginAPI):
    def on_setup(self):
        self.setups = getattr(self, "setups", 0) + 1
        self.steps = 400
        self.fail = False
        self.stopped_calls = 0
        self.model = {"weights": [0.5, 0.5]}

    def on_json_input(self, data):
        return {"sum": data["a"] + data["b"], "setups": self.setups}

    def on_float_array_input(self, values):
        return [v * 2 for v in values]

    def on_tensor_input(self, x):
        values = x.tolist()
        flat = [v for row in values for v in (row if isinstance(row, list) else [row])]
        return {"mean": sum(flat) / len(flat), "shape": list(x.shape)}

    def on_begin_training(self):
        done = 0
        for step in range(self.steps):
            if self.should_stop:
                break
            if self.fail and step == 3:
                raise ValueError("training failed on purpose")
            time.sleep(0.005)
            done += 1
            if step % 10 == 0:
                self.report_progress({"step": step, "loss": 1.0 / (step + 1)})
            if step == 5:
                self.call_event("milestone", {"step": step})
                self.call_event("note", "plain text")
        return {"steps": done, "model": self.model, "object": object()}

    def on_stop_training(self):
        self.stopped_calls += 1

    # custom functions (CallCustomFunction)
    def configure(self, steps, fail=False):
        self.steps = steps
        self.fail = fail
        return True

    def scale(self, x, factor=2):
        return x * factor

    def state(self):
        return {"setups": self.setups, "stopped_calls": self.stopped_calls, "should_stop": self.should_stop}


def get_api():
    return Fixture.get_instance()
