"""Public Maya entry point for Flatten Mesh To UV."""

import maya.cmds as cmds


PLUGIN_NAME = "FlattenMeshToUV.py"


def ensure_loaded():
    """Load the command plug-in once and return its registered path."""
    if not cmds.pluginInfo(PLUGIN_NAME, query=True, loaded=True):
        return cmds.loadPlugin(PLUGIN_NAME, quiet=True)
    return PLUGIN_NAME


def run(scale=1.0, output_name=None, uv_set=None):
    """Flatten the selected mesh by its UVs through the public command."""
    ensure_loaded()
    arguments = {"scale": float(scale)}
    if output_name is not None:
        arguments["name"] = output_name
    if uv_set is not None:
        arguments["uvSet"] = uv_set
    return cmds.flattenMeshToUV(**arguments)
