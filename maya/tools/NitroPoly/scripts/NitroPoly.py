# -*- coding: utf-8 -*-
from __future__ import unicode_literals

import math
import re
import traceback
from collections import OrderedDict
from contextlib import contextmanager
from functools import partial

import maya.cmds as cmds
import maya.mel as mel

try:
    import maya.OpenMayaUI as omui
    try:
        from PySide6 import QtCore, QtWidgets
        import shiboken6 as shiboken
    except ImportError:
        from PySide2 import QtCore, QtWidgets
        import shiboken2 as shiboken
    _QT_AVAILABLE = True
except ImportError:
    omui = None
    QtCore = None
    QtWidgets = None
    shiboken = None
    _QT_AVAILABLE = False

__author__ = "Alexis_Lee"
__version__ = "4.0.4"

WINDOW_NAME = "NitroPolyFloatingWindow"
WINDOW_TITLE = "NitroPoly " + __version__
_INSTANCE = None


class NitroPolyError(RuntimeError):
    pass


if _QT_AVAILABLE:
    class _HoverHelpFilter(QtCore.QObject):
        def __init__(self, callback, tool_id, parent=None):
            super(_HoverHelpFilter, self).__init__(parent)
            self.callback = callback
            self.tool_id = tool_id

        def eventFilter(self, watched, event):
            if event.type() == QtCore.QEvent.Enter:
                self.callback(self.tool_id)
            return False
else:
    class _HoverHelpFilter(object):
        pass


def _spec(tool_id, label, category, method, help_text, hotkey=None, tone="normal"):
    return {
        "id": tool_id,
        "label": label,
        "category": category,
        "method": method,
        "help": help_text,
        "hotkey": hotkey,
        "tone": tone,
    }


