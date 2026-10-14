"""MtoU_LiveLink Maya entry point.

Run this file in Maya's Script Editor or install the whole `scripts` directory so
that `MtoULiveLink.py` and its `mtou_livelink` package stay together. The file
keeps the historical entry points `run()` and `main()`.
"""

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from mtou_livelink import controller  # noqa: E402
from mtou_livelink.cache import PlaybackCache  # noqa: E402
from mtou_livelink.config import PROTOCOL_VERSION, __version__  # noqa: E402
from mtou_livelink.mayaapi import cmds  # noqa: E402
from mtou_livelink.scene import require_maya  # noqa: E402
from mtou_livelink.controller import (  # noqa: E402
    DIAGNOSTIC_WINDOW_NAME,
    WINDOW_NAME,
    Controller,
)


def run():
    require_maya()
    try:
        PlaybackCache.cleanup_stale()
    except (OSError, TypeError, ValueError):
        pass
    if cmds.window(WINDOW_NAME, exists=True):
        cmds.showWindow(WINDOW_NAME)
        cmds.window(WINDOW_NAME, edit=True, restoreCommand="")
        return controller.CONTROLLER
    controller.CONTROLLER = Controller()
    controller.CONTROLLER.build_ui()
    return controller.CONTROLLER


def main():
    return run()


if __name__ == "__main__":
    main()
