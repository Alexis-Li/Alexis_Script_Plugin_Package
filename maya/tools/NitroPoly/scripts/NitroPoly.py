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
    _spec("grow_loop", "循环扩展", "编辑选择", "grow_loop",
          "用途：沿当前循环边或边界链向两端扩展一步。\n使用：选择连续循环边的一部分后执行。",
          "NitroPoly_GrowLoop", "plus"),
    _spec("shrink_loop", "循环缩小", "编辑选择", "shrink_loop",
          "用途：从当前循环边选择的两端各移除一步。\n使用：选择至少三条连续循环边后执行。",
          "NitroPoly_ShrinkLoop", "minus"),
    _spec("grow_ring", "环形扩展", "编辑选择", "grow_ring",
          "用途：沿四边面边环向两侧扩展一步。\n使用：选择同一边环中的一部分边后执行。",
          "NitroPoly_GrowRing", "plus"),
    _spec("shrink_ring", "环形缩小", "编辑选择", "shrink_ring",
          "用途：从当前边环选择的两端各移除一步。\n使用：选择至少三条同一边环中的边后执行。",
          "NitroPoly_ShrinkRing", "minus"),
    _spec("dot_loop", "循环间隔", "编辑选择", "dot_loop",
          "用途：按间隔值选择循环边。\n使用：选择一条循环边作为起点，设置间隔后执行。",
          "NitroPoly_DotLoop", "plus"),
    _spec("dot_ring", "环形间隔", "编辑选择", "dot_ring",
          "用途：按间隔值选择边环。\n使用：选择一条边环边作为起点，设置间隔后执行。",
          "NitroPoly_DotRing", "minus"),
    _spec("hard_edge", "选择硬边", "编辑选择", "hard_edge",
          "用途：选出所选模型上的硬边。\n使用：选择模型或模型组件后执行。",
          "NitroPoly_SelecthardEdge"),
    _spec("uv_edge", "选择UV边", "编辑选择", "uv_edge",
          "用途：选出 UV 接缝边，不包含普通几何边界。\n使用：选择模型或模型组件后执行。",
          "NitroPoly_SelectUVEdge"),
    _spec("point_to_point", "点到点", "编辑选择", "point_to_point",
          "用途：选择两个顶点间的最短边路径。\n使用：在同一模型上选择两个顶点后执行。",
          "NitroPoly_PointToPoint"),
    _spec("face_fill", "填充面", "编辑选择", "face_fill",
          "用途：选择两个面之间的最短连续面路径。\n使用：在同一模型上选择两个面后执行。",
          "NitroPoly_FaceFill"),

    _spec("combine_clean", "清洁合并", "网格编辑", "combine_clean",
          "用途：合并多个多边形模型并删除历史，保留第一个模型的名称、父级和轴心。\n使用：对象模式选择两个或更多模型后执行。",
          "NitroPoly_CleanCombine", "plus"),
    _spec("detach_clean", "清洁分离", "网格编辑", "detach_clean",
          "用途：把选中面提取为独立模型，同时从原模型删除这些面。\n使用：在一个模型上选择部分面后执行。",
          "NitroPoly_CleanDetach", "minus"),
    _spec("uni_connect", "简化连接", "网格编辑", "uni_connect",
          "用途：按当前选择类型执行连接；对象模式进入 Multi-Cut，顶点或边执行连接，面执行原位挤出。\n使用：选择对象、顶点、边或面后执行。",
          "NitroPoly_UniConnect", "plus"),
    _spec("uni_remove", "简化移除", "网格编辑", "uni_remove",
          "用途：按当前选择类型执行删除、合并或塌陷。\n使用：选择对象、顶点、边或面后执行。",
          "NitroPoly_uniRemove", "minus"),

    _spec("base_pivot", "到底部", "轴心点/解冻变换", "base_pivot",
          "用途：把模型轴心移到世界包围盒底部中心。\n使用：对象模式选择一个或多个模型后执行。",
          "NitroPoly_basePivot"),
    _spec("world_pivot", "到原点", "轴心点/解冻变换", "world_pivot",
          "用途：把所选对象轴心移动到世界坐标原点。\n使用：对象模式选择一个或多个对象后执行。",
          "NitroPoly_WorldPivot"),
    _spec("unfreeze_translate", "解冻变换", "轴心点/解冻变换", "unfreeze_translate",
          "用途：在尽量保持模型世界位置的前提下恢复冻结前的位移通道。\n使用：对象模式选择模型后执行；复杂约束、实例或锁定通道应先备份。",
          "NitroPoly_UnFreezeTransform"),
    _spec("move_to_origin", "物体移到原点", "轴心点/解冻变换", "move_to_origin",
          "用途：按旋转轴心把对象移动到世界原点。\n使用：对象模式选择一个或多个对象后执行。",
          "NitroPoly_MoveToOrigin"),

    _spec("corner_plus", "∠ 旋转 45° +", "拓扑工具", "corner_plus",
          "用途：以所选边为旋转轴，将所选面区域旋转正 45°。\n使用：同时选择一条边和一个或多个面后执行。",
          "NitroPoly_CornerRot_Plus", "plus"),
    _spec("corner_minus", "∠ 旋转 45° -", "拓扑工具", "corner_minus",
          "用途：以所选边为旋转轴，将所选面区域旋转负 45°。\n使用：同时选择一条边和一个或多个面后执行。",
          "NitroPoly_CornerRot_Minus", "minus"),
    _spec("f2_extend", "F2 扩展", "拓扑工具", "f2_extend",
          "用途：按相邻四边面方向从一条边界边外推新四边面，并按阈值自动焊接附近顶点。\n使用：选择一条属于四边面的边界边后执行。",
          "NitroPoly_F2Extend"),
    _spec("bevel_plus", "倒角 +", "拓扑工具", "bevel_plus",
          "用途：按理论尖角增大连续倒角轮廓宽度，轮廓两端保持不动。\n使用：选择至少三条连续、开放的倒角轮廓边后执行。",
          "NitroPoly_Bevel_Plus", "plus"),
    _spec("bevel_minus", "倒角 -", "拓扑工具", "bevel_minus",
          "用途：按理论尖角缩小连续倒角轮廓宽度，轮廓两端保持不动。\n使用：选择至少三条连续、开放的倒角轮廓边后执行。",
          "NitroPoly_Bevel_Minus", "minus"),

    _spec("load_edge_loop", "加载循环边", "连接工具", "load_edge_loop",
          "用途：把当前连续边链保存为切割和缝合的参考边链。\n使用：选择参考边链后执行。",
          "NitroPoly_LoadEdgeLoop", "plus"),
    _spec("cut_stitch", "切割和缝合", "连接工具", "cut_stitch",
          "用途：按已加载参考边链的位置切割目标边，并将切点吸附到参考顶点；同一模型时自动焊接。\n使用：先加载参考边链，再选择一条或多条穿过参考边链的目标边，按缝合阈值执行。",
          "NitroPoly_CutAndStitch", "minus"),
    _spec("corner_connect", "平分", "连接工具", "corner_connect",
          "用途：按 NitroPoly 2.0 的角部算法建立平分连接，并自动处理角点拓扑。\n使用：选择两条或更多边；也可选择两个顶点建立角部，选择多个顶点执行普通连接。",
          "NitroPoly_CornerConnect"),
    _spec("end_connect", "四边末端", "连接工具", "end_connect",
          "用途：在六顶点末端区域自动寻找对边，建立四边末端连接。\n使用：只选择一条末端边后执行。",
          "NitroPoly_EndConnect"),
    _spec("distance_connect", "距离", "连接工具", "distance_connect",
          "用途：连接两条边；相邻边直接连接，不相邻时沿同一边环连接中间区域。\n使用：选择同一模型上的两条边后执行。",
          "NitroPoly_DistanceConnect"),
    _spec("flow_connect", "流", "连接工具", "flow_connect",
          "用途：按 Maya Edge Flow 建立贴合周围曲率的连接边。\n使用：选择同一模型上的两条或更多边后执行。",
          "NitroPoly_FlowConnect"),
    _spec("vertex_to_edge", "点到边", "连接工具", "vertex_to_edge",
          "用途：把顶点垂直投影到所选边，在投影位置切出新点后连接。\n使用：在 Multi 选择模式下选择一个顶点和一条同模型边。",
          "NitroPoly_VertEdge"),
    _spec("load_vertex", "加载顶点", "连接工具", "load_vertex",
          "用途：保存一个目标顶点，供“连接到顶点”使用。\n使用：选择一个顶点后执行。",
          "NitroPoly_LoadVertex", "plus"),
    _spec("connect_to_vertex", "连接到顶点", "连接工具", "connect_to_vertex",
          "用途：把当前所选顶点逐个连接到已加载顶点。\n使用：先加载目标顶点，再选择同一模型上的一个或多个其他顶点。",
          "NitroPoly_ConnectToVertex", "minus"),

    _spec("space_loop", "空间", "循环工具", "space_loop",
          "用途：沿原有边链弧长均匀分布顶点；开放边链保留两端。\n使用：选择一组或多组连续边链后执行，可重复执行。",
          "NitroPoly_Space"),
    _spec("straight_loop", "直线", "循环工具", "straight_loop",
          "用途：把开放边链内部顶点投影到两端连线。\n使用：选择至少两条连续开放边后执行。",
          "NitroPoly_Straight"),
    _spec("circle_loop", "圆形", "循环工具", "circle_loop",
          "用途：把闭合边环整理为等半径、等角度圆形。\n使用：选择闭合边环后执行。",
          "NitroPoly_Circle"),
    _spec("geo_poly", "多边形", "循环工具", "geo_poly",
          "用途：把所选面区域的外边界整理为规则多边形。\n使用：选择连续面区域后执行。",
          "NitroPoly_Geopoly"),
    _spec("view_planar", "视角平面", "循环工具", "view_planar",
          "用途：沿当前视角把所选组件压到同一平面。\n使用：选择顶点、边或面后执行。",
          "NitroPoly_ViewPlanar"),
    _spec("make_planar", "平均平面", "循环工具", "make_planar",
          "用途：按所选区域平均法线把组件压到同一平面。\n使用：选择顶点、边或面后执行。",
          "NitroPoly_MakePlanar"),
    _spec("center_loop", "中心", "循环工具", "center_loop",
          "用途：使用 Maya Edge Flow 把所选边调整到相邻面的中心流线上。\n使用：选择边后执行，可重复执行。",
          "NitroPoly_Center"),
    _spec("relax_loop", "松弛", "循环工具", "relax_loop",
          "用途：对所选顶点或由边、面转换得到的顶点执行一次平均松弛。\n使用：选择组件后执行，可重复执行。",
          "NitroPoly_Relax"),
]