TOOL_SPECS = [
    _spec("grow_loop", "\u5faa\u73af\u6269\u5c55", "\u7f16\u8f91\u9009\u62e9", "grow_loop",
          "\u7528\u9014\uff1a\u6cbf\u5f53\u524d\u5faa\u73af\u8fb9\u6216\u8fb9\u754c\u94fe\u5411\u4e24\u7aef\u6269\u5c55\u4e00\u6b65\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u8fde\u7eed\u5faa\u73af\u8fb9\u7684\u4e00\u90e8\u5206\u540e\u6267\u884c\u3002",
          "NitroPoly_GrowLoop", "plus"),
    _spec("shrink_loop", "\u5faa\u73af\u7f29\u5c0f", "\u7f16\u8f91\u9009\u62e9", "shrink_loop",
          "\u7528\u9014\uff1a\u4ece\u5f53\u524d\u5faa\u73af\u8fb9\u9009\u62e9\u7684\u4e24\u7aef\u5404\u79fb\u9664\u4e00\u6b65\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u81f3\u5c11\u4e09\u6761\u8fde\u7eed\u5faa\u73af\u8fb9\u540e\u6267\u884c\u3002",
          "NitroPoly_ShrinkLoop", "minus"),
    _spec("grow_ring", "\u73af\u5f62\u6269\u5c55", "\u7f16\u8f91\u9009\u62e9", "grow_ring",
          "\u7528\u9014\uff1a\u6cbf\u56db\u8fb9\u9762\u8fb9\u73af\u5411\u4e24\u4fa7\u6269\u5c55\u4e00\u6b65\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u540c\u4e00\u8fb9\u73af\u4e2d\u7684\u4e00\u90e8\u5206\u8fb9\u540e\u6267\u884c\u3002",
          "NitroPoly_GrowRing", "plus"),
    _spec("shrink_ring", "\u73af\u5f62\u7f29\u5c0f", "\u7f16\u8f91\u9009\u62e9", "shrink_ring",
          "\u7528\u9014\uff1a\u4ece\u5f53\u524d\u8fb9\u73af\u9009\u62e9\u7684\u4e24\u7aef\u5404\u79fb\u9664\u4e00\u6b65\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u81f3\u5c11\u4e09\u6761\u540c\u4e00\u8fb9\u73af\u4e2d\u7684\u8fb9\u540e\u6267\u884c\u3002",
          "NitroPoly_ShrinkRing", "minus"),
    _spec("dot_loop", "\u5faa\u73af\u95f4\u9694", "\u7f16\u8f91\u9009\u62e9", "dot_loop",
          "\u7528\u9014\uff1a\u6309\u95f4\u9694\u503c\u9009\u62e9\u5faa\u73af\u8fb9\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u4e00\u6761\u5faa\u73af\u8fb9\u4f5c\u4e3a\u8d77\u70b9\uff0c\u8bbe\u7f6e\u95f4\u9694\u540e\u6267\u884c\u3002",
          "NitroPoly_DotLoop", "plus"),
    _spec("dot_ring", "\u73af\u5f62\u95f4\u9694", "\u7f16\u8f91\u9009\u62e9", "dot_ring",
          "\u7528\u9014\uff1a\u6309\u95f4\u9694\u503c\u9009\u62e9\u8fb9\u73af\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u4e00\u6761\u8fb9\u73af\u8fb9\u4f5c\u4e3a\u8d77\u70b9\uff0c\u8bbe\u7f6e\u95f4\u9694\u540e\u6267\u884c\u3002",
          "NitroPoly_DotRing", "minus"),
    _spec("hard_edge", "\u9009\u62e9\u786c\u8fb9", "\u7f16\u8f91\u9009\u62e9", "hard_edge",
          "\u7528\u9014\uff1a\u9009\u51fa\u6240\u9009\u6a21\u578b\u4e0a\u7684\u786c\u8fb9\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u6a21\u578b\u6216\u6a21\u578b\u7ec4\u4ef6\u540e\u6267\u884c\u3002",
          "NitroPoly_SelecthardEdge"),
    _spec("uv_edge", "\u9009\u62e9UV\u8fb9", "\u7f16\u8f91\u9009\u62e9", "uv_edge",
          "\u7528\u9014\uff1a\u9009\u51fa UV \u63a5\u7f1d\u8fb9\uff0c\u4e0d\u5305\u542b\u666e\u901a\u51e0\u4f55\u8fb9\u754c\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u6a21\u578b\u6216\u6a21\u578b\u7ec4\u4ef6\u540e\u6267\u884c\u3002",
          "NitroPoly_SelectUVEdge"),
    _spec("point_to_point", "\u70b9\u5230\u70b9", "\u7f16\u8f91\u9009\u62e9", "point_to_point",
          "\u7528\u9014\uff1a\u9009\u62e9\u4e24\u4e2a\u9876\u70b9\u95f4\u7684\u6700\u77ed\u8fb9\u8def\u5f84\u3002\n\u4f7f\u7528\uff1a\u5728\u540c\u4e00\u6a21\u578b\u4e0a\u9009\u62e9\u4e24\u4e2a\u9876\u70b9\u540e\u6267\u884c\u3002",
          "NitroPoly_PointToPoint"),
    _spec("face_fill", "\u586b\u5145\u9762", "\u7f16\u8f91\u9009\u62e9", "face_fill",
          "\u7528\u9014\uff1a\u9009\u62e9\u4e24\u4e2a\u9762\u4e4b\u95f4\u7684\u6700\u77ed\u8fde\u7eed\u9762\u8def\u5f84\u3002\n\u4f7f\u7528\uff1a\u5728\u540c\u4e00\u6a21\u578b\u4e0a\u9009\u62e9\u4e24\u4e2a\u9762\u540e\u6267\u884c\u3002",
          "NitroPoly_FaceFill"),

    _spec("combine_clean", "\u6e05\u6d01\u5408\u5e76", "\u7f51\u683c\u7f16\u8f91", "combine_clean",
          "\u7528\u9014\uff1a\u5408\u5e76\u591a\u4e2a\u591a\u8fb9\u5f62\u6a21\u578b\u5e76\u5220\u9664\u5386\u53f2\uff0c\u4fdd\u7559\u7b2c\u4e00\u4e2a\u6a21\u578b\u7684\u540d\u79f0\u3001\u7236\u7ea7\u548c\u8f74\u5fc3\u3002\n\u4f7f\u7528\uff1a\u5bf9\u8c61\u6a21\u5f0f\u9009\u62e9\u4e24\u4e2a\u6216\u66f4\u591a\u6a21\u578b\u540e\u6267\u884c\u3002",
          "NitroPoly_CleanCombine", "plus"),
    _spec("detach_clean", "\u6e05\u6d01\u5206\u79bb", "\u7f51\u683c\u7f16\u8f91", "detach_clean",
          "\u7528\u9014\uff1a\u628a\u9009\u4e2d\u9762\u63d0\u53d6\u4e3a\u72ec\u7acb\u6a21\u578b\uff0c\u540c\u65f6\u4ece\u539f\u6a21\u578b\u5220\u9664\u8fd9\u4e9b\u9762\u3002\n\u4f7f\u7528\uff1a\u5728\u4e00\u4e2a\u6a21\u578b\u4e0a\u9009\u62e9\u90e8\u5206\u9762\u540e\u6267\u884c\u3002",
          "NitroPoly_CleanDetach", "minus"),
    _spec("uni_connect", "\u7b80\u5316\u8fde\u63a5", "\u7f51\u683c\u7f16\u8f91", "uni_connect",
          "\u7528\u9014\uff1a\u6309\u5f53\u524d\u9009\u62e9\u7c7b\u578b\u6267\u884c\u8fde\u63a5\uff1b\u5bf9\u8c61\u6a21\u5f0f\u8fdb\u5165 Multi-Cut\uff0c\u9876\u70b9\u6216\u8fb9\u6267\u884c\u8fde\u63a5\uff0c\u9762\u6267\u884c\u539f\u4f4d\u6324\u51fa\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u5bf9\u8c61\u3001\u9876\u70b9\u3001\u8fb9\u6216\u9762\u540e\u6267\u884c\u3002",
          "NitroPoly_UniConnect", "plus"),
    _spec("uni_remove", "\u7b80\u5316\u79fb\u9664", "\u7f51\u683c\u7f16\u8f91", "uni_remove",
          "\u7528\u9014\uff1a\u6309\u5f53\u524d\u9009\u62e9\u7c7b\u578b\u6267\u884c\u5220\u9664\u3001\u5408\u5e76\u6216\u584c\u9677\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u5bf9\u8c61\u3001\u9876\u70b9\u3001\u8fb9\u6216\u9762\u540e\u6267\u884c\u3002",
          "NitroPoly_uniRemove", "minus"),

    _spec("base_pivot", "\u5230\u5e95\u90e8", "\u8f74\u5fc3\u70b9/\u89e3\u51bb\u53d8\u6362", "base_pivot",
          "\u7528\u9014\uff1a\u628a\u6a21\u578b\u8f74\u5fc3\u79fb\u5230\u4e16\u754c\u5305\u56f4\u76d2\u5e95\u90e8\u4e2d\u5fc3\u3002\n\u4f7f\u7528\uff1a\u5bf9\u8c61\u6a21\u5f0f\u9009\u62e9\u4e00\u4e2a\u6216\u591a\u4e2a\u6a21\u578b\u540e\u6267\u884c\u3002",
          "NitroPoly_basePivot"),
    _spec("world_pivot", "\u5230\u539f\u70b9", "\u8f74\u5fc3\u70b9/\u89e3\u51bb\u53d8\u6362", "world_pivot",
          "\u7528\u9014\uff1a\u628a\u6240\u9009\u5bf9\u8c61\u8f74\u5fc3\u79fb\u52a8\u5230\u4e16\u754c\u5750\u6807\u539f\u70b9\u3002\n\u4f7f\u7528\uff1a\u5bf9\u8c61\u6a21\u5f0f\u9009\u62e9\u4e00\u4e2a\u6216\u591a\u4e2a\u5bf9\u8c61\u540e\u6267\u884c\u3002",
          "NitroPoly_WorldPivot"),
    _spec("unfreeze_translate", "\u89e3\u51bb\u53d8\u6362", "\u8f74\u5fc3\u70b9/\u89e3\u51bb\u53d8\u6362", "unfreeze_translate",
          "\u7528\u9014\uff1a\u5728\u5c3d\u91cf\u4fdd\u6301\u6a21\u578b\u4e16\u754c\u4f4d\u7f6e\u7684\u524d\u63d0\u4e0b\u6062\u590d\u51bb\u7ed3\u524d\u7684\u4f4d\u79fb\u901a\u9053\u3002\n\u4f7f\u7528\uff1a\u5bf9\u8c61\u6a21\u5f0f\u9009\u62e9\u6a21\u578b\u540e\u6267\u884c\uff1b\u590d\u6742\u7ea6\u675f\u3001\u5b9e\u4f8b\u6216\u9501\u5b9a\u901a\u9053\u5e94\u5148\u5907\u4efd\u3002",
          "NitroPoly_UnFreezeTransform"),
    _spec("move_to_origin", "\u7269\u4f53\u79fb\u5230\u539f\u70b9", "\u8f74\u5fc3\u70b9/\u89e3\u51bb\u53d8\u6362", "move_to_origin",
          "\u7528\u9014\uff1a\u6309\u65cb\u8f6c\u8f74\u5fc3\u628a\u5bf9\u8c61\u79fb\u52a8\u5230\u4e16\u754c\u539f\u70b9\u3002\n\u4f7f\u7528\uff1a\u5bf9\u8c61\u6a21\u5f0f\u9009\u62e9\u4e00\u4e2a\u6216\u591a\u4e2a\u5bf9\u8c61\u540e\u6267\u884c\u3002",
          "NitroPoly_MoveToOrigin"),

    _spec("corner_plus", "\u2220 \u65cb\u8f6c 45\u00b0 +", "\u62d3\u6251\u5de5\u5177", "corner_plus",
          "\u7528\u9014\uff1a\u4ee5\u6240\u9009\u8fb9\u4e3a\u65cb\u8f6c\u8f74\uff0c\u5c06\u6240\u9009\u9762\u533a\u57df\u65cb\u8f6c\u6b63 45\u00b0\u3002\n\u4f7f\u7528\uff1a\u540c\u65f6\u9009\u62e9\u4e00\u6761\u8fb9\u548c\u4e00\u4e2a\u6216\u591a\u4e2a\u9762\u540e\u6267\u884c\u3002",
          "NitroPoly_CornerRot_Plus", "plus"),
    _spec("corner_minus", "\u2220 \u65cb\u8f6c 45\u00b0 -", "\u62d3\u6251\u5de5\u5177", "corner_minus",
          "\u7528\u9014\uff1a\u4ee5\u6240\u9009\u8fb9\u4e3a\u65cb\u8f6c\u8f74\uff0c\u5c06\u6240\u9009\u9762\u533a\u57df\u65cb\u8f6c\u8d1f 45\u00b0\u3002\n\u4f7f\u7528\uff1a\u540c\u65f6\u9009\u62e9\u4e00\u6761\u8fb9\u548c\u4e00\u4e2a\u6216\u591a\u4e2a\u9762\u540e\u6267\u884c\u3002",
          "NitroPoly_CornerRot_Minus", "minus"),
    _spec("f2_extend", "F2 \u6269\u5c55", "\u62d3\u6251\u5de5\u5177", "f2_extend",
          "\u7528\u9014\uff1a\u6309\u76f8\u90bb\u56db\u8fb9\u9762\u65b9\u5411\u4ece\u4e00\u6761\u8fb9\u754c\u8fb9\u5916\u63a8\u65b0\u56db\u8fb9\u9762\uff0c\u5e76\u6309\u9608\u503c\u81ea\u52a8\u710a\u63a5\u9644\u8fd1\u9876\u70b9\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u4e00\u6761\u5c5e\u4e8e\u56db\u8fb9\u9762\u7684\u8fb9\u754c\u8fb9\u540e\u6267\u884c\u3002",
          "NitroPoly_F2Extend"),
    _spec("bevel_plus", "\u5012\u89d2 +", "\u62d3\u6251\u5de5\u5177", "bevel_plus",
          "\u7528\u9014\uff1a\u6309\u7406\u8bba\u5c16\u89d2\u589e\u5927\u8fde\u7eed\u5012\u89d2\u8f6e\u5ed3\u5bbd\u5ea6\uff0c\u8f6e\u5ed3\u4e24\u7aef\u4fdd\u6301\u4e0d\u52a8\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u81f3\u5c11\u4e09\u6761\u8fde\u7eed\u3001\u5f00\u653e\u7684\u5012\u89d2\u8f6e\u5ed3\u8fb9\u540e\u6267\u884c\u3002",
          "NitroPoly_Bevel_Plus", "plus"),
    _spec("bevel_minus", "\u5012\u89d2 -", "\u62d3\u6251\u5de5\u5177", "bevel_minus",
          "\u7528\u9014\uff1a\u6309\u7406\u8bba\u5c16\u89d2\u7f29\u5c0f\u8fde\u7eed\u5012\u89d2\u8f6e\u5ed3\u5bbd\u5ea6\uff0c\u8f6e\u5ed3\u4e24\u7aef\u4fdd\u6301\u4e0d\u52a8\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u81f3\u5c11\u4e09\u6761\u8fde\u7eed\u3001\u5f00\u653e\u7684\u5012\u89d2\u8f6e\u5ed3\u8fb9\u540e\u6267\u884c\u3002",
          "NitroPoly_Bevel_Minus", "minus"),

    _spec("load_edge_loop", "\u52a0\u8f7d\u5faa\u73af\u8fb9", "\u8fde\u63a5\u5de5\u5177", "load_edge_loop",
          "\u7528\u9014\uff1a\u628a\u5f53\u524d\u8fde\u7eed\u8fb9\u94fe\u4fdd\u5b58\u4e3a\u5207\u5272\u548c\u7f1d\u5408\u7684\u53c2\u8003\u8fb9\u94fe\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u53c2\u8003\u8fb9\u94fe\u540e\u6267\u884c\u3002",
          "NitroPoly_LoadEdgeLoop", "plus"),
    _spec("cut_stitch", "\u5207\u5272\u548c\u7f1d\u5408", "\u8fde\u63a5\u5de5\u5177", "cut_stitch",
          "\u7528\u9014\uff1a\u6309\u5df2\u52a0\u8f7d\u53c2\u8003\u8fb9\u94fe\u7684\u4f4d\u7f6e\u5207\u5272\u76ee\u6807\u8fb9\uff0c\u5e76\u5c06\u5207\u70b9\u5438\u9644\u5230\u53c2\u8003\u9876\u70b9\uff1b\u540c\u4e00\u6a21\u578b\u65f6\u81ea\u52a8\u710a\u63a5\u3002\n\u4f7f\u7528\uff1a\u5148\u52a0\u8f7d\u53c2\u8003\u8fb9\u94fe\uff0c\u518d\u9009\u62e9\u4e00\u6761\u6216\u591a\u6761\u7a7f\u8fc7\u53c2\u8003\u8fb9\u94fe\u7684\u76ee\u6807\u8fb9\uff0c\u6309\u7f1d\u5408\u9608\u503c\u6267\u884c\u3002",
          "NitroPoly_CutAndStitch", "minus"),
    _spec("corner_connect", "\u5e73\u5206", "\u8fde\u63a5\u5de5\u5177", "corner_connect",
          "\u7528\u9014\uff1a\u6309 NitroPoly 2.0 \u7684\u89d2\u90e8\u7b97\u6cd5\u5efa\u7acb\u5e73\u5206\u8fde\u63a5\uff0c\u5e76\u81ea\u52a8\u5904\u7406\u89d2\u70b9\u62d3\u6251\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u4e24\u6761\u6216\u66f4\u591a\u8fb9\uff1b\u4e5f\u53ef\u9009\u62e9\u4e24\u4e2a\u9876\u70b9\u5efa\u7acb\u89d2\u90e8\uff0c\u9009\u62e9\u591a\u4e2a\u9876\u70b9\u6267\u884c\u666e\u901a\u8fde\u63a5\u3002",
          "NitroPoly_CornerConnect"),
    _spec("end_connect", "\u56db\u8fb9\u672b\u7aef", "\u8fde\u63a5\u5de5\u5177", "end_connect",
          "\u7528\u9014\uff1a\u5728\u516d\u9876\u70b9\u672b\u7aef\u533a\u57df\u81ea\u52a8\u5bfb\u627e\u5bf9\u8fb9\uff0c\u5efa\u7acb\u56db\u8fb9\u672b\u7aef\u8fde\u63a5\u3002\n\u4f7f\u7528\uff1a\u53ea\u9009\u62e9\u4e00\u6761\u672b\u7aef\u8fb9\u540e\u6267\u884c\u3002",
          "NitroPoly_EndConnect"),
    _spec("distance_connect", "\u8ddd\u79bb", "\u8fde\u63a5\u5de5\u5177", "distance_connect",
          "\u7528\u9014\uff1a\u8fde\u63a5\u4e24\u6761\u8fb9\uff1b\u76f8\u90bb\u8fb9\u76f4\u63a5\u8fde\u63a5\uff0c\u4e0d\u76f8\u90bb\u65f6\u6cbf\u540c\u4e00\u8fb9\u73af\u8fde\u63a5\u4e2d\u95f4\u533a\u57df\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u540c\u4e00\u6a21\u578b\u4e0a\u7684\u4e24\u6761\u8fb9\u540e\u6267\u884c\u3002",
          "NitroPoly_DistanceConnect"),
    _spec("flow_connect", "\u6d41", "\u8fde\u63a5\u5de5\u5177", "flow_connect",
          "\u7528\u9014\uff1a\u6309 Maya Edge Flow \u5efa\u7acb\u8d34\u5408\u5468\u56f4\u66f2\u7387\u7684\u8fde\u63a5\u8fb9\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u540c\u4e00\u6a21\u578b\u4e0a\u7684\u4e24\u6761\u6216\u66f4\u591a\u8fb9\u540e\u6267\u884c\u3002",
          "NitroPoly_FlowConnect"),
    _spec("vertex_to_edge", "\u70b9\u5230\u8fb9", "\u8fde\u63a5\u5de5\u5177", "vertex_to_edge",
          "\u7528\u9014\uff1a\u628a\u9876\u70b9\u5782\u76f4\u6295\u5f71\u5230\u6240\u9009\u8fb9\uff0c\u5728\u6295\u5f71\u4f4d\u7f6e\u5207\u51fa\u65b0\u70b9\u540e\u8fde\u63a5\u3002\n\u4f7f\u7528\uff1a\u5728 Multi \u9009\u62e9\u6a21\u5f0f\u4e0b\u9009\u62e9\u4e00\u4e2a\u9876\u70b9\u548c\u4e00\u6761\u540c\u6a21\u578b\u8fb9\u3002",
          "NitroPoly_VertEdge"),
    _spec("load_vertex", "\u52a0\u8f7d\u9876\u70b9", "\u8fde\u63a5\u5de5\u5177", "load_vertex",
          "\u7528\u9014\uff1a\u4fdd\u5b58\u4e00\u4e2a\u76ee\u6807\u9876\u70b9\uff0c\u4f9b\u201c\u8fde\u63a5\u5230\u9876\u70b9\u201d\u4f7f\u7528\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u4e00\u4e2a\u9876\u70b9\u540e\u6267\u884c\u3002",
          "NitroPoly_LoadVertex", "plus"),
    _spec("connect_to_vertex", "\u8fde\u63a5\u5230\u9876\u70b9", "\u8fde\u63a5\u5de5\u5177", "connect_to_vertex",
          "\u7528\u9014\uff1a\u628a\u5f53\u524d\u6240\u9009\u9876\u70b9\u9010\u4e2a\u8fde\u63a5\u5230\u5df2\u52a0\u8f7d\u9876\u70b9\u3002\n\u4f7f\u7528\uff1a\u5148\u52a0\u8f7d\u76ee\u6807\u9876\u70b9\uff0c\u518d\u9009\u62e9\u540c\u4e00\u6a21\u578b\u4e0a\u7684\u4e00\u4e2a\u6216\u591a\u4e2a\u5176\u4ed6\u9876\u70b9\u3002",
          "NitroPoly_ConnectToVertex", "minus"),

    _spec("space_loop", "\u7a7a\u95f4", "\u5faa\u73af\u5de5\u5177", "space_loop",
          "\u7528\u9014\uff1a\u6cbf\u539f\u6709\u8fb9\u94fe\u5f27\u957f\u5747\u5300\u5206\u5e03\u9876\u70b9\uff1b\u5f00\u653e\u8fb9\u94fe\u4fdd\u7559\u4e24\u7aef\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u4e00\u7ec4\u6216\u591a\u7ec4\u8fde\u7eed\u8fb9\u94fe\u540e\u6267\u884c\uff0c\u53ef\u91cd\u590d\u6267\u884c\u3002",
          "NitroPoly_Space"),
    _spec("straight_loop", "\u76f4\u7ebf", "\u5faa\u73af\u5de5\u5177", "straight_loop",
          "\u7528\u9014\uff1a\u628a\u5f00\u653e\u8fb9\u94fe\u5185\u90e8\u9876\u70b9\u6295\u5f71\u5230\u4e24\u7aef\u8fde\u7ebf\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u81f3\u5c11\u4e24\u6761\u8fde\u7eed\u5f00\u653e\u8fb9\u540e\u6267\u884c\u3002",
          "NitroPoly_Straight"),
    _spec("circle_loop", "\u5706\u5f62", "\u5faa\u73af\u5de5\u5177", "circle_loop",
          "\u7528\u9014\uff1a\u628a\u95ed\u5408\u8fb9\u73af\u6574\u7406\u4e3a\u7b49\u534a\u5f84\u3001\u7b49\u89d2\u5ea6\u5706\u5f62\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u95ed\u5408\u8fb9\u73af\u540e\u6267\u884c\u3002",
          "NitroPoly_Circle"),
    _spec("geo_poly", "\u591a\u8fb9\u5f62", "\u5faa\u73af\u5de5\u5177", "geo_poly",
          "\u7528\u9014\uff1a\u628a\u6240\u9009\u9762\u533a\u57df\u7684\u5916\u8fb9\u754c\u6574\u7406\u4e3a\u89c4\u5219\u591a\u8fb9\u5f62\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u8fde\u7eed\u9762\u533a\u57df\u540e\u6267\u884c\u3002",
          "NitroPoly_Geopoly"),
    _spec("view_planar", "\u89c6\u89d2\u5e73\u9762", "\u5faa\u73af\u5de5\u5177", "view_planar",
          "\u7528\u9014\uff1a\u6cbf\u5f53\u524d\u89c6\u89d2\u628a\u6240\u9009\u7ec4\u4ef6\u538b\u5230\u540c\u4e00\u5e73\u9762\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u9876\u70b9\u3001\u8fb9\u6216\u9762\u540e\u6267\u884c\u3002",
          "NitroPoly_ViewPlanar"),
    _spec("make_planar", "\u5e73\u5747\u5e73\u9762", "\u5faa\u73af\u5de5\u5177", "make_planar",
          "\u7528\u9014\uff1a\u6309\u6240\u9009\u533a\u57df\u5e73\u5747\u6cd5\u7ebf\u628a\u7ec4\u4ef6\u538b\u5230\u540c\u4e00\u5e73\u9762\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u9876\u70b9\u3001\u8fb9\u6216\u9762\u540e\u6267\u884c\u3002",
          "NitroPoly_MakePlanar"),
    _spec("center_loop", "\u4e2d\u5fc3", "\u5faa\u73af\u5de5\u5177", "center_loop",
          "\u7528\u9014\uff1a\u4f7f\u7528 Maya Edge Flow \u628a\u6240\u9009\u8fb9\u8c03\u6574\u5230\u76f8\u90bb\u9762\u7684\u4e2d\u5fc3\u6d41\u7ebf\u4e0a\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u8fb9\u540e\u6267\u884c\uff0c\u53ef\u91cd\u590d\u6267\u884c\u3002",
          "NitroPoly_Center"),
    _spec("relax_loop", "\u677e\u5f1b", "\u5faa\u73af\u5de5\u5177", "relax_loop",
          "\u7528\u9014\uff1a\u5bf9\u6240\u9009\u9876\u70b9\u6216\u7531\u8fb9\u3001\u9762\u8f6c\u6362\u5f97\u5230\u7684\u9876\u70b9\u6267\u884c\u4e00\u6b21\u5e73\u5747\u677e\u5f1b\u3002\n\u4f7f\u7528\uff1a\u9009\u62e9\u7ec4\u4ef6\u540e\u6267\u884c\uff0c\u53ef\u91cd\u590d\u6267\u884c\u3002",
          "NitroPoly_Relax"),
]

