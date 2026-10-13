"""An old-style TensorFlow-Unreal 1.x / UnrealEnginePython script (camelCase API, unreal_engine module),
unchanged apart from the import of TFPluginAPI."""
import time

import unreal_engine as ue
from unreal_bridge.ml import TFPluginAPI


class ExampleAPI(TFPluginAPI):
    def onSetup(self):
        self.op = "+"
        ue.log("legacy setup, content dir " + ue.get_content_dir())

    def onJsonInput(self, jsonInput):
        a, b = jsonInput["a"], jsonInput["b"]
        return {"c": a + b if self.op == "+" else a - b}

    def changeOperation(self, type):
        self.op = type

    def onBeginTraining(self):
        for i in range(500):
            if self.shouldStop:
                break
            if i == 2:
                self.callEvent("TrainingUpdateEvent", {"i": i}, True)
            time.sleep(0.005)
        return {"done": True}


def getApi():
    return ExampleAPI.getInstance()