SPEC_BY_ID = dict((item["id"], item) for item in TOOL_SPECS)
CATEGORIES = [
    "编辑选择",
    "网格编辑",
    "轴心点/解冻变换",
    "拓扑工具",
    "连接工具",
    "循环工具",
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
        raise NitroPolyError("无法读取组件编号：{}".format(component))
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
            raise NitroPolyError("所选组件必须属于同一个模型")
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
            raise NitroPolyError("没有可排序的边")
        cls.ensure_same_owner(edges)
        edge_vertices = {}
        vertex_edges = {}
        for edge in edges:
            vertices = cls.edge_vertices(edge)
            if len(vertices) != 2:
                raise NitroPolyError("无法读取边端点：{}".format(edge))
            edge_vertices[edge] = vertices
            for vertex in vertices:
                vertex_edges.setdefault(vertex, []).append(edge)
        if any(len(values) > 2 for values in vertex_edges.values()):
            raise NitroPolyError("选择中存在分叉")
        endpoints = [vertex for vertex, values in vertex_edges.items() if len(values) == 1]
        if len(endpoints) not in (0, 2):
            raise NitroPolyError("所选边不是单条连续边链")
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
            raise NitroPolyError("选择包含不连续边组")
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
            raise NitroPolyError("路径存在分叉")
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
            raise NitroPolyError("无法建立完整路径顺序")
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
            raise NitroPolyError("请选择边")
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
            raise NitroPolyError("当前选择没有可保留的内部边")
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
            raise NitroPolyError("请选择一条边作为起点")
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
                raise NitroPolyError("面模式需要选择两个相邻面")
            first_edges = set(self.query.face_edges(faces[0]))
            shared = first_edges.intersection(self.query.face_edges(faces[1]))
            if len(shared) != 1:
                raise NitroPolyError("两个面必须相邻")
            shared_edge = list(shared)[0]
            ring = self.query.full_edge_path(shared_edge, "ring")
            strip_faces = set(self.query.faces(ring))
            strip_edges = set(self.query.edges(list(strip_faces)))
            candidates = list(strip_edges.difference(ring))
            if not candidates:
                raise NitroPolyError("无法建立面循环")
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
            raise NitroPolyError("请选择多边形模型")
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
            raise NitroPolyError("请选择多边形模型")
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
            self.message("所选模型没有检测到 UV 接缝")

    def point_to_point(self):
        vertices = self.query.selection(31)
        if len(vertices) != 2:
            raise NitroPolyError("请选择两个顶点")
        owner = self.query.ensure_same_owner(vertices)
        transform = self.query.transform(owner)
        ids = (_index(vertices[0]), _index(vertices[1]))
        cmds.polySelect(transform, shortestEdgePath=ids)

    def face_fill(self):
        faces = self.query.selection(34)
        if len(faces) != 2:
            raise NitroPolyError("请选择两个面")
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
            raise NitroPolyError("两个面之间没有可用路径")
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
            raise NitroPolyError("请选择两个或更多多边形模型")
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
            raise NitroPolyError("请选择需要分离的面")
        owner = self.query.ensure_same_owner(faces)
        transform = self.query.transform(owner)
        face_count = cmds.polyEvaluate(owner, face=True)
        selected_ids = set(_index(face) for face in faces)
        if len(selected_ids) >= face_count:
            raise NitroPolyError("不能分离模型的全部面")
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
                raise NitroPolyError("请选择至少两个顶点")
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
        raise NitroPolyError("请选择对象或多边形组件")

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
        raise NitroPolyError("请选择对象或多边形组件")


class TransformTools(ToolBase):
    def base_pivot(self):
        transforms = self.query.selected_transforms()
        if not transforms:
            raise NitroPolyError("请选择对象")
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
            raise NitroPolyError("请选择对象")
        for transform in transforms:
            cmds.xform(transform, worldSpace=True, pivots=(0.0, 0.0, 0.0))

    def unfreeze_translate(self):
        transforms = self.query.selected_transforms()
        if not transforms:
            raise NitroPolyError("请选择对象")
        for transform in transforms:
            locked = [
                attribute for attribute in ("translateX", "translateY", "translateZ")
                if cmds.getAttr(transform + "." + attribute, lock=True)
            ]
            if locked:
                raise NitroPolyError("{} 的位移通道已锁定".format(transform))
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
            raise NitroPolyError("请选择对象")
        for transform in transforms:
            cmds.move(0.0, 0.0, 0.0, transform, rotatePivotRelative=True)


class TopologyTools(ToolBase):
    def _corner_rotate(self, degrees):
        edges = self.query.selection(32)
        faces = self.query.selection(34)
        if len(edges) != 1 or not faces:
            raise NitroPolyError("需要同时选择一条边和一个或多个面")
        components = edges + faces
        self.query.ensure_same_owner(components)
        edge_vertices = self.query.edge_vertices(edges[0])
        if len(edge_vertices) != 2:
            raise NitroPolyError("无法读取旋转轴")
        positions = [
            cmds.xform(vertex, query=True, worldSpace=True, translation=True)
            for vertex in edge_vertices
        ]
        pivot = _v_mul(_v_add(positions[0], positions[1]), 0.5)
        axis = _v_normal(_v_sub(positions[1], positions[0]))
        if _v_length(axis) <= 1e-12:
            raise NitroPolyError("旋转轴长度为零")
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
            raise NitroPolyError("请选择一条边界边")
        edge = edges[0]
        connected_faces = self.query.edge_faces(edge)
        if len(connected_faces) != 1:
            raise NitroPolyError("所选边必须是边界边")
        face = connected_faces[0]
        face_vertices = self.query.face_vertices(face)
        if len(face_vertices) != 4:
            raise NitroPolyError("F2 扩展要求相邻面为四边面")
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
                raise NitroPolyError("无法判断四边面外推方向")
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
            raise NitroPolyError("F2 扩展没有生成两个新顶点")
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
            raise NitroPolyError("请选择至少三条连续倒角轮廓边")
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
            raise NitroPolyError("所选边不是可调整的开放倒角轮廓")
        for vertex, targets in moves.items():
            cmds.xform(vertex, worldSpace=True, translation=_v_average(targets))
        cmds.select(edges, replace=True)
        if valid < len(groups):
            self.message("部分边组不符合倒角轮廓条件，已跳过")

    def bevel_plus(self):
        self._bevel(True)

    def bevel_minus(self):
        self._bevel(False)


class ConnectTools(ToolBase):
    def load_edge_loop(self):
        edges = self.query.selection(32)
        if not edges:
            raise NitroPolyError("请选择参考边链")
        self.query.ensure_same_owner(edges)
        self.query.ordered_edge_chain(edges)
        self.app.state["loaded_edges"] = list(edges)
        self.app.ui.set_status("loaded_edges", "{} 条边".format(len(edges)))

    def cut_stitch(self):
        target_edges = self.query.selection(32)
        reference_edges = self.app.state.get("loaded_edges") or []
        if not target_edges:
            raise NitroPolyError("请选择需要切割的目标边")
        if not reference_edges or any(
                not cmds.objExists(edge) for edge in reference_edges):
            self.app.state["loaded_edges"] = []
            self.app.ui.set_status("loaded_edges", "无循环边")
            raise NitroPolyError("请重新加载参考边链")
        if len(reference_edges) < len(target_edges):
            raise NitroPolyError("目标边数量不能多于已加载参考边")

        self.query.ensure_same_owner(reference_edges)
        target_owner = self.query.ensure_same_owner(target_edges)
        _, reference_vertices = self.query.ordered_edge_chain(reference_edges)
        if len(reference_vertices) < 2:
            raise NitroPolyError("参考边链无效")

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
            raise NitroPolyError("目标边没有在缝合阈值内穿过参考边链")

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
                raise NitroPolyError("请选择两个顶点")
        elif selected_edges:
            if len(selected_edges) != 1:
                raise NitroPolyError("请选择一条边")
            edge = selected_edges[0]
            vertices = self.query.edge_vertices(edge)
            if len(vertices) != 2:
                raise NitroPolyError("无法读取边端点")
        else:
            raise NitroPolyError("请选择两个顶点或一条边")

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
            raise NitroPolyError("角部连接失败")
        before = cmds.polyEvaluate(owner, vertex=True)
        cmds.select(created_edges, replace=True)
        cmds.polySubdivideEdge(
            divisions=1,
            constructionHistory=False
        )
        after = cmds.polyEvaluate(owner, vertex=True)
        if after <= before:
            raise NitroPolyError("未生成角部顶点")
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
                raise NitroPolyError("请选择至少两条边")
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
                raise NitroPolyError("请选择至少两个顶点")
            self.query.ensure_same_owner(vertices)
            if len(vertices) == 2:
                self._connect_corner()
            else:
                self._soft_edge_connect(vertices)
            return
        raise NitroPolyError("请选择边或顶点")

    def end_connect(self):
        edges = self.query.selection(32)
        if len(edges) != 1:
            raise NitroPolyError("请选择一条末端边")
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
            raise NitroPolyError("所选边必须位于唯一的六顶点末端区域")

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
            raise NitroPolyError("未找到唯一的末端对边")

        created_edges = self._soft_edge_connect([edge, opposite_edges[0]])
        if not created_edges:
            raise NitroPolyError("末端连接失败")
        pre_vertices = set(self.query.vertices(created_edges))
        cmds.select(created_edges, replace=True)
        cmds.polySubdivideEdge(constructionHistory=False)
        post_vertices = set(self.query.vertices(created_edges))
        middle_vertices = sorted(
            post_vertices.difference(pre_vertices),
            key=_index
        )
        if not middle_vertices:
            raise NitroPolyError("未生成末端中间顶点")

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
            raise NitroPolyError("请选择两条边")
        owner = self.query.ensure_same_owner(edges)
        before = cmds.polyEvaluate(owner, vertex=True)

        adjacent_edges = set(self.query.edges(self.query.edge_faces(edges[0])))
        if edges[1] in adjacent_edges:
            connect_edges = edges
        else:
            path = self.query.full_edge_path(edges[0], "ring")
            ordered, closed = self.query.order_edge_path(path, "ring")
            if edges[1] not in ordered:
                raise NitroPolyError("两条边不在同一边环")
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
            raise NitroPolyError("没有生成新的连接边")
        cmds.polySoftEdge(
            new_edges,
            angle=180,
            constructionHistory=False
        )
        cmds.select(new_edges, replace=True)

    def flow_connect(self):
        edges = self.query.selection(32)
        if len(edges) < 2:
            raise NitroPolyError("请选择至少两条边")
        owner = self.query.ensure_same_owner(edges)
        before = cmds.polyEvaluate(owner, vertex=True)
        cmds.polyConnectComponents(
            edges,
            insertWithEdgeFlow=True,
            constructionHistory=False
        )
        new_edges = self._new_edges_after(owner, before)
        if not new_edges:
            raise NitroPolyError("没有生成新的流连接边")
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
            raise NitroPolyError("请选择一个顶点和一条边")
        vertex = vertices[0]
        edge = edges[0]
        owner = self.query.ensure_same_owner([vertex, edge])
        edge_vertices = self.query.edge_vertices(edge)
        if len(edge_vertices) != 2:
            raise NitroPolyError("无法读取目标边端点")

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
            raise NitroPolyError("目标边长度为零")
        parameter = _v_dot(_v_sub(point, start), line) / length_sq
        if parameter < -1e-7 or parameter > 1.0000001:
            raise NitroPolyError("顶点投影不在所选边线段内")
        parameter = max(0.0, min(1.0, parameter))

        before = cmds.polyEvaluate(owner, vertex=True)
        cmds.polySplit(
            owner,
            ip=[(_index(edge), parameter)],
            ch=False
        )
        after = cmds.polyEvaluate(owner, vertex=True)
        if after <= before:
            raise NitroPolyError("没有在目标边上生成新顶点")
        new_vertex = "{}.vtx[{}]".format(owner, before)
        cmds.polyConnectComponents(
            [vertex, new_vertex],
            constructionHistory=False
        )
        cmds.select(new_vertex, replace=True)

    def load_vertex(self):
        vertices = self.query.selection(31)
        if len(vertices) != 1:
            raise NitroPolyError("请选择一个顶点")
        self.app.state["loaded_vertex"] = vertices[0]
        self.app.ui.set_status(
            "loaded_vertex",
            "vtx[{}]".format(_index(vertices[0]))
        )

    def connect_to_vertex(self):
        target = self.app.state.get("loaded_vertex")
        if not target or not cmds.objExists(target):
            self.app.state["loaded_vertex"] = ""
            self.app.ui.set_status("loaded_vertex", "没有加载顶点")
            raise NitroPolyError("请重新加载目标顶点")
        selected = self.query.selection(31)
        selected = [item for item in selected if item != target]
        if not selected:
            raise NitroPolyError("请选择需要连接的其他顶点")
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
            raise NitroPolyError("没有生成连接边")
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
            raise NitroPolyError("请选择连续边")
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
                raise NitroPolyError("直线工具不能处理闭合边环")
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
            raise NitroPolyError("圆形工具要求闭合边环")
        vertices = vertices[:-1]
        points = [
            cmds.xform(vertex, query=True, worldSpace=True, translation=True)
            for vertex in vertices
        ]
        center = _v_average(points)
        normal = _newell_normal(points)
        if _v_length(normal) <= 1e-12:
            raise NitroPolyError("无法计算边环平面")
        first_vector = _project_to_plane(points[0], center, normal)
        first_vector = _v_sub(first_vector, center)
        basis_u = _v_normal(first_vector)
        if _v_length(basis_u) <= 1e-12:
            raise NitroPolyError("无法建立圆形方向")
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
            raise NitroPolyError("请选择连续面区域")
        owner = self.query.ensure_same_owner(faces)
        selected_faces = set(faces)
        boundary = []
        for edge in set(self.query.edges(faces)):
            connected = set(self.query.edge_faces(edge))
            if len(connected.intersection(selected_faces)) == 1:
                boundary.append(edge)
        groups = self.query.edge_groups(boundary)
        if not groups:
            raise NitroPolyError("没有找到面区域外边界")
        for group in groups:
            self._circle_edges(group)
        cmds.select(faces, replace=True)

    def _selected_vertices(self):
        selection = self.query.selection()
        vertices = self.query.vertices(selection)
        if len(vertices) < 3:
            raise NitroPolyError("请选择至少三个有效顶点")
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
            raise NitroPolyError("无法取得当前视图相机")
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
            raise NitroPolyError("请选择至少三个有效顶点")
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
            raise NitroPolyError("无法计算平均平面")
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
            raise NitroPolyError("请选择边")
        cmds.select(edges, replace=True)
        mel.eval("polyEditEdgeFlow -adjustEdgeFlow 1;")

    def relax_loop(self):
        vertices = self.query.vertices(self.query.selection())
        if not vertices:
            raise NitroPolyError("请选择顶点、边或面")
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
        text = "{}  ·  {}\n\n{}".format(
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
        self._frame("编辑选择")
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
        self._frame("网格编辑")
        self._row(4, [96, 96, 96, 96])
        for tool_id in ("combine_clean", "detach_clean", "uni_connect", "uni_remove"):
            self._button(tool_id, 94)
        cmds.setParent("..")
        cmds.setParent("..")

    def _build_transform(self):
        self._frame("轴心点/解冻变换")
        self._row(4, [96, 96, 96, 96])
        for tool_id in ("base_pivot", "world_pivot", "unfreeze_translate", "move_to_origin"):
            self._button(tool_id, 94)
        cmds.setParent("..")
        cmds.setParent("..")

    def _build_topology(self):
        self._frame("拓扑工具")
        self._row(2, [194, 194])
        self._button("corner_plus", 190)
        self._button("corner_minus", 190)
        cmds.setParent("..")

        self.controls["f2_threshold"] = cmds.floatSliderGrp(
            label="F2 阈值",
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
            label="倒角步长",
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
        self._frame("连接工具")
        self.controls["stitch_threshold"] = cmds.floatSliderGrp(
            label="缝合阈值",
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
            text="无循环边", editable=False, width=98,
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
            text="没有加载顶点", editable=False, width=98,
            backgroundColor=self.color(39, 39, 47)
        )
        self._button("load_vertex", 121)
        self._button("connect_to_vertex", 151)
        cmds.setParent("..")
        cmds.setParent("..")

    def _build_loop(self):
        self._frame("循环工具")
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
        self._frame("功能介绍")
        self.help_field = cmds.scrollField(
            editable=False,
            wordWrap=True,
            height=120,
            text="鼠标移入功能按钮时，这里会立即显示该功能的用途和使用方法。"
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
        cmds.warning("NitroPoly：{}".format(text))

    def execute(self, tool_id, *args):
        if tool_id not in self.routes:
            raise NitroPolyError("未注册功能：{}".format(tool_id))
        try:
            with execution_context(tool_id):
                return self.routes[tool_id]()
        except NitroPolyError as exc:
            self.message(str(exc))
        except Exception as exc:
            traceback.print_exc()
            self.message("{}：{}".format(SPEC_BY_ID[tool_id]["label"], exc))
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
                cmds.warning("NitroPoly：无法注册运行时命令 {}".format(name))


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