SPEC_BY_ID = dict((item["id"], item) for item in TOOL_SPECS)
CATEGORIES = [
    "\u7f16\u8f91\u9009\u62e9",
    "\u7f51\u683c\u7f16\u8f91",
    "\u8f74\u5fc3\u70b9/\u89e3\u51bb\u53d8\u6362",
    "\u62d3\u6251\u5de5\u5177",
    "\u8fde\u63a5\u5de5\u5177",
    "\u5faa\u73af\u5de5\u5177",
]

LEGACY_ALIASES = {
    "growLoop": "grow_loop",
    "shrinkLoop": "shrink_loop",
    "growRing": "grow_ring",
    "shrinkRing": "shrink_ring",
    "dotLoop": "dot_loop",
    "dotRing": "dot_ring",
    "hardEdge": "hard_edge",
    "uvEdge": "uv_edge",
    "pointTopoint": "point_to_point",
    "faceBorderSel": "face_fill",
    "CombineClean": "combine_clean",
    "detatchClean": "detach_clean",
    "UniConnector": "uni_connect",
    "UniRemover": "uni_remove",
    "basepivot": "base_pivot",
    "worldPivot": "world_pivot",
    "UnFreezeTranslate": "unfreeze_translate",
    "moveToOrigin": "move_to_origin",
    "corner45Plus": "corner_plus",
    "corner45Minus": "corner_minus",
    "edgeExtend": "f2_extend",
    "bevelModifierPlus": "bevel_plus",
    "bevelModifierMinus": "bevel_minus",
    "loadEdgeLoop": "load_edge_loop",
    "cutAndStitch": "cut_stitch",
    "cornerConnect": "corner_connect",
    "endConnect": "end_connect",
    "distConnect": "distance_connect",
    "flowConnect": "flow_connect",
    "vertexToEdge": "vertex_to_edge",
    "loadVertex": "load_vertex",
    "connectToVertex": "connect_to_vertex",
    "spaceloop": "space_loop",
    "straightloop": "straight_loop",
    "circle": "circle_loop",
    "geoPoly": "geo_poly",
    "viewPlanar": "view_planar",
    "makePlanar": "make_planar",
    "centerloop": "center_loop",
    "relaxLoop": "relax_loop",
}


def _index(component):
    match = re.search(r"\[(-?\d+)\]", component)
    if not match:
        raise NitroPolyError("\u65e0\u6cd5\u8bfb\u53d6\u7ec4\u4ef6\u7f16\u53f7\uff1a{}".format(component))
    return int(match.group(1))


def _owner(component):
    return component.split(".", 1)[0]


def _v_add(a, b):
    return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]


def _v_sub(a, b):
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]


def _v_mul(a, value):
    return [a[0] * value, a[1] * value, a[2] * value]


def _v_dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _v_cross(a, b):
    return [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]


def _v_length(a):
    return math.sqrt(max(0.0, _v_dot(a, a)))


def _v_normal(a):
    length = _v_length(a)
    if length <= 1e-12:
        return [0.0, 0.0, 0.0]
    return _v_mul(a, 1.0 / length)


def _v_distance(a, b):
    return _v_length(_v_sub(a, b))


def _v_average(values):
    if not values:
        return [0.0, 0.0, 0.0]
    total = [0.0, 0.0, 0.0]
    for value in values:
        total = _v_add(total, value)
    return _v_mul(total, 1.0 / float(len(values)))


def _newell_normal(points):
    if len(points) < 3:
        return [0.0, 0.0, 0.0]
    normal = [0.0, 0.0, 0.0]
    for index, current in enumerate(points):
        following = points[(index + 1) % len(points)]
        normal[0] += (current[1] - following[1]) * (current[2] + following[2])
        normal[1] += (current[2] - following[2]) * (current[0] + following[0])
        normal[2] += (current[0] - following[0]) * (current[1] + following[1])
    return _v_normal(normal)


def _project_to_plane(point, center, normal):
    return _v_sub(point, _v_mul(normal, _v_dot(_v_sub(point, center), normal)))


def _rotate_around_axis(point, pivot, axis, radians):
    vector = _v_sub(point, pivot)
    cosine = math.cos(radians)
    sine = math.sin(radians)
    term_a = _v_mul(vector, cosine)
    term_b = _v_mul(_v_cross(axis, vector), sine)
    term_c = _v_mul(axis, _v_dot(axis, vector) * (1.0 - cosine))
    return _v_add(pivot, _v_add(term_a, _v_add(term_b, term_c)))


def _component_ranges(owner, component_type, indices):
    values = sorted(set(int(value) for value in indices))
    if not values:
        return []
    result = []
    start = previous = values[0]
    for value in values[1:]:
        if value == previous + 1:
            previous = value
            continue
        if start == previous:
            result.append("{}.{}[{}]".format(owner, component_type, start))
        else:
            result.append("{}.{}[{}:{}]".format(owner, component_type, start, previous))
        start = previous = value
    if start == previous:
        result.append("{}.{}[{}]".format(owner, component_type, start))
    else:
        result.append("{}.{}[{}:{}]".format(owner, component_type, start, previous))
    return result


class MeshQuery(object):
    @staticmethod
    def selection(mask=None):
        if mask is None:
            return cmds.ls(selection=True, long=True, flatten=True) or []
        values = cmds.filterExpand(selectionMask=mask, expand=True) or []
        return cmds.ls(values, long=True, flatten=True) or []

    @staticmethod
    def mesh_shape(node):
        node = _owner(node)
        matches = cmds.ls(node, long=True) or []
        if not matches:
            return ""
        node = matches[0]
        node_type = cmds.nodeType(node)
        if node_type == "mesh":
            return node
        if node_type == "transform":
            shapes = cmds.listRelatives(
                node, shapes=True, noIntermediate=True, fullPath=True, type="mesh"
            ) or []
            return shapes[0] if shapes else ""
        return ""

    @staticmethod
    def transform(node):
        node = _owner(node)
        matches = cmds.ls(node, long=True) or []
        if not matches:
            return ""
        node = matches[0]
        if cmds.nodeType(node) == "transform":
            return node
        if cmds.nodeType(node) == "mesh":
            parents = cmds.listRelatives(node, parent=True, fullPath=True) or []
            return parents[0] if parents else ""
        return ""

    @classmethod
    def selected_mesh_shapes(cls):
        result = []
        for item in cls.selection():
            shape = cls.mesh_shape(item)
            if shape and shape not in result:
                result.append(shape)
        return result

    @staticmethod
    def convert(items, **kwargs):
        result = cmds.polyListComponentConversion(items, **kwargs)
        return cmds.ls(result, long=True, flatten=True) or []

    @classmethod
    def vertices(cls, items):
        return cls.convert(items, toVertex=True)

    @classmethod
    def edges(cls, items):
        return cls.convert(items, toEdge=True)

    @classmethod
    def faces(cls, items):
        return cls.convert(items, toFace=True)

    @classmethod
    def uvs(cls, items):
        return cls.convert(items, toUV=True)

    @classmethod
    def edge_vertices(cls, edge):
        return cls.convert(edge, fromEdge=True, toVertex=True)

    @classmethod
    def edge_faces(cls, edge):
        return cls.convert(edge, fromEdge=True, toFace=True)

    @classmethod
    def face_edges(cls, face):
        return cls.convert(face, fromFace=True, toEdge=True)

    @classmethod
    def face_vertices(cls, face):
        return cls.convert(face, fromFace=True, toVertex=True)

    @classmethod
    def ensure_same_owner(cls, components):
        owners = set(_owner(item) for item in components)
        if len(owners) != 1:
            raise NitroPolyError("\u6240\u9009\u7ec4\u4ef6\u5fc5\u987b\u5c5e\u4e8e\u540c\u4e00\u4e2a\u6a21\u578b")
        return list(owners)[0]

    @classmethod
    def edge_groups(cls, edges):
        edges = list(edges)
        if not edges:
            return []
        cls.ensure_same_owner(edges)
        edge_vertices = {}
        vertex_edges = {}
        for edge in edges:
            vertices = cls.edge_vertices(edge)
            if len(vertices) != 2:
                continue
            edge_vertices[edge] = vertices
            for vertex in vertices:
                vertex_edges.setdefault(vertex, set()).add(edge)
        groups = []
        remaining = set(edge_vertices)
        while remaining:
            seed = min(remaining, key=_index)
            remaining.remove(seed)
            stack = [seed]
            group = []
            while stack:
                edge = stack.pop()
                group.append(edge)
                for vertex in edge_vertices[edge]:
                    for neighbour in vertex_edges.get(vertex, ()):
                        if neighbour in remaining:
                            remaining.remove(neighbour)
                            stack.append(neighbour)
            groups.append(sorted(group, key=_index))
        return groups

    @classmethod
    def ordered_edge_chain(cls, edges):
        edges = list(edges)
        if not edges:
            raise NitroPolyError("\u6ca1\u6709\u53ef\u6392\u5e8f\u7684\u8fb9")
        cls.ensure_same_owner(edges)
        edge_vertices = {}
        vertex_edges = {}
        for edge in edges:
            vertices = cls.edge_vertices(edge)
            if len(vertices) != 2:
                raise NitroPolyError("\u65e0\u6cd5\u8bfb\u53d6\u8fb9\u7aef\u70b9\uff1a{}".format(edge))
            edge_vertices[edge] = vertices
            for vertex in vertices:
                vertex_edges.setdefault(vertex, []).append(edge)
        if any(len(values) > 2 for values in vertex_edges.values()):
            raise NitroPolyError("\u9009\u62e9\u4e2d\u5b58\u5728\u5206\u53c9")
        endpoints = [vertex for vertex, values in vertex_edges.items() if len(values) == 1]
        if len(endpoints) not in (0, 2):
            raise NitroPolyError("\u6240\u9009\u8fb9\u4e0d\u662f\u5355\u6761\u8fde\u7eed\u8fb9\u94fe")
        if endpoints:
            start = min(endpoints, key=_index)
        else:
            first_edge = min(edges, key=_index)
            start = min(edge_vertices[first_edge], key=_index)
        remaining = set(edges)
        ordered_edges = []
        ordered_vertices = [start]
        current = start
        while remaining:
            candidates = [edge for edge in vertex_edges[current] if edge in remaining]
            if not candidates:
                break
            edge = min(candidates, key=_index)
            remaining.remove(edge)
            ordered_edges.append(edge)
            vertices = edge_vertices[edge]
            current = vertices[1] if current == vertices[0] else vertices[0]
            ordered_vertices.append(current)
        if remaining:
            raise NitroPolyError("\u9009\u62e9\u5305\u542b\u4e0d\u8fde\u7eed\u8fb9\u7ec4")
        return ordered_edges, ordered_vertices

    @classmethod
    def ring_neighbours(cls, edge):
        edge_vertices = set(cls.edge_vertices(edge))
        result = set()
        for face in cls.edge_faces(edge):
            face_edges = cls.face_edges(face)
            if len(face_edges) != 4:
                continue
            for candidate in face_edges:
                if candidate == edge:
                    continue
                if not edge_vertices.intersection(cls.edge_vertices(candidate)):
                    result.add(candidate)
        return result

    @classmethod
    def order_edge_path(cls, edges, mode):
        edges = list(edges)
        if not edges:
            return [], False
        selected = set(edges)
        adjacency = dict((edge, set()) for edge in edges)
        if mode == "loop":
            vertex_edges = {}
            for edge in edges:
                for vertex in cls.edge_vertices(edge):
                    vertex_edges.setdefault(vertex, []).append(edge)
            for values in vertex_edges.values():
                for edge in values:
                    adjacency[edge].update(item for item in values if item != edge)
        else:
            for edge in edges:
                adjacency[edge].update(cls.ring_neighbours(edge).intersection(selected))
        if any(len(values) > 2 for values in adjacency.values()):
            raise NitroPolyError("\u8def\u5f84\u5b58\u5728\u5206\u53c9")
        endpoints = [edge for edge, values in adjacency.items() if len(values) <= 1]
        closed = not endpoints and len(edges) > 2
        start = min(endpoints or edges, key=_index)
        ordered = []
        previous = None
        current = start
        while current is not None and current not in ordered:
            ordered.append(current)
            candidates = [item for item in adjacency[current] if item != previous and item not in ordered]
            next_edge = min(candidates, key=_index) if candidates else None
            previous, current = current, next_edge
        if len(ordered) != len(edges):
            raise NitroPolyError("\u65e0\u6cd5\u5efa\u7acb\u5b8c\u6574\u8def\u5f84\u987a\u5e8f")
        return ordered, closed

    @classmethod
    def full_edge_path(cls, seed_edge, mode):
        transform = cls.transform(seed_edge)
        owner = _owner(seed_edge)
        edge_id = _index(seed_edge)
        result = []
        try:
            flag = {"edgeLoop": edge_id} if mode == "loop" else {"edgeRing": edge_id}
            ids = cmds.polySelect(transform, noSelection=True, **flag) or []
            result = ["{}.e[{}]".format(owner, int(value)) for value in ids]
        except Exception:
            result = []
        if result:
            return cmds.ls(result, long=True, flatten=True) or result
        original = cls.selection()
        try:
            cmds.select(seed_edge, replace=True)
            mel.eval("polySelectSp -{};".format("loop" if mode == "loop" else "ring"))
            return cls.selection(32)
        finally:
            if original:
                cmds.select(original, replace=True)
            else:
                cmds.select(clear=True)

    @classmethod
    def selected_transforms(cls):
        result = []
        for item in cls.selection():
            transform = cls.transform(item)
            if transform and transform not in result:
                result.append(transform)
        return result


