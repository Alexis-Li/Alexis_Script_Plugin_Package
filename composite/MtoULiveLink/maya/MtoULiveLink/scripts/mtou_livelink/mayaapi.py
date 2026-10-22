"""Maya bindings shared by the adapter modules.

Each module imports the names it uses, so a module that never touches Maya stays
importable outside Maya and every consumer reads the binding at call time.
"""

try:
    import maya.api.OpenMaya as om
    import maya.api.OpenMayaAnim as oma
    import maya.cmds as cmds
except ImportError:
    om = None
    oma = None
    cmds = None

try:
    import maya.OpenMayaUI as omui
except ImportError:
    omui = None

try:
    import maya.utils as utils
except ImportError:
    utils = None


def run_deferred(action, fallback):
    """Run ``action`` later on Maya's main thread; safe from any thread.

    ``cmds.evalDeferred`` called from a worker thread is silently dropped by an
    interactive Maya session, so a statement Unreal sent over the socket would
    never reach the panel. ``maya.utils.executeDeferred`` queues it from any
    thread. ``fallback`` (normally ``cmds.evalDeferred``) is used where
    ``maya.utils`` is unavailable.
    """
    if utils is not None:
        utils.executeDeferred(action)
    else:
        fallback(action)
