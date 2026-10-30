"""MtoU_LiveLink Maya entry point.

Install by dragging the component's `install.mel` into a Maya viewport, which
registers the component as a Maya module, or run this file in Maya's Script
Editor with its `mtou_livelink` package beside it. The file keeps the
historical entry points `run()` and `main()`.
"""

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

INCOMPLETE_INSTALL = (
    "MtoU_LiveLink 安装不完整：找不到 {0}。请重新完整解压 Maya 组件，把其中的 "
    "install.mel 拖入 Maya 视口重新安装，然后重启 Maya。\n"
    "Incomplete MtoU_LiveLink install: {0} is missing. Extract the whole Maya "
    "component again, drag its install.mel into a Maya viewport, then restart Maya.")

try:
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
except ImportError as _error:
    _missing = getattr(_error, "name", None) or ""
    if not _missing.startswith("mtou_livelink"):
        raise
    raise ImportError(INCOMPLETE_INSTALL.format(_missing), name=_missing) from _error


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
