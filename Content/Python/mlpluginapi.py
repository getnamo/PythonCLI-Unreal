"""Compatibility module for ml-remote-server scripts (`from mlpluginapi import MLPluginAPI`)."""
from unreal_bridge.ml import MLPluginAPI  # noqa: F401


def get_api():
    return MLPluginAPI.get_instance()
