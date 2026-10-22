"""Maya bindings shared by the adapter modules.

Each module imports the names it uses, so a module that never touches Maya stays
importable outside Maya and every consumer reads the binding at call time.
"""

import collections

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

# Deferred work of a Maya without a UI event loop (mayapy, batch): a host loop
# runs it on its own thread through drain_deferred().
_batch_queue = collections.deque()
_batch_mode = None


def _in_batch_mode():
    """True in a Maya without an idle loop; read once, on the importing thread."""
    global _batch_mode
    if _batch_mode is None:
        try:
            value = cmds.about(batch=True)
        except Exception:  # cmds missing or not initialized yet
            return False
        if not isinstance(value, bool):
            return False
        _batch_mode = value
    return _batch_mode


_in_batch_mode()


def run_deferred(action, fallback):
    """Run ``action`` later on Maya's main thread; safe from any thread.

    ``cmds.evalDeferred`` called from a worker thread is silently dropped by an
    interactive Maya session, so a statement Unreal sent over the socket would
    never reach the panel. ``maya.utils.executeDeferred`` queues it from any
    thread. ``fallback`` (normally ``cmds.evalDeferred``) is used where
    ``maya.utils`` is unavailable.

    mayapy has no idle loop, so ``executeDeferred`` would hold the action
    forever: in batch mode it waits in a queue the host loop empties with
    :func:`drain_deferred`.
    """
    if _in_batch_mode():
        _batch_queue.append(action)
    elif utils is not None:
        utils.executeDeferred(action)
    else:
        fallback(action)


def drain_deferred():
    """Run the work queued in batch mode on the calling (main) thread.

    Returns the number of actions run; an interactive Maya always returns 0.
    """
    ran = 0
    while True:
        try:
            action = _batch_queue.popleft()
        except IndexError:
            return ran
        action()
        ran += 1
