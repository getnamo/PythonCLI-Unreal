"""Compatibility module for scripts written for ml-remote-server / UnrealEnginePython (`import unreal_engine as ue`).
New scripts use unreal_bridge."""
import os

import unreal_bridge as _ue


def log(*parts):
    _ue.log(*parts)


def log_warning(*parts):
    _ue.warning(*parts)


def log_error(*parts):
    _ue.error(*parts)


def get_content_dir():
    """The project's Content directory, with a trailing separator (old scripts append to it)."""
    return os.path.join(_ue.content_dir, "")


def run_on_gt(callback, param=None):
    """Old API: ran a callback on the game thread. Python never runs on Unreal's game thread now, so it's
    called right away; use unreal_bridge.emit (or MLPluginAPI.call_event) to reach Unreal."""
    if callback is not None:
        return callback(param)