@contextmanager
def execution_context(name):
    unit = None
    soft_enabled = None
    soft_distance = None
    opened = False
    try:
        cmds.undoInfo(openChunk=True, chunkName="NitroPoly.{}".format(name))
        opened = True
        unit = cmds.currentUnit(query=True, linear=True)
        soft_enabled = cmds.softSelect(query=True, softSelectEnabled=True)
        soft_distance = cmds.softSelect(query=True, softSelectDistance=True)
        yield
    finally:
        try:
            if cmds.progressWindow(query=True, exists=True):
                cmds.progressWindow(endProgress=True)
        except Exception:
            pass
        try:
            mel.eval("resetPolySelectConstraint;")
        except Exception:
            pass
        try:
            if unit:
                cmds.currentUnit(linear=unit)
        except Exception:
            pass
        try:
            if soft_distance is not None:
                cmds.softSelect(softSelectDistance=soft_distance)
            if soft_enabled is not None:
                cmds.softSelect(softSelectEnabled=soft_enabled)
        except Exception:
            pass
        if opened:
            try:
                cmds.undoInfo(closeChunk=True)
            except Exception:
                pass


class ToolBase(object):
    def __init__(self, app):
        self.app = app
        self.query = app.query

    def value(self, key, default):
        return self.app.ui.value(key, default)

    def message(self, text):
        self.app.message(text)


class SelectionTools(ToolBase):
    def _modify_path(self, mode, grow):
        selected = self.query.selection(32)
        if not selected:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u8fb9")
        self.query.ensure_same_owner(selected)
        selected_set = set(selected)
        result = set()
        visited = set()
        for seed in selected:
            if seed in visited:
                continue
            full_path = self.query.full_edge_path(seed, mode)
            full_set = set(full_path)
            path_selected = selected_set.intersection(full_set)
            visited.update(path_selected)
            ordered, closed = self.query.order_edge_path(full_path, mode)
            if not ordered:
                continue
            selected_indices = set(index for index, edge in enumerate(ordered) if edge in path_selected)
            if grow:
                output_indices = set(selected_indices)
                for index in selected_indices:
                    if index > 0:
                        output_indices.add(index - 1)
                    elif closed:
                        output_indices.add(len(ordered) - 1)
                    if index < len(ordered) - 1:
                        output_indices.add(index + 1)
                    elif closed:
                        output_indices.add(0)
            else:
                output_indices = set()
                for index in selected_indices:
                    previous = index - 1 if index > 0 else (len(ordered) - 1 if closed else None)
                    following = index + 1 if index < len(ordered) - 1 else (0 if closed else None)
                    if previous is not None and following is not None:
                        if previous in selected_indices and following in selected_indices:
                            output_indices.add(index)
            result.update(ordered[index] for index in output_indices)
        if not result:
            raise NitroPolyError("\u5f53\u524d\u9009\u62e9\u6ca1\u6709\u53ef\u4fdd\u7559\u7684\u5185\u90e8\u8fb9")
        cmds.select(sorted(result, key=_index), replace=True)

    def grow_loop(self):
        self._modify_path("loop", True)

    def shrink_loop(self):
        self._modify_path("loop", False)

    def grow_ring(self):
        self._modify_path("ring", True)

    def shrink_ring(self):
        self._modify_path("ring", False)

    def _dot(self, mode):
        selected = self.query.selection(32)
        if not selected:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u4e00\u6761\u8fb9\u4f5c\u4e3a\u8d77\u70b9")
        spacing = max(2, int(self.value("gap", 1)) + 1)
        selected_set = set(selected)
        result = set()
        visited = set()
        for seed in selected:
            if seed in visited:
                continue
            path = self.query.full_edge_path(seed, mode)
            visited.update(path)
            ordered, closed = self.query.order_edge_path(path, mode)
            if not ordered:
                continue
            seed_indices = [index for index, edge in enumerate(ordered) if edge in selected_set]
            offset = min(seed_indices) if seed_indices else 0
            for index, edge in enumerate(ordered):
                if (index - offset) % spacing == 0:
                    result.add(edge)
            if closed and len(result) == 1 and len(ordered) > spacing:
                result.add(ordered[(offset + spacing) % len(ordered)])
        cmds.select(sorted(result, key=_index), replace=True)

    def dot_loop(self):
        faces = self.query.selection(34)
        if faces:
            if len(faces) != 2:
                raise NitroPolyError("\u9762\u6a21\u5f0f\u9700\u8981\u9009\u62e9\u4e24\u4e2a\u76f8\u90bb\u9762")
            first_edges = set(self.query.face_edges(faces[0]))
            shared = first_edges.intersection(self.query.face_edges(faces[1]))
            if len(shared) != 1:
                raise NitroPolyError("\u4e24\u4e2a\u9762\u5fc5\u987b\u76f8\u90bb")
            shared_edge = list(shared)[0]
            ring = self.query.full_edge_path(shared_edge, "ring")
            strip_faces = set(self.query.faces(ring))
            strip_edges = set(self.query.edges(list(strip_faces)))
            candidates = list(strip_edges.difference(ring))
            if not candidates:
                raise NitroPolyError("\u65e0\u6cd5\u5efa\u7acb\u9762\u5faa\u73af")
            cmds.select(candidates[0], replace=True)
            self._dot("loop")
            dotted_faces = set(self.query.faces(self.query.selection(32)))
            cmds.select(sorted(dotted_faces.intersection(strip_faces), key=_index), replace=True)
            return
        self._dot("loop")

    def dot_ring(self):
        self._dot("ring")

    def hard_edge(self):
        shapes = self.query.selected_mesh_shapes()
        if not shapes:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u591a\u8fb9\u5f62\u6a21\u578b")
        all_edges = []
        for shape in shapes:
            count = cmds.polyEvaluate(shape, edge=True)
            if count:
                all_edges.append("{}.e[0:{}]".format(shape, count - 1))
        cmds.select(all_edges, replace=True)
        mel.eval("polySelectConstraint -mode 3 -type 0x8000 -sm 1;")
        mel.eval("resetPolySelectConstraint;")

    def uv_edge(self):
        shapes = self.query.selected_mesh_shapes()
        if not shapes:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u591a\u8fb9\u5f62\u6a21\u578b")
        seams = []
        for shape in shapes:
            cmds.select(shape + ".map[*]", replace=True)
            mel.eval("polySelectBorderShell 1;")
            mel.eval("PolySelectConvert 20;")
            candidates = self.query.selection(32)
            for edge in candidates:
                if len(self.query.uvs(edge)) > 2:
                    seams.append(edge)
        if seams:
            cmds.select(seams, replace=True)
        else:
            cmds.select(clear=True)
            self.message("\u6240\u9009\u6a21\u578b\u6ca1\u6709\u68c0\u6d4b\u5230 UV \u63a5\u7f1d")

    def point_to_point(self):
        vertices = self.query.selection(31)
        if len(vertices) != 2:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u4e24\u4e2a\u9876\u70b9")
        owner = self.query.ensure_same_owner(vertices)
        transform = self.query.transform(owner)
        ids = (_index(vertices[0]), _index(vertices[1]))
        cmds.polySelect(transform, shortestEdgePath=ids)

    def face_fill(self):
        faces = self.query.selection(34)
        if len(faces) != 2:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u4e24\u4e2a\u9762")
        self.query.ensure_same_owner(faces)
        start, goal = faces
        queue = [start]
        previous = {start: None}
        while queue:
            current = queue.pop(0)
            if current == goal:
                break
            neighbours = set()
            for edge in self.query.face_edges(current):
                neighbours.update(self.query.edge_faces(edge))
            for neighbour in neighbours:
                if neighbour not in previous:
                    previous[neighbour] = current
                    queue.append(neighbour)
        if goal not in previous:
            raise NitroPolyError("\u4e24\u4e2a\u9762\u4e4b\u95f4\u6ca1\u6709\u53ef\u7528\u8def\u5f84")
        path = []
        current = goal
        while current is not None:
            path.append(current)
            current = previous[current]
        cmds.select(list(reversed(path)), replace=True)


class MeshTools(ToolBase):
    def combine_clean(self):
        transforms = self.query.selected_transforms()
        transforms = [
            item for item in transforms
            if self.query.mesh_shape(item)
        ]
        if len(transforms) < 2:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u4e24\u4e2a\u6216\u66f4\u591a\u591a\u8fb9\u5f62\u6a21\u578b")
        first = transforms[0]
        first_short = first.rsplit("|", 1)[-1]
        parent = cmds.listRelatives(first, parent=True, fullPath=True) or []
        pivot = cmds.xform(first, query=True, worldSpace=True, rotatePivot=True)
        result = cmds.polyUnite(
            transforms,
            constructionHistory=False,
            mergeUVSets=True,
            centerPivot=False
        )
        new_transform = result[0]
        new_transform = cmds.rename(new_transform, first_short)
        if parent:
            new_transform = cmds.parent(new_transform, parent[0])[0]
        cmds.xform(new_transform, worldSpace=True, pivots=pivot)
        cmds.delete(new_transform, constructionHistory=True)
        cmds.select(new_transform, replace=True)

    def detach_clean(self):
        faces = self.query.selection(34)
        if not faces:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u9700\u8981\u5206\u79bb\u7684\u9762")
        owner = self.query.ensure_same_owner(faces)
        transform = self.query.transform(owner)
        face_count = cmds.polyEvaluate(owner, face=True)
        selected_ids = set(_index(face) for face in faces)
        if len(selected_ids) >= face_count:
            raise NitroPolyError("\u4e0d\u80fd\u5206\u79bb\u6a21\u578b\u7684\u5168\u90e8\u9762")
        duplicate = cmds.duplicate(transform, returnRootsOnly=True)[0]
        duplicate_shape = self.query.mesh_shape(duplicate)
        delete_ids = set(range(face_count)).difference(selected_ids)
        delete_components = _component_ranges(duplicate_shape, "f", delete_ids)
        if delete_components:
            cmds.delete(delete_components)
        cmds.delete(faces)
        cmds.delete([transform, duplicate], constructionHistory=True)
        cmds.xform(duplicate, centerPivots=True)
        cmds.select(duplicate, replace=True)

    def uni_connect(self):
        vertices = self.query.selection(31)
        edges = self.query.selection(32)
        faces = self.query.selection(34)
        if vertices:
            if len(vertices) < 2:
                raise NitroPolyError("\u8bf7\u9009\u62e9\u81f3\u5c11\u4e24\u4e2a\u9876\u70b9")
            owner = self.query.ensure_same_owner(vertices)
            before = cmds.polyEvaluate(owner, edge=True)
            cmds.polyConnectComponents(vertices, constructionHistory=False)
            after = cmds.polyEvaluate(owner, edge=True)
            if after > before:
                cmds.select(
                    "{}.e[{}:{}]".format(owner, before, after - 1),
                    replace=True
                )
            return
        if edges:
            owner = self.query.ensure_same_owner(edges)
            border_edges = [
                edge for edge in edges
                if len(self.query.edge_faces(edge)) == 1
            ]
            before_vertices = cmds.polyEvaluate(owner, vertex=True)
            before_edges = cmds.polyEvaluate(owner, edge=True)
            if len(edges) == 1:
                cmds.select(edges, replace=True)
                cmds.polySubdivideEdge(
                    edges,
                    divisions=1,
                    constructionHistory=False
                )
            elif len(edges) == 2 and len(border_edges) == 2:
                cmds.select(edges, replace=True)
                cmds.polyAppend(
                    append=[_index(edges[0]), _index(edges[1])],
                    constructionHistory=False
                )
            elif len(edges) == len(border_edges):
                full_loop = set(self.query.full_edge_path(edges[0], "loop"))
                cmds.select(edges, replace=True)
                if full_loop and full_loop == set(edges):
                    cmds.polyCloseBorder(constructionHistory=False)
                else:
                    cmds.polyBridgeEdge(
                        edges,
                        divisions=0,
                        constructionHistory=False
                    )
            else:
                cmds.polyConnectComponents(edges, constructionHistory=False)
            after_vertices = cmds.polyEvaluate(owner, vertex=True)
            after_edges = cmds.polyEvaluate(owner, edge=True)
            if after_vertices > before_vertices:
                cmds.select(
                    "{}.vtx[{}:{}]".format(
                        owner, before_vertices, after_vertices - 1
                    ),
                    replace=True
                )
            elif after_edges > before_edges:
                cmds.select(
                    "{}.e[{}:{}]".format(owner, before_edges, after_edges - 1),
                    replace=True
                )
            return
        if faces:
            cmds.polyExtrudeFacet(faces, constructionHistory=False)
            try:
                mel.eval('performPolyMove "" 0;')
            except Exception:
                pass
            return
        transforms = self.query.selected_transforms()
        if transforms:
            mel.eval("dR_multiCutTool;")
            return
        raise NitroPolyError("\u8bf7\u9009\u62e9\u5bf9\u8c61\u6216\u591a\u8fb9\u5f62\u7ec4\u4ef6")

    def uni_remove(self):
        vertices = self.query.selection(31)
        edges = self.query.selection(32)
        faces = self.query.selection(34)
        if vertices:
            if len(vertices) == 1:
                mel.eval("DeleteVertex;")
            else:
                mel.eval("MergeToCenter;")
            return
        if edges:
            if len(edges) == 1:
                mel.eval("polyCollapseEdge;")
            else:
                seed_path = set(self.query.full_edge_path(edges[0], "loop"))
                if seed_path.difference(edges):
                    mel.eval("polyCollapseEdge;")
                else:
                    mel.eval("DeleteEdge;")
            cmds.select(clear=True)
            return
        if faces:
            mel.eval("polyMergeToCenter;")
            return
        transforms = self.query.selected_transforms()
        if transforms:
            cmds.delete(transforms)
            return
        raise NitroPolyError("\u8bf7\u9009\u62e9\u5bf9\u8c61\u6216\u591a\u8fb9\u5f62\u7ec4\u4ef6")


