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
