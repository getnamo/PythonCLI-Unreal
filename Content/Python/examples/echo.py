"""Smallest PythonCLI script. In a Python component set Script = "examples/echo.py", then:
  EmitEvent("echo", "[1, \"hi\"]")    -> OnEvent("echoed", [1, "hi"])
  Call Python Function "add" "[2, 3]" -> 5
"""
import unreal_bridge as ue


def on_start():
    ue.log("echo.py started with args", ue.args)


@ue.on("echo")
def echo(*args):
    ue.emit("echoed", *args)


def add(a, b):
    return a + b