class TransformTools(ToolBase):
    def base_pivot(self):
        transforms = self.query.selected_transforms()
        if not transforms:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u5bf9\u8c61")
        for transform in transforms:
            box = cmds.exactWorldBoundingBox(transform)
            bottom = [
                (box[0] + box[3]) * 0.5,
                box[1],
                (box[2] + box[5]) * 0.5,
            ]
            cmds.xform(transform, worldSpace=True, pivots=bottom)

    def world_pivot(self):
        transforms = self.query.selected_transforms()
        if not transforms:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u5bf9\u8c61")
        for transform in transforms:
            cmds.xform(transform, worldSpace=True, pivots=(0.0, 0.0, 0.0))

    def unfreeze_translate(self):
        transforms = self.query.selected_transforms()
        if not transforms:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u5bf9\u8c61")
        for transform in transforms:
            locked = [
                attribute for attribute in ("translateX", "translateY", "translateZ")
                if cmds.getAttr(transform + "." + attribute, lock=True)
            ]
            if locked:
                raise NitroPolyError("{} \u7684\u4f4d\u79fb\u901a\u9053\u5df2\u9501\u5b9a".format(transform))
            cmds.makeIdentity(transform, apply=True, translate=True, rotate=False, scale=False)
            cmds.move(0.0, 0.0, 0.0, transform, rotatePivotRelative=True)
            position = [
                -cmds.getAttr(transform + ".translateX"),
                -cmds.getAttr(transform + ".translateY"),
                -cmds.getAttr(transform + ".translateZ"),
            ]
            cmds.makeIdentity(transform, apply=True, translate=True, rotate=False, scale=False)
            cmds.setAttr(transform + ".translateX", position[0])
            cmds.setAttr(transform + ".translateY", position[1])
            cmds.setAttr(transform + ".translateZ", position[2])

    def move_to_origin(self):
        transforms = self.query.selected_transforms()
        if not transforms:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u5bf9\u8c61")
        for transform in transforms:
            cmds.move(0.0, 0.0, 0.0, transform, rotatePivotRelative=True)


class TopologyTools(ToolBase):
    def _corner_rotate(self, degrees):
        edges = self.query.selection(32)
        faces = self.query.selection(34)
        if len(edges) != 1 or not faces:
            raise NitroPolyError("\u9700\u8981\u540c\u65f6\u9009\u62e9\u4e00\u6761\u8fb9\u548c\u4e00\u4e2a\u6216\u591a\u4e2a\u9762")
        components = edges + faces
        self.query.ensure_same_owner(components)
        edge_vertices = self.query.edge_vertices(edges[0])
        if len(edge_vertices) != 2:
            raise NitroPolyError("\u65e0\u6cd5\u8bfb\u53d6\u65cb\u8f6c\u8f74")
        positions = [
            cmds.xform(vertex, query=True, worldSpace=True, translation=True)
            for vertex in edge_vertices
        ]
        pivot = _v_mul(_v_add(positions[0], positions[1]), 0.5)
        axis = _v_normal(_v_sub(positions[1], positions[0]))
        if _v_length(axis) <= 1e-12:
            raise NitroPolyError("\u65cb\u8f6c\u8f74\u957f\u5ea6\u4e3a\u96f6")
        fixed = set(edge_vertices)
        vertices = set(self.query.vertices(faces)).difference(fixed)
        radians = math.radians(float(degrees))
        for vertex in vertices:
            point = cmds.xform(vertex, query=True, worldSpace=True, translation=True)
            result = _rotate_around_axis(point, pivot, axis, radians)
            cmds.xform(vertex, worldSpace=True, translation=result)
        cmds.select(faces, replace=True)

    def corner_plus(self):
        self._corner_rotate(45.0)

    def corner_minus(self):
        self._corner_rotate(-45.0)

    def f2_extend(self):
        edges = self.query.selection(32)
        if len(edges) != 1:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u4e00\u6761\u8fb9\u754c\u8fb9")
        edge = edges[0]
        connected_faces = self.query.edge_faces(edge)
        if len(connected_faces) != 1:
            raise NitroPolyError("\u6240\u9009\u8fb9\u5fc5\u987b\u662f\u8fb9\u754c\u8fb9")
        face = connected_faces[0]
        face_vertices = self.query.face_vertices(face)
        if len(face_vertices) != 4:
            raise NitroPolyError("F2 \u6269\u5c55\u8981\u6c42\u76f8\u90bb\u9762\u4e3a\u56db\u8fb9\u9762")
        edge_vertices = self.query.edge_vertices(edge)
        face_edges = self.query.face_edges(face)
        inner_for_endpoint = {}
        for endpoint in edge_vertices:
            connected = []
            for face_edge in face_edges:
                if face_edge == edge:
                    continue
                vertices = self.query.edge_vertices(face_edge)
                if endpoint in vertices:
                    connected.extend(vertex for vertex in vertices if vertex != endpoint)
            if len(connected) != 1:
                raise NitroPolyError("\u65e0\u6cd5\u5224\u65ad\u56db\u8fb9\u9762\u5916\u63a8\u65b9\u5411")
            inner_for_endpoint[endpoint] = connected[0]
        endpoint_positions = dict(
            (vertex, cmds.xform(vertex, query=True, worldSpace=True, translation=True))
            for vertex in edge_vertices
        )
        target_positions = {}
        for endpoint in edge_vertices:
            inner_position = cmds.xform(
                inner_for_endpoint[endpoint],
                query=True,
                worldSpace=True,
                translation=True
            )
            target_positions[endpoint] = _v_add(
                endpoint_positions[endpoint],
                _v_sub(endpoint_positions[endpoint], inner_position)
            )
        owner = _owner(edge)
        before = cmds.polyEvaluate(owner, vertex=True)
        cmds.polyExtrudeEdge(edge, divisions=1, constructionHistory=False)
        after = cmds.polyEvaluate(owner, vertex=True)
        new_vertices = [
            "{}.vtx[{}]".format(owner, value)
            for value in range(before, after)
        ]
        if len(new_vertices) != 2:
            raise NitroPolyError("F2 \u6269\u5c55\u6ca1\u6709\u751f\u6210\u4e24\u4e2a\u65b0\u9876\u70b9")
        assignments = {}
        remaining = set(new_vertices)
        for endpoint in edge_vertices:
            best = min(
                remaining,
                key=lambda vertex: _v_distance(
                    cmds.xform(vertex, query=True, worldSpace=True, translation=True),
                    endpoint_positions[endpoint]
                )
            )
            assignments[endpoint] = best
            remaining.remove(best)
        for endpoint, new_vertex in assignments.items():
            cmds.xform(
                new_vertex,
                worldSpace=True,
                translation=target_positions[endpoint]
            )
        threshold = max(0.0, float(self.value("f2_threshold", 0.001)))
        if threshold > 0.0:
            all_vertices = cmds.ls(owner + ".vtx[*]", flatten=True) or []
            for new_vertex in list(assignments.values()):
                if not cmds.objExists(new_vertex):
                    continue
                position = cmds.xform(
                    new_vertex, query=True, worldSpace=True, translation=True
                )
                candidates = [
                    vertex for vertex in all_vertices
                    if vertex != new_vertex and cmds.objExists(vertex)
                ]
                if candidates:
                    closest = min(
                        candidates,
                        key=lambda vertex: _v_distance(
                            position,
                            cmds.xform(
                                vertex, query=True, worldSpace=True, translation=True
                            )
                        )
                    )
                    closest_position = cmds.xform(
                        closest, query=True, worldSpace=True, translation=True
                    )
                    if _v_distance(position, closest_position) <= threshold:
                        cmds.xform(new_vertex, worldSpace=True, translation=closest_position)
                        cmds.polyMergeVertex(
                            [new_vertex, closest],
                            distance=threshold,
                            constructionHistory=False
                        )
        cmds.select(
            [vertex for vertex in assignments.values() if cmds.objExists(vertex)],
            replace=True
        )

    def _bevel(self, increase):
        edges = self.query.selection(32)
        if len(edges) < 3:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u81f3\u5c11\u4e09\u6761\u8fde\u7eed\u5012\u89d2\u8f6e\u5ed3\u8fb9")
        self.query.ensure_same_owner(edges)
        groups = self.query.edge_groups(edges)
        factor_step = min(max(float(self.value("bevel_step", 0.1)), 0.001), 0.95)
        factor = 1.0 + factor_step if increase else 1.0 - factor_step
        moves = {}
        valid = 0
        for group in groups:
            if len(group) < 3:
                continue
            ordered_edges, ordered_vertices = self.query.ordered_edge_chain(group)
            if ordered_vertices[0] == ordered_vertices[-1]:
                continue
            points = [
                cmds.xform(vertex, query=True, worldSpace=True, translation=True)
                for vertex in ordered_vertices
            ]
            p0, p1 = points[0], points[1]
            pn, pn1 = points[-1], points[-2]
            direction_a = _v_sub(p1, p0)
            direction_b = _v_sub(pn1, pn)
            a = _v_dot(direction_a, direction_a)
            b = _v_dot(direction_a, direction_b)
            c = _v_dot(direction_b, direction_b)
            w0 = _v_sub(p0, pn)
            d = _v_dot(direction_a, w0)
            e = _v_dot(direction_b, w0)
            denominator = a * c - b * b
            scale = max(a, c, 1.0)
            if a <= 1e-16 or c <= 1e-16 or abs(denominator) <= 1e-12 * scale * scale:
                continue
            parameter_a = (b * e - c * d) / denominator
            parameter_b = (a * e - b * d) / denominator
            if parameter_a < -1e-5 or parameter_b < -1e-5:
                continue
            point_a = _v_add(p0, _v_mul(direction_a, parameter_a))
            point_b = _v_add(pn, _v_mul(direction_b, parameter_b))
            corner = _v_mul(_v_add(point_a, point_b), 0.5)
            for vertex, point in zip(ordered_vertices[1:-1], points[1:-1]):
                target = _v_add(corner, _v_mul(_v_sub(point, corner), factor))
                moves.setdefault(vertex, []).append(target)
            valid += 1
        if not moves:
            raise NitroPolyError("\u6240\u9009\u8fb9\u4e0d\u662f\u53ef\u8c03\u6574\u7684\u5f00\u653e\u5012\u89d2\u8f6e\u5ed3")
        for vertex, targets in moves.items():
            cmds.xform(vertex, worldSpace=True, translation=_v_average(targets))
        cmds.select(edges, replace=True)
        if valid < len(groups):
            self.message("\u90e8\u5206\u8fb9\u7ec4\u4e0d\u7b26\u5408\u5012\u89d2\u8f6e\u5ed3\u6761\u4ef6\uff0c\u5df2\u8df3\u8fc7")

    def bevel_plus(self):
        self._bevel(True)

    def bevel_minus(self):
        self._bevel(False)


class ConnectTools(ToolBase):
    def load_edge_loop(self):
        edges = self.query.selection(32)
        if not edges:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u53c2\u8003\u8fb9\u94fe")
        self.query.ensure_same_owner(edges)
        self.query.ordered_edge_chain(edges)
        self.app.state["loaded_edges"] = list(edges)
        self.app.ui.set_status("loaded_edges", "{} \u6761\u8fb9".format(len(edges)))

    def cut_stitch(self):
        target_edges = self.query.selection(32)
        reference_edges = self.app.state.get("loaded_edges") or []
        if not target_edges:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u9700\u8981\u5207\u5272\u7684\u76ee\u6807\u8fb9")
        if not reference_edges or any(
                not cmds.objExists(edge) for edge in reference_edges):
            self.app.state["loaded_edges"] = []
            self.app.ui.set_status("loaded_edges", "\u65e0\u5faa\u73af\u8fb9")
            raise NitroPolyError("\u8bf7\u91cd\u65b0\u52a0\u8f7d\u53c2\u8003\u8fb9\u94fe")
        if len(reference_edges) < len(target_edges):
            raise NitroPolyError("\u76ee\u6807\u8fb9\u6570\u91cf\u4e0d\u80fd\u591a\u4e8e\u5df2\u52a0\u8f7d\u53c2\u8003\u8fb9")

        self.query.ensure_same_owner(reference_edges)
        target_owner = self.query.ensure_same_owner(target_edges)
        _, reference_vertices = self.query.ordered_edge_chain(reference_edges)
        if len(reference_vertices) < 2:
            raise NitroPolyError("\u53c2\u8003\u8fb9\u94fe\u65e0\u6548")

        reference_data = [
            (
                vertex,
                cmds.xform(
                    vertex,
                    query=True,
                    worldSpace=True,
                    translation=True
                )
            )
            for vertex in reference_vertices
        ]
        threshold = max(
            0.000001,
            float(self.value("stitch_threshold", 0.1))
        )
        same_mesh = _owner(reference_edges[0]) == target_owner
        created_vertices = []
        target_vertices = set()
        processed = 0

        for edge in target_edges:
            edge_vertices = self.query.edge_vertices(edge)
            if len(edge_vertices) != 2:
                continue
            start_position = cmds.xform(
                edge_vertices[0],
                query=True,
                worldSpace=True,
                translation=True
            )
            end_position = cmds.xform(
                edge_vertices[1],
                query=True,
                worldSpace=True,
                translation=True
            )
            line = _v_sub(end_position, start_position)
            length_sq = _v_dot(line, line)
            if length_sq <= 1e-16:
                continue

            matches = []
            for reference_vertex, reference_position in reference_data:
                parameter = _v_dot(
                    _v_sub(reference_position, start_position),
                    line
                ) / length_sq
                projection = _v_add(
                    start_position,
                    _v_mul(line, parameter)
                )
                if (
                    -1e-7 <= parameter <= 1.0000001
                    and _v_distance(reference_position, projection) <= threshold
                ):
                    matches.append(
                        (
                            max(0.0, min(1.0, parameter)),
                            reference_vertex,
                            reference_position
                        )
                    )
            matches.sort(key=lambda item: item[0])
            unique_matches = []
            for match in matches:
                if (
                    not unique_matches
                    or abs(match[0] - unique_matches[-1][0]) > 1e-7
                ):
                    unique_matches.append(match)
            matches = unique_matches
            if len(matches) < 2:
                continue

            interior = [
                match for match in matches[1:-1]
                if 1e-7 < match[0] < 0.9999999
            ]
            new_vertices = []
            if interior:
                before = cmds.polyEvaluate(target_owner, vertex=True)
                cmds.select(edge, replace=True)
                cmds.polySplit(
                    target_owner,
                    ip=[(_index(edge), match[0]) for match in interior],
                    ch=False
                )
                after = cmds.polyEvaluate(target_owner, vertex=True)
                new_vertices = [
                    "{}.vtx[{}]".format(target_owner, value)
                    for value in range(before, after)
                ]
                new_vertices.sort(
                    key=lambda vertex: _v_dot(
                        _v_sub(
                            cmds.xform(
                                vertex,
                                query=True,
                                worldSpace=True,
                                translation=True
                            ),
                            start_position
                        ),
                        line
                    ) / length_sq
                )
                for vertex, match in zip(new_vertices, interior):
                    cmds.xform(
                        vertex,
                        worldSpace=True,
                        translation=match[2]
                    )
                created_vertices.extend(new_vertices)

            cmds.xform(
                edge_vertices[0],
                worldSpace=True,
                translation=matches[0][2]
            )
            cmds.xform(
                edge_vertices[1],
                worldSpace=True,
                translation=matches[-1][2]
            )
            target_vertices.update(edge_vertices)
            target_vertices.update(new_vertices)
            processed += 1

        if not processed:
            raise NitroPolyError("\u76ee\u6807\u8fb9\u6ca1\u6709\u5728\u7f1d\u5408\u9608\u503c\u5185\u7a7f\u8fc7\u53c2\u8003\u8fb9\u94fe")

        if same_mesh:
            weld_vertices = list(set(reference_vertices) | target_vertices)
            if weld_vertices:
                cmds.polyMergeVertex(
                    weld_vertices,
                    distance=0.1,
                    constructionHistory=False
                )

        selectable = [
            vertex for vertex in created_vertices
            if cmds.objExists(vertex)
        ]
        if selectable:
            cmds.select(selectable, replace=True)
        else:
            cmds.select(target_edges, replace=True)

    def _new_edges_after(self, owner, before_vertices):
        after_vertices = cmds.polyEvaluate(owner, vertex=True)
        if after_vertices <= before_vertices:
            return []
        vertices = "{}.vtx[{}:{}]".format(
            owner,
            before_vertices,
            after_vertices - 1
        )
        return self.query.convert(vertices, toEdge=True, internal=True)

    def _soft_edge_connect(self, components):
        components = cmds.ls(components, long=True, flatten=True) or []
        if not components:
            return []
        owner = self.query.ensure_same_owner(components)
        is_vertex = ".vtx[" in components[0]
        if is_vertex:
            before = cmds.polyEvaluate(owner, edge=True)
            cmds.polyConnectComponents(
                components,
                constructionHistory=False
            )
            after = cmds.polyEvaluate(owner, edge=True)
            new_edges = [
                "{}.e[{}]".format(owner, value)
                for value in range(before, after)
            ]
        else:
            before = cmds.polyEvaluate(owner, vertex=True)
            cmds.polyConnectComponents(
                components,
                constructionHistory=False
            )
            new_edges = self._new_edges_after(owner, before)
        if new_edges:
            cmds.polySoftEdge(
                new_edges,
                angle=180,
                constructionHistory=False
            )
            cmds.select(new_edges, replace=True)
        return new_edges

    def _duplicate_items(self, values):
        counts = {}
        result = []
        for value in values:
            counts[value] = counts.get(value, 0) + 1
            if counts[value] == 2:
                result.append(value)
        return result

    def _top_vertices(self, vertices):
        face_values = []
        for vertex in vertices:
            face_values.extend(self.query.faces(vertex))
        shared_faces = self._duplicate_items(face_values)
        shared_face_vertices = self.query.vertices(shared_faces)
        if len(shared_face_vertices) != 6:
            return []
        connected_edges = self.query.edges(vertices)
        connected_vertices = self.query.vertices(connected_edges)
        return list(set(shared_face_vertices).difference(connected_vertices))

    def _corner_edges(self, selected_edges):
        selected_edges = cmds.ls(
            selected_edges,
            long=True,
            flatten=True
        ) or []
        corner_edges = []
        for edge in selected_edges:
            vertices = self.query.edge_vertices(edge)
            connected_edges = self.query.edges(vertices)
            edge_groups = []
            for vertex in vertices:
                group = set(self.query.edges(vertex))
                group.discard(edge)
                edge_groups.extend(group)
            overlap_vertices = self.query.vertices(edge_groups)
            middle_vertices = self._duplicate_items(overlap_vertices)
            middle_edges = self.query.edges(middle_vertices)
            intersecting = list(set(middle_edges).intersection(connected_edges))
            back_vertices = self.query.vertices(intersecting)
            final_vertices = list(set(back_vertices).intersection(vertices))
            corner_edges.extend(
                self.query.convert(
                    final_vertices,
                    toEdge=True,
                    internal=True
                )
            )
        result = []
        for edge in corner_edges:
            if edge not in result:
                result.append(edge)
        return result

    def _build_angle(self, middle_vertex):
        connected_edges = self.query.edges(middle_vertex)
        vertices = list(
            set(self.query.vertices(connected_edges)).difference([middle_vertex])
        )
        if len(vertices) < 2:
            return []
        vertices = sorted(vertices, key=_index)[:2]

        face_values = []
        for vertex in vertices:
            face_values.extend(self.query.faces(vertex))
        middle_faces = self._duplicate_items(face_values)
        middle_face_vertices = self.query.vertices(middle_faces)
        neighbour_edges = self.query.edges(vertices)
        neighbour_vertices = self.query.vertices(neighbour_edges)
        top_vertices = list(
            set(middle_face_vertices).difference(neighbour_vertices)
        )
        if not top_vertices:
            return []

        first = cmds.xform(
            vertices[0], query=True, worldSpace=True, translation=True
        )
        second = cmds.xform(
            vertices[1], query=True, worldSpace=True, translation=True
        )
        top = cmds.xform(
            top_vertices[0], query=True, worldSpace=True, translation=True
        )
        midpoint = _v_mul(_v_add(first, second), 0.5)
        final_midpoint = _v_add(
            midpoint,
            _v_mul(_v_sub(top, midpoint), 0.333)
        )
        cmds.xform(
            middle_vertex,
            worldSpace=True,
            translation=final_midpoint
        )
        return self._soft_edge_connect([middle_vertex] + top_vertices)

    def _connect_corner(self):
        selected_edges = self.query.selection(32)
        selected_vertices = self.query.selection(31)
        edge = None
        if selected_vertices:
            vertices = selected_vertices
            if len(vertices) != 2:
                raise NitroPolyError("\u8bf7\u9009\u62e9\u4e24\u4e2a\u9876\u70b9")
        elif selected_edges:
            if len(selected_edges) != 1:
                raise NitroPolyError("\u8bf7\u9009\u62e9\u4e00\u6761\u8fb9")
            edge = selected_edges[0]
            vertices = self.query.edge_vertices(edge)
            if len(vertices) != 2:
                raise NitroPolyError("\u65e0\u6cd5\u8bfb\u53d6\u8fb9\u7aef\u70b9")
        else:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u4e24\u4e2a\u9876\u70b9\u6216\u4e00\u6761\u8fb9")

        self.query.ensure_same_owner(vertices)
        top_vertices = self._top_vertices(vertices)
        if edge:
            cmds.polyDelEdge(
                edge,
                constructionHistory=False,
                cleanVertices=False
            )
        if not top_vertices:
            return self._soft_edge_connect(vertices)

        first = cmds.xform(
            vertices[0], query=True, worldSpace=True, translation=True
        )
        second = cmds.xform(
            vertices[1], query=True, worldSpace=True, translation=True
        )
        top = cmds.xform(
            top_vertices[0], query=True, worldSpace=True, translation=True
        )
        midpoint = _v_mul(_v_add(first, second), 0.5)
        final_midpoint = _v_add(
            midpoint,
            _v_mul(_v_sub(top, midpoint), 0.333)
        )

        owner = _owner(vertices[0])
        created_edges = self._soft_edge_connect(vertices)
        if not created_edges:
            raise NitroPolyError("\u89d2\u90e8\u8fde\u63a5\u5931\u8d25")
        before = cmds.polyEvaluate(owner, vertex=True)
        cmds.select(created_edges, replace=True)
        cmds.polySubdivideEdge(
            divisions=1,
            constructionHistory=False
        )
        after = cmds.polyEvaluate(owner, vertex=True)
        if after <= before:
            raise NitroPolyError("\u672a\u751f\u6210\u89d2\u90e8\u9876\u70b9")
        new_vertex = "{}.vtx[{}]".format(owner, before)
        cmds.xform(
            new_vertex,
            worldSpace=True,
            translation=final_midpoint
        )
        return self._soft_edge_connect([new_vertex] + top_vertices)

    def corner_connect(self):
        edges = self.query.selection(32)
        vertices = self.query.selection(31)
        if edges:
            if len(edges) < 2:
                raise NitroPolyError("\u8bf7\u9009\u62e9\u81f3\u5c11\u4e24\u6761\u8fb9")
            owner = self.query.ensure_same_owner(edges)
            before = cmds.polyEvaluate(owner, vertex=True)
            created_edges = self._soft_edge_connect(edges)
            corner_edges = self._corner_edges(created_edges)
            if corner_edges:
                pre_vertices = set(self.query.vertices(corner_edges))
                cmds.select(corner_edges, replace=True)
                cmds.polySubdivideEdge(constructionHistory=False)
                post_vertices = set(self.query.vertices(corner_edges))
                corner_vertices = sorted(
                    post_vertices.difference(pre_vertices),
                    key=_index
                )
                for vertex in corner_vertices:
                    if cmds.objExists(vertex):
                        self._build_angle(vertex)
            after = cmds.polyEvaluate(owner, vertex=True)
            if after > before:
                new_vertices = "{}.vtx[{}:{}]".format(
                    owner,
                    before,
                    after - 1
                )
                new_edges = self.query.convert(
                    new_vertices,
                    toEdge=True,
                    internal=True
                )
                cmds.select(new_edges or created_edges, replace=True)
            elif created_edges:
                cmds.select(created_edges, replace=True)
            return

        if vertices:
            if len(vertices) < 2:
                raise NitroPolyError("\u8bf7\u9009\u62e9\u81f3\u5c11\u4e24\u4e2a\u9876\u70b9")
            self.query.ensure_same_owner(vertices)
            if len(vertices) == 2:
                self._connect_corner()
            else:
                self._soft_edge_connect(vertices)
            return
        raise NitroPolyError("\u8bf7\u9009\u62e9\u8fb9\u6216\u9876\u70b9")

    def end_connect(self):
        edges = self.query.selection(32)
        if len(edges) != 1:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u4e00\u6761\u672b\u7aef\u8fb9")
        edge = edges[0]
        owner = self.query.ensure_same_owner(edges)
        original_vertices = self.query.edge_vertices(edge)
        connected_edges = self.query.edges(original_vertices)
        neighbourhood_vertices = self.query.vertices(connected_edges)
        connected_faces = self.query.edge_faces(edge)

        six_vertex_faces = []
        for face in connected_faces:
            if len(self.query.face_vertices(face)) == 6:
                six_vertex_faces.append(face)
        if len(six_vertex_faces) != 1:
            raise NitroPolyError("\u6240\u9009\u8fb9\u5fc5\u987b\u4f4d\u4e8e\u552f\u4e00\u7684\u516d\u9876\u70b9\u672b\u7aef\u533a\u57df")

        face_vertices = self.query.face_vertices(six_vertex_faces[0])
        final_vertices = list(
            set(face_vertices).difference(neighbourhood_vertices)
        )
        opposite_edges = self.query.convert(
            final_vertices,
            toEdge=True,
            internal=True
        )
        if len(opposite_edges) != 1:
            raise NitroPolyError("\u672a\u627e\u5230\u552f\u4e00\u7684\u672b\u7aef\u5bf9\u8fb9")

        created_edges = self._soft_edge_connect([edge, opposite_edges[0]])
        if not created_edges:
            raise NitroPolyError("\u672b\u7aef\u8fde\u63a5\u5931\u8d25")
        pre_vertices = set(self.query.vertices(created_edges))
        cmds.select(created_edges, replace=True)
        cmds.polySubdivideEdge(constructionHistory=False)
        post_vertices = set(self.query.vertices(created_edges))
        middle_vertices = sorted(
            post_vertices.difference(pre_vertices),
            key=_index
        )
        if not middle_vertices:
            raise NitroPolyError("\u672a\u751f\u6210\u672b\u7aef\u4e2d\u95f4\u9876\u70b9")

        corner_edges = set()
        for old_vertex in original_vertices:
            cmds.select(middle_vertices + [old_vertex], replace=True)
            self.corner_connect()
            current = self.query.selection()
            current_vertices = self.query.vertices(current)
            corner_edges.update(self.query.edges(current_vertices))

        middle_edges = set(self.query.edges(middle_vertices))
        delete_edges = list(middle_edges.difference(corner_edges))
        if delete_edges:
            cmds.polyDelEdge(
                delete_edges,
                constructionHistory=False,
                cleanVertices=True
            )
        remaining = [
            item for item in corner_edges
            if cmds.objExists(item)
        ]
        if remaining:
            cmds.select(sorted(remaining, key=_index), replace=True)
        else:
            cmds.select(owner, replace=True)

    def distance_connect(self):
        edges = self.query.selection(32)
        if len(edges) != 2:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u4e24\u6761\u8fb9")
        owner = self.query.ensure_same_owner(edges)
        before = cmds.polyEvaluate(owner, vertex=True)

        adjacent_edges = set(self.query.edges(self.query.edge_faces(edges[0])))
        if edges[1] in adjacent_edges:
            connect_edges = edges
        else:
            path = self.query.full_edge_path(edges[0], "ring")
            ordered, closed = self.query.order_edge_path(path, "ring")
            if edges[1] not in ordered:
                raise NitroPolyError("\u4e24\u6761\u8fb9\u4e0d\u5728\u540c\u4e00\u8fb9\u73af")
            first_index = ordered.index(edges[0])
            second_index = ordered.index(edges[1])
            low, high = sorted((first_index, second_index))
            connect_edges = ordered[low:high + 1]
            if closed:
                alternate = ordered[high:] + ordered[:low + 1]
                if len(alternate) < len(connect_edges):
                    connect_edges = alternate

        cmds.polyConnectComponents(
            connect_edges,
            constructionHistory=False
        )
        new_edges = self._new_edges_after(owner, before)
        if not new_edges:
            raise NitroPolyError("\u6ca1\u6709\u751f\u6210\u65b0\u7684\u8fde\u63a5\u8fb9")
        cmds.polySoftEdge(
            new_edges,
            angle=180,
            constructionHistory=False
        )
        cmds.select(new_edges, replace=True)

    def flow_connect(self):
        edges = self.query.selection(32)
        if len(edges) < 2:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u81f3\u5c11\u4e24\u6761\u8fb9")
        owner = self.query.ensure_same_owner(edges)
        before = cmds.polyEvaluate(owner, vertex=True)
        cmds.polyConnectComponents(
            edges,
            insertWithEdgeFlow=True,
            constructionHistory=False
        )
        new_edges = self._new_edges_after(owner, before)
        if not new_edges:
            raise NitroPolyError("\u6ca1\u6709\u751f\u6210\u65b0\u7684\u6d41\u8fde\u63a5\u8fb9")
        cmds.polySoftEdge(
            new_edges,
            angle=180,
            constructionHistory=False
        )
        cmds.select(new_edges, replace=True)

    def vertex_to_edge(self):
        vertices = self.query.selection(31)
        edges = self.query.selection(32)
        if len(vertices) != 1 or len(edges) != 1:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u4e00\u4e2a\u9876\u70b9\u548c\u4e00\u6761\u8fb9")
        vertex = vertices[0]
        edge = edges[0]
        owner = self.query.ensure_same_owner([vertex, edge])
        edge_vertices = self.query.edge_vertices(edge)
        if len(edge_vertices) != 2:
            raise NitroPolyError("\u65e0\u6cd5\u8bfb\u53d6\u76ee\u6807\u8fb9\u7aef\u70b9")

        start = cmds.xform(
            edge_vertices[0], query=True, worldSpace=True, translation=True
        )
        end = cmds.xform(
            edge_vertices[1], query=True, worldSpace=True, translation=True
        )
        point = cmds.xform(
            vertex, query=True, worldSpace=True, translation=True
        )
        line = _v_sub(end, start)
        length_sq = _v_dot(line, line)
        if length_sq <= 1e-16:
            raise NitroPolyError("\u76ee\u6807\u8fb9\u957f\u5ea6\u4e3a\u96f6")
        parameter = _v_dot(_v_sub(point, start), line) / length_sq
        if parameter < -1e-7 or parameter > 1.0000001:
            raise NitroPolyError("\u9876\u70b9\u6295\u5f71\u4e0d\u5728\u6240\u9009\u8fb9\u7ebf\u6bb5\u5185")
        parameter = max(0.0, min(1.0, parameter))

        before = cmds.polyEvaluate(owner, vertex=True)
        cmds.polySplit(
            owner,
            ip=[(_index(edge), parameter)],
            ch=False
        )
        after = cmds.polyEvaluate(owner, vertex=True)
        if after <= before:
            raise NitroPolyError("\u6ca1\u6709\u5728\u76ee\u6807\u8fb9\u4e0a\u751f\u6210\u65b0\u9876\u70b9")
        new_vertex = "{}.vtx[{}]".format(owner, before)
        cmds.polyConnectComponents(
            [vertex, new_vertex],
            constructionHistory=False
        )
        cmds.select(new_vertex, replace=True)

    def load_vertex(self):
        vertices = self.query.selection(31)
        if len(vertices) != 1:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u4e00\u4e2a\u9876\u70b9")
        self.app.state["loaded_vertex"] = vertices[0]
        self.app.ui.set_status(
            "loaded_vertex",
            "vtx[{}]".format(_index(vertices[0]))
        )

    def connect_to_vertex(self):
        target = self.app.state.get("loaded_vertex")
        if not target or not cmds.objExists(target):
            self.app.state["loaded_vertex"] = ""
            self.app.ui.set_status("loaded_vertex", "\u6ca1\u6709\u52a0\u8f7d\u9876\u70b9")
            raise NitroPolyError("\u8bf7\u91cd\u65b0\u52a0\u8f7d\u76ee\u6807\u9876\u70b9")
        selected = self.query.selection(31)
        selected = [item for item in selected if item != target]
        if not selected:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u9700\u8981\u8fde\u63a5\u7684\u5176\u4ed6\u9876\u70b9")
        self.query.ensure_same_owner(selected + [target])
        owner = _owner(target)
        before = cmds.polyEvaluate(owner, edge=True)
        for point in selected:
            cmds.polyConnectComponents(
                [target, point],
                constructionHistory=False
            )
        after = cmds.polyEvaluate(owner, edge=True)
        if after <= before:
            raise NitroPolyError("\u6ca1\u6709\u751f\u6210\u8fde\u63a5\u8fb9")
        new_edges = [
            "{}.e[{}]".format(owner, value)
            for value in range(before, after)
        ]
        cmds.polySoftEdge(
            new_edges,
            angle=180,
            constructionHistory=False
        )
        cmds.select(new_edges, replace=True)


class LoopTools(ToolBase):
    def _groups(self):
        edges = self.query.selection(32)
        if not edges:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u8fde\u7eed\u8fb9")
        return self.query.edge_groups(edges), edges

    def _sample_polyline(self, points, distances, target):
        if target <= 0.0:
            return list(points[0])
        if target >= distances[-1]:
            return list(points[-1])
        for index in range(1, len(distances)):
            if distances[index] >= target:
                start_distance = distances[index - 1]
                segment = distances[index] - start_distance
                ratio = 0.0 if segment <= 1e-12 else (target - start_distance) / segment
                return _v_add(
                    points[index - 1],
                    _v_mul(_v_sub(points[index], points[index - 1]), ratio)
                )
        return list(points[-1])

    def _space_edges(self, edges):
        _, vertices = self.query.ordered_edge_chain(edges)
        closed = vertices[0] == vertices[-1]
        if closed:
            vertices = vertices[:-1]
        positions = [
            cmds.xform(vertex, query=True, worldSpace=True, translation=True)
            for vertex in vertices
        ]
        if closed:
            polyline = positions + [positions[0]]
        else:
            polyline = positions
        distances = [0.0]
        for index in range(1, len(polyline)):
            distances.append(
                distances[-1] + _v_distance(polyline[index - 1], polyline[index])
            )
        total = distances[-1]
        if total <= 1e-12:
            return
        count = len(vertices)
        divisor = float(count) if closed else float(count - 1)
        for index, vertex in enumerate(vertices):
            if not closed and index in (0, count - 1):
                continue
            target = total * float(index) / divisor
            result = self._sample_polyline(polyline, distances, target)
            cmds.xform(vertex, worldSpace=True, translation=result)

    def space_loop(self):
        groups, original = self._groups()
        for group in groups:
            self._space_edges(group)
        cmds.select(original, replace=True)

    def straight_loop(self):
        groups, original = self._groups()
        for group in groups:
            _, vertices = self.query.ordered_edge_chain(group)
            if vertices[0] == vertices[-1]:
                raise NitroPolyError("\u76f4\u7ebf\u5de5\u5177\u4e0d\u80fd\u5904\u7406\u95ed\u5408\u8fb9\u73af")
            first = cmds.xform(
                vertices[0], query=True, worldSpace=True, translation=True
            )
            last = cmds.xform(
                vertices[-1], query=True, worldSpace=True, translation=True
            )
            direction = _v_sub(last, first)
            length_sq = _v_dot(direction, direction)
            if length_sq <= 1e-12:
                continue
            for vertex in vertices[1:-1]:
                point = cmds.xform(
                    vertex, query=True, worldSpace=True, translation=True
                )
                ratio = _v_dot(_v_sub(point, first), direction) / length_sq
                result = _v_add(first, _v_mul(direction, ratio))
                cmds.xform(vertex, worldSpace=True, translation=result)
        cmds.select(original, replace=True)

    def _circle_edges(self, edges):
        _, vertices = self.query.ordered_edge_chain(edges)
        if vertices[0] != vertices[-1]:
            raise NitroPolyError("\u5706\u5f62\u5de5\u5177\u8981\u6c42\u95ed\u5408\u8fb9\u73af")
        vertices = vertices[:-1]
        points = [
            cmds.xform(vertex, query=True, worldSpace=True, translation=True)
            for vertex in vertices
        ]
        center = _v_average(points)
        normal = _newell_normal(points)
        if _v_length(normal) <= 1e-12:
            raise NitroPolyError("\u65e0\u6cd5\u8ba1\u7b97\u8fb9\u73af\u5e73\u9762")
        first_vector = _project_to_plane(points[0], center, normal)
        first_vector = _v_sub(first_vector, center)
        basis_u = _v_normal(first_vector)
        if _v_length(basis_u) <= 1e-12:
            raise NitroPolyError("\u65e0\u6cd5\u5efa\u7acb\u5706\u5f62\u65b9\u5411")
        basis_v = _v_normal(_v_cross(normal, basis_u))
        radius = sum(_v_distance(point, center) for point in points) / float(len(points))
        if len(points) > 1:
            second_vector = _v_sub(
                _project_to_plane(points[1], center, normal),
                center
            )
            orientation = 1.0 if _v_dot(_v_cross(basis_u, second_vector), normal) >= 0.0 else -1.0
        else:
            orientation = 1.0
        for index, vertex in enumerate(vertices):
            angle = orientation * (2.0 * math.pi * float(index) / float(len(vertices)))
            direction = _v_add(
                _v_mul(basis_u, math.cos(angle)),
                _v_mul(basis_v, math.sin(angle))
            )
            cmds.xform(
                vertex,
                worldSpace=True,
                translation=_v_add(center, _v_mul(direction, radius))
            )

    def circle_loop(self):
        groups, original = self._groups()
        for group in groups:
            self._circle_edges(group)
        cmds.select(original, replace=True)

    def geo_poly(self):
        faces = self.query.selection(34)
        if not faces:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u8fde\u7eed\u9762\u533a\u57df")
        owner = self.query.ensure_same_owner(faces)
        selected_faces = set(faces)
        boundary = []
        for edge in set(self.query.edges(faces)):
            connected = set(self.query.edge_faces(edge))
            if len(connected.intersection(selected_faces)) == 1:
                boundary.append(edge)
        groups = self.query.edge_groups(boundary)
        if not groups:
            raise NitroPolyError("\u6ca1\u6709\u627e\u5230\u9762\u533a\u57df\u5916\u8fb9\u754c")
        for group in groups:
            self._circle_edges(group)
        cmds.select(faces, replace=True)

    def _selected_vertices(self):
        selection = self.query.selection()
        vertices = self.query.vertices(selection)
        if len(vertices) < 3:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u81f3\u5c11\u4e09\u4e2a\u6709\u6548\u9876\u70b9")
        return vertices

    def view_planar(self):
        vertices = self._selected_vertices()
        points = [
            cmds.xform(vertex, query=True, worldSpace=True, translation=True)
            for vertex in vertices
        ]
        center = _v_average(points)
        panel = cmds.getPanel(withFocus=True)
        camera = ""
        if panel and cmds.getPanel(typeOf=panel) == "modelPanel":
            camera = cmds.modelPanel(panel, query=True, camera=True)
        if not camera:
            panels = cmds.getPanel(type="modelPanel") or []
            if panels:
                camera = cmds.modelPanel(panels[0], query=True, camera=True)
        if not camera:
            raise NitroPolyError("\u65e0\u6cd5\u53d6\u5f97\u5f53\u524d\u89c6\u56fe\u76f8\u673a")
        matrix = cmds.xform(camera, query=True, worldSpace=True, matrix=True)
        normal = _v_normal([-matrix[8], -matrix[9], -matrix[10]])
        for vertex, point in zip(vertices, points):
            cmds.xform(
                vertex,
                worldSpace=True,
                translation=_project_to_plane(point, center, normal)
            )

    def _average_face_normal(self, faces):
        normals = []
        pattern = re.compile(r"[-+]?(?:\d*\.\d+|\d+)(?:[eE][-+]?\d+)?")
        for face in faces:
            info = cmds.polyInfo(face, faceNormals=True) or []
            if not info:
                continue
            values = [float(value) for value in pattern.findall(info[0])]
            if len(values) >= 3:
                normals.append(values[-3:])
        return _v_normal(_v_average(normals))

    def make_planar(self):
        selection = self.query.selection()
        vertices = self.query.vertices(selection)
        if len(vertices) < 3:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u81f3\u5c11\u4e09\u4e2a\u6709\u6548\u9876\u70b9")
        faces = self.query.selection(34)
        if not faces:
            faces = self.query.faces(selection)
        normal = self._average_face_normal(faces)
        points = [
            cmds.xform(vertex, query=True, worldSpace=True, translation=True)
            for vertex in vertices
        ]
        if _v_length(normal) <= 1e-12:
            normal = _newell_normal(points)
        if _v_length(normal) <= 1e-12:
            raise NitroPolyError("\u65e0\u6cd5\u8ba1\u7b97\u5e73\u5747\u5e73\u9762")
        center = _v_average(points)
        for vertex, point in zip(vertices, points):
            cmds.xform(
                vertex,
                worldSpace=True,
                translation=_project_to_plane(point, center, normal)
            )

    def center_loop(self):
        edges = self.query.selection(32)
        if not edges:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u8fb9")
        cmds.select(edges, replace=True)
        mel.eval("polyEditEdgeFlow -adjustEdgeFlow 1;")

    def relax_loop(self):
        vertices = self.query.vertices(self.query.selection())
        if not vertices:
            raise NitroPolyError("\u8bf7\u9009\u62e9\u9876\u70b9\u3001\u8fb9\u6216\u9762")
        cmds.polyAverageVertex(vertices, iterations=1, constructionHistory=False)
        cmds.select(vertices, replace=True)


class NitroPolyUI(object):
    def __init__(self, app):
        self.app = app
        self.controls = {}
        self.status = {}
        self.help_field = None
        self.hover_filters = []

    @staticmethod
    def color(red, green, blue):
        return (red / 255.0, green / 255.0, blue / 255.0)

    def value(self, key, default):
        control = self.controls.get(key)
        if not control:
            return default
        try:
            if key == "gap":
                return cmds.intField(control, query=True, value=True)
            return cmds.floatSliderGrp(control, query=True, value=True)
        except Exception:
            return default

    def set_status(self, key, text):
        control = self.status.get(key)
        if control and cmds.control(control, exists=True):
            cmds.textField(
                control,
                edit=True,
                text=text,
                backgroundColor=self.color(55, 72, 67)
            )

    def show_tool_help(self, tool_id):
        if not self.help_field or not cmds.scrollField(self.help_field, exists=True):
            return
        spec = SPEC_BY_ID[tool_id]
        text = "{}  \u00b7  {}\n\n{}".format(
            spec["label"], spec["category"], spec["help"]
        )
        cmds.scrollField(self.help_field, edit=True, text=text)

    def _install_hover_help(self, control, tool_id):
        if not _QT_AVAILABLE:
            return
        try:
            full_name = cmds.button(control, query=True, fullPathName=True)
            pointer = omui.MQtUtil.findControl(full_name)
            if not pointer:
                pointer = omui.MQtUtil.findControl(control)
            if not pointer:
                return
            widget = shiboken.wrapInstance(int(pointer), QtWidgets.QWidget)
            hover_filter = _HoverHelpFilter(self.show_tool_help, tool_id, widget)
            widget.installEventFilter(hover_filter)
            self.hover_filters.append(hover_filter)
        except Exception:
            pass

    def _button(self, tool_id, width=88):
        spec = SPEC_BY_ID[tool_id]
        colors = {
            "normal": self.color(48, 57, 66),
            "plus": self.color(89, 98, 106),
            "minus": self.color(63, 72, 82),
        }
        control = cmds.button(
            label=spec["label"],
            height=25,
            width=width,
            backgroundColor=colors.get(spec["tone"], colors["normal"]),
            annotation=spec["help"],
            command=partial(self.app.execute, tool_id)
        )
        self._install_hover_help(control, tool_id)
        return control

    def _frame(self, label):
        return cmds.frameLayout(
            label=label,
            collapsable=True,
            collapse=False,
            marginWidth=6,
            marginHeight=5,
            backgroundColor=self.color(39, 39, 47)
        )

    def _row(self, columns, widths=None):
        widths = widths or [92] * columns
        return cmds.rowLayout(
            numberOfColumns=columns,
            adjustableColumn=columns,
            columnWidth=[(index + 1, widths[index]) for index in range(columns)],
            columnAttach=[(index + 1, "both", 2) for index in range(columns)]
        )

    def build(self):
        self.hover_filters = []
        if cmds.window(WINDOW_NAME, exists=True):
            cmds.deleteUI(WINDOW_NAME)
        if cmds.windowPref(WINDOW_NAME, exists=True):
            cmds.windowPref(WINDOW_NAME, remove=True)

        window = cmds.window(
            WINDOW_NAME,
            title=WINDOW_TITLE,
            widthHeight=(410, 760),
            sizeable=True,
            minimizeButton=True,
            maximizeButton=True,
            retain=False
        )
        scroll = cmds.scrollLayout(
            childResizable=True,
            horizontalScrollBarThickness=0
        )
        cmds.columnLayout(adjustableColumn=True, rowSpacing=3)

        self._build_selection()
        self._build_mesh()
        self._build_transform()
        self._build_topology()
        self._build_connect()
        self._build_loop()
        self._build_information()

        cmds.setParent(scroll)
        cmds.showWindow(window)
        cmds.window(window, edit=True, widthHeight=(410, 760))
        return window

    def _build_selection(self):
        self._frame("\u7f16\u8f91\u9009\u62e9")
        self._row(4, [96, 96, 96, 96])
        for tool_id in ("grow_loop", "shrink_loop", "grow_ring", "shrink_ring"):
            self._button(tool_id, 94)
        cmds.setParent("..")

        cmds.rowLayout(
            numberOfColumns=3,
            adjustableColumn=3,
            columnWidth3=(42, 166, 166),
            columnAttach3=("both", "both", "both"),
            columnOffset3=(2, 2, 2)
        )
        self.controls["gap"] = cmds.intField(
            value=1, minValue=1, maxValue=100, step=1, width=40
        )
        self._button("dot_loop", 162)
        self._button("dot_ring", 162)
        cmds.setParent("..")

        self._row(4, [96, 96, 96, 96])
        for tool_id in ("hard_edge", "uv_edge", "point_to_point", "face_fill"):
            self._button(tool_id, 94)
        cmds.setParent("..")
        cmds.setParent("..")

    def _build_mesh(self):
        self._frame("\u7f51\u683c\u7f16\u8f91")
        self._row(4, [96, 96, 96, 96])
        for tool_id in ("combine_clean", "detach_clean", "uni_connect", "uni_remove"):
            self._button(tool_id, 94)
        cmds.setParent("..")
        cmds.setParent("..")

    def _build_transform(self):
        self._frame("\u8f74\u5fc3\u70b9/\u89e3\u51bb\u53d8\u6362")
        self._row(4, [96, 96, 96, 96])
        for tool_id in ("base_pivot", "world_pivot", "unfreeze_translate", "move_to_origin"):
            self._button(tool_id, 94)
        cmds.setParent("..")
        cmds.setParent("..")

    def _build_topology(self):
        self._frame("\u62d3\u6251\u5de5\u5177")
        self._row(2, [194, 194])
        self._button("corner_plus", 190)
        self._button("corner_minus", 190)
        cmds.setParent("..")

        self.controls["f2_threshold"] = cmds.floatSliderGrp(
            label="F2 \u9608\u503c",
            field=True,
            value=0.001,
            minValue=0.0001,
            maxValue=1.0,
            fieldMinValue=0.0,
            fieldMaxValue=100.0,
            step=0.001,
            columnWidth3=(55, 55, 270)
        )
        self._button("f2_extend", 386)

        self.controls["bevel_step"] = cmds.floatSliderGrp(
            label="\u5012\u89d2\u6b65\u957f",
            field=True,
            value=0.1,
            minValue=0.001,
            maxValue=0.95,
            fieldMinValue=0.001,
            fieldMaxValue=0.95,
            step=0.01,
            columnWidth3=(55, 55, 270)
        )
        self._row(2, [194, 194])
        self._button("bevel_plus", 190)
        self._button("bevel_minus", 190)
        cmds.setParent("..")
        cmds.setParent("..")

    def _build_connect(self):
        self._frame("\u8fde\u63a5\u5de5\u5177")
        self.controls["stitch_threshold"] = cmds.floatSliderGrp(
            label="\u7f1d\u5408\u9608\u503c",
            field=True,
            value=0.1,
            minValue=0.0001,
            maxValue=1.0,
            fieldMinValue=0.0,
            fieldMaxValue=100.0,
            step=0.01,
            columnWidth3=(55, 55, 270)
        )
        cmds.rowLayout(
            numberOfColumns=3,
            adjustableColumn=3,
            columnWidth3=(100, 125, 155),
            columnAttach3=("both", "both", "both"),
            columnOffset3=(2, 2, 2)
        )
        self.status["loaded_edges"] = cmds.textField(
            text="\u65e0\u5faa\u73af\u8fb9", editable=False, width=98,
            backgroundColor=self.color(39, 39, 47)
        )
        self._button("load_edge_loop", 121)
        self._button("cut_stitch", 151)
        cmds.setParent("..")

        self._row(5, [76, 76, 76, 76, 76])
        for tool_id in (
            "corner_connect", "end_connect", "distance_connect",
            "flow_connect", "vertex_to_edge"
        ):
            self._button(tool_id, 74)
        cmds.setParent("..")

        cmds.rowLayout(
            numberOfColumns=3,
            adjustableColumn=3,
            columnWidth3=(100, 125, 155),
            columnAttach3=("both", "both", "both"),
            columnOffset3=(2, 2, 2)
        )
        self.status["loaded_vertex"] = cmds.textField(
            text="\u6ca1\u6709\u52a0\u8f7d\u9876\u70b9", editable=False, width=98,
            backgroundColor=self.color(39, 39, 47)
        )
        self._button("load_vertex", 121)
        self._button("connect_to_vertex", 151)
        cmds.setParent("..")
        cmds.setParent("..")

    def _build_loop(self):
        self._frame("\u5faa\u73af\u5de5\u5177")
        self._row(4, [96, 96, 96, 96])
        for tool_id in ("space_loop", "straight_loop", "circle_loop", "geo_poly"):
            self._button(tool_id, 94)
        cmds.setParent("..")
        self._row(4, [96, 96, 96, 96])
        for tool_id in ("view_planar", "make_planar", "center_loop", "relax_loop"):
            self._button(tool_id, 94)
        cmds.setParent("..")
        cmds.setParent("..")

    def _build_information(self):
        self._frame("\u529f\u80fd\u4ecb\u7ecd")
        self.help_field = cmds.scrollField(
            editable=False,
            wordWrap=True,
            height=120,
            text="\u9f20\u6807\u79fb\u5165\u529f\u80fd\u6309\u94ae\u65f6\uff0c\u8fd9\u91cc\u4f1a\u7acb\u5373\u663e\u793a\u8be5\u529f\u80fd\u7684\u7528\u9014\u548c\u4f7f\u7528\u65b9\u6cd5\u3002"
        )
        cmds.setParent("..")


class NitroPolyApp(object):
    def __init__(self):
        self.query = MeshQuery()
        self.state = {
            "loaded_edges": [],
            "loaded_vertex": "",
        }
        self.ui = NitroPolyUI(self)
        self.services = OrderedDict([
            ("selection", SelectionTools(self)),
            ("mesh", MeshTools(self)),
            ("transform", TransformTools(self)),
            ("topology", TopologyTools(self)),
            ("connect", ConnectTools(self)),
            ("loop", LoopTools(self)),
        ])
        self.hotkeysList = [
            {"name": spec["hotkey"], "method": spec["id"]}
            for spec in TOOL_SPECS
            if spec.get("hotkey")
        ]
        self.routes = {}
        for service in self.services.values():
            for spec in TOOL_SPECS:
                method = getattr(service, spec["method"], None)
                if callable(method):
                    self.routes[spec["id"]] = method

    def message(self, text):
        try:
            cmds.inViewMessage(
                assistMessage=text,
                position="midCenter",
                fade=True,
                fadeStayTime=2500,
                backColor=0x202020
            )
        except Exception:
            pass
        cmds.warning("NitroPoly\uff1a{}".format(text))

    def execute(self, tool_id, *args):
        if tool_id not in self.routes:
            raise NitroPolyError("\u672a\u6ce8\u518c\u529f\u80fd\uff1a{}".format(tool_id))
        try:
            with execution_context(tool_id):
                return self.routes[tool_id]()
        except NitroPolyError as exc:
            self.message(str(exc))
        except Exception as exc:
            traceback.print_exc()
            self.message("{}\uff1a{}".format(SPEC_BY_ID[tool_id]["label"], exc))
        return None

    def show(self):
        self.register_runtime_commands()
        return self.ui.build()

    def register_runtime_commands(self):
        for spec in TOOL_SPECS:
            name = spec.get("hotkey")
            if not name:
                continue
            command = 'import NitroPoly; NitroPoly.run("{}")'.format(spec["id"])
            try:
                if cmds.runTimeCommand(name, exists=True):
                    cmds.runTimeCommand(
                        name,
                        edit=True,
                        command=command,
                        commandLanguage="python",
                        category="NitroPoly"
                    )
                else:
                    cmds.runTimeCommand(
                        name,
                        command=command,
                        commandLanguage="python",
                        category="NitroPoly"
                    )
            except Exception:
                cmds.warning("NitroPoly\uff1a\u65e0\u6cd5\u6ce8\u518c\u8fd0\u884c\u65f6\u547d\u4ee4 {}".format(name))


class NP(NitroPolyApp):
    @property
    def HighEdges(self):
        return self.state.get("loaded_edges") or []

    @HighEdges.setter
    def HighEdges(self, value):
        self.state["loaded_edges"] = list(value or [])

    @property
    def LoadedVertex(self):
        return self.state.get("loaded_vertex") or ""

    @LoadedVertex.setter
    def LoadedVertex(self, value):
        self.state["loaded_vertex"] = value or ""

    def UI(self):
        return self.show()

    def hotkeyMaker(self, hotkey):
        name = hotkey.get("name")
        method = hotkey.get("method") or hotkey.get("id")
        if not name or not method:
            return
        tool_id = LEGACY_ALIASES.get(method, method)
        command = 'import NitroPoly; NitroPoly.run("{}")'.format(tool_id)
        if cmds.runTimeCommand(name, exists=True):
            cmds.runTimeCommand(
                name,
                edit=True,
                command=command,
                commandLanguage="python",
                category="NitroPoly"
            )
        else:
            cmds.runTimeCommand(
                name,
                command=command,
                commandLanguage="python",
                category="NitroPoly"
            )

    def corner45(self, direction=True, *args):
        return self.execute("corner_plus" if direction else "corner_minus")

    def bevelModifier(self, status=True, *args):
        return self.execute("bevel_plus" if status else "bevel_minus")

    def getAllSel(self):
        return self.query.selection()

    def inLineMessage(self, message, fadeTime=3):
        self.message(message)



def _legacy_method(tool_id):
    def method(self, *args, **kwargs):
        return self.execute(tool_id, *args)
    return method


for _legacy_name, _tool_id in LEGACY_ALIASES.items():
    setattr(NP, _legacy_name, _legacy_method(_tool_id))


def instance():
    global _INSTANCE
    if _INSTANCE is None:
        _INSTANCE = NP()
    return _INSTANCE


def run(method_name, *args, **kwargs):
    tool_id = LEGACY_ALIASES.get(method_name, method_name)
    return instance().execute(tool_id, *args)


def main():
    return instance().show()


def initializePlugin(plugin_object):
    try:
        import maya.api.OpenMaya as om
        om.MFnPlugin(plugin_object, __author__, __version__, "Any")
    except Exception:
        pass
    try:
        cmds.evalDeferred(main)
    except Exception:
        main()


def uninitializePlugin(plugin_object):
    global _INSTANCE
    try:
        if cmds.window(WINDOW_NAME, exists=True):
            cmds.deleteUI(WINDOW_NAME)
    except Exception:
        pass
    _INSTANCE = None


if __name__ == "__main__":
    main()
