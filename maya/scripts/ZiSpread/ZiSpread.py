# -*- coding: utf-8 -*-
from __future__ import division, print_function

import math
import re

import maya.cmds as cmds
import maya.api.OpenMaya as om


__author__ = "Alexis_Lee"
__version__ = "1.0.2"

_CONTEXT_NAME = "ziSpreadRebuiltContext"
_STEP_PIXELS = 5.0
_MAX_ITERATIONS = 100
_EPSILON = 1.0e-10

_TOOL = None


class ZiSpreadError(RuntimeError):
    pass


def _copy_point(point):
    return om.MPoint(point.x, point.y, point.z, point.w)


def _copy_points(points):
    result = om.MPointArray()
    for point in points:
        result.append(_copy_point(point))
    return result


def _dot(vector_a, vector_b):
    return (
        vector_a.x * vector_b.x
        + vector_a.y * vector_b.y
        + vector_a.z * vector_b.z
    )


def _is_finite(value):
    return not math.isnan(value) and not math.isinf(value)


def _equalized_position(point_a, point_b, point_c):
    vector_ab = point_b - point_a
    vector_cb = point_b - point_c
    distance_ab = vector_ab.length()
    distance_bc = vector_cb.length()

    if distance_ab <= _EPSILON or distance_bc <= _EPSILON:
        return _copy_point(point_b)

    if abs(distance_ab - distance_bc) <= _EPSILON:
        return _copy_point(point_b)

    if distance_ab > distance_bc:
        origin = point_a
        opposite = point_c
        ray = point_b - point_a
    else:
        origin = point_c
        opposite = point_a
        ray = point_b - point_c

    base = opposite - origin
    ray_length = ray.length()
    base_length = base.length()

    if ray_length <= _EPSILON or base_length <= _EPSILON:
        return _copy_point(point_b)

    cosine = _dot(ray, base) / (ray_length * base_length)
    cosine = max(-1.0, min(1.0, cosine))

    denominator = 2.0 * cosine
    if abs(denominator) <= _EPSILON:
        return _copy_point(point_b)

    target_length = base_length / denominator
    if target_length <= 0.0 or not _is_finite(target_length):
        return _copy_point(point_b)

    direction = ray * (1.0 / ray_length)
    result = origin + direction * target_length
    return om.MPoint(result.x, result.y, result.z, point_b.w)


def _component_edge_id(component):
    match = re.search(r"\.e\[(\d+)\]$", component)
    if not match:
        raise ZiSpreadError("无法识别边组件：{0}".format(component))
    return int(match.group(1))


def _mesh_shape_from_component(component):
    node_name = component.rsplit(".e[", 1)[0]
    matches = cmds.ls(node_name, long=True) or []
    if not matches:
        raise ZiSpreadError("找不到模型：{0}".format(node_name))

    node_name = matches[0]
    node_type = cmds.nodeType(node_name)

    if node_type == "mesh":
        return node_name

    if node_type == "transform":
        shapes = cmds.listRelatives(
            node_name,
            shapes=True,
            noIntermediate=True,
            fullPath=True,
            type="mesh",
        ) or []
        if shapes:
            return shapes[0]

    raise ZiSpreadError("所选组件不属于多边形模型。")


def _dag_path(mesh_shape):
    selection = om.MSelectionList()
    selection.add(mesh_shape)
    dag_path = selection.getDagPath(0)
    if dag_path.apiType() == om.MFn.kTransform:
        dag_path.extendToShape()
    return dag_path


def _face_strip_neighbor(face_vertices, center_vertex, edge_other_vertex):
    try:
        center_index = face_vertices.index(center_vertex)
    except ValueError:
        return None

    previous_vertex = face_vertices[center_index - 1]
    next_vertex = face_vertices[(center_index + 1) % len(face_vertices)]

    if previous_vertex == edge_other_vertex:
        return next_vertex
    if next_vertex == edge_other_vertex:
        return previous_vertex
    return None


class ZiSpreadTool(object):
    def __init__(self, preserve_uvs=False, realtime=True, display=True):
        self.preserve_uvs = bool(preserve_uvs)
        self.realtime = bool(realtime)
        self.display = bool(display)

        self.mesh_shape = None
        self.dag_path = None
        self.mesh_fn = None
        self.triples = []
        self.center_vertices = []
        self.original_points = None
        self.cache = []
        self.anchor_x = 0.0
        self.direction = 0.0
        self.target_iterations = 0
        self.applied_iterations = 0
        self.dragging = False
        self.skipped_edges = 0

    def install_context(self):
        if cmds.draggerContext(_CONTEXT_NAME, exists=True):
            cmds.deleteUI(_CONTEXT_NAME)

        cmds.draggerContext(
            _CONTEXT_NAME,
            pressCommand=self.press,
            dragCommand=self.drag,
            releaseCommand=self.release,
            finalize=self.finalize,
            cursor="crossHair",
            projection="viewPlane",
            space="screen",
            undoMode="step",
        )
        cmds.setToolTo(_CONTEXT_NAME)
        self._set_overlay("ZiSpread：选择四边面边环后横向拖动")

    def press(self):
        try:
            self._prepare_selection()
            anchor = cmds.draggerContext(
                _CONTEXT_NAME, query=True, anchorPoint=True
            )
            self.anchor_x = float(anchor[0])
            self.direction = 0.0
            self.target_iterations = 0
            self.applied_iterations = 0
            self.dragging = True
            self._set_overlay(
                "ZiSpread | 迭代 0 | 有效顶点 {0}".format(
                    len(self.center_vertices)
                )
            )
        except Exception as exc:
            self.dragging = False
            self._show_error(str(exc))

    def drag(self):
        if not self.dragging:
            return

        try:
            drag_point = cmds.draggerContext(
                _CONTEXT_NAME, query=True, dragPoint=True
            )
            delta_x = float(drag_point[0]) - self.anchor_x

            if self.direction == 0.0 and abs(delta_x) >= _STEP_PIXELS:
                self.direction = 1.0 if delta_x > 0.0 else -1.0

            if self.direction == 0.0:
                target = 0
            else:
                distance = max(0.0, delta_x * self.direction)
                target = int(distance / _STEP_PIXELS)

            target = max(0, min(_MAX_ITERATIONS, target))
            self.target_iterations = target
            self._ensure_cache(target)

            if self.realtime and target != self.applied_iterations:
                self._apply_points(self.cache[target])
                self.applied_iterations = target
                cmds.refresh(force=True)

            self._set_overlay(
                "ZiSpread | 迭代 {0}/{1} | 有效顶点 {2}".format(
                    target,
                    _MAX_ITERATIONS,
                    len(self.center_vertices),
                )
            )
        except Exception as exc:
            self._restore_original()
            self.dragging = False
            self._show_error(str(exc))

    def release(self):
        if not self.dragging:
            return

        self.dragging = False

        try:
            self._ensure_cache(self.target_iterations)
            final_points = self.cache[self.target_iterations]

            if not self.realtime:
                self._apply_points(final_points)
                cmds.refresh(force=True)

            if self.target_iterations <= 0:
                self._restore_original()
                self._set_overlay("ZiSpread：未产生修改")
                return

            self._commit_undoable(final_points)
            self.applied_iterations = self.target_iterations
            self._set_overlay(
                "ZiSpread：完成 {0} 次迭代，可按 Ctrl+Z 撤销".format(
                    self.target_iterations
                )
            )
        except Exception as exc:
            self._restore_original()
            self._show_error(str(exc))

    def finalize(self):
        if self.dragging:
            self._restore_original()
            self.dragging = False

    def stop(self, restore=False):
        if restore and self.dragging:
            self._restore_original()
        self.dragging = False
        if cmds.draggerContext(_CONTEXT_NAME, exists=True):
            cmds.setToolTo("selectSuperContext")

    def _prepare_selection(self):
        selected_edges = cmds.filterExpand(
            cmds.ls(selection=True, flatten=True) or [],
            selectionMask=32,
            expand=True,
        ) or []

        if not selected_edges:
            raise ZiSpreadError("请先选择一组多边形边。")

        mesh_shape = _mesh_shape_from_component(selected_edges[0])
        edge_ids = []

        for component in selected_edges:
            component_shape = _mesh_shape_from_component(component)
            if component_shape != mesh_shape:
                raise ZiSpreadError("一次只能处理一个模型上的边。")
            edge_ids.append(_component_edge_id(component))

        self.mesh_shape = mesh_shape
        self.dag_path = _dag_path(mesh_shape)
        self.mesh_fn = om.MFnMesh(self.dag_path)
        self.original_points = _copy_points(
            self.mesh_fn.getPoints(om.MSpace.kObject)
        )
        self.cache = [_copy_points(self.original_points)]
        self.triples = []
        self.center_vertices = []
        self.skipped_edges = 0

        triples_by_center = {}
        edge_iterator = om.MItMeshEdge(self.dag_path)

        for edge_id in sorted(set(edge_ids)):
            edge_iterator.setIndex(edge_id)
            connected_faces = list(edge_iterator.getConnectedFaces())

            if edge_iterator.onBoundary() or len(connected_faces) != 2:
                self.skipped_edges += 1
                continue

            face_a = list(self.mesh_fn.getPolygonVertices(connected_faces[0]))
            face_b = list(self.mesh_fn.getPolygonVertices(connected_faces[1]))

            if len(face_a) != 4 or len(face_b) != 4:
                self.skipped_edges += 1
                continue

            vertex_a, vertex_b = self.mesh_fn.getEdgeVertices(edge_id)

            for center_vertex, other_vertex in (
                (vertex_a, vertex_b),
                (vertex_b, vertex_a),
            ):
                neighbor_a = _face_strip_neighbor(
                    face_a, center_vertex, other_vertex
                )
                neighbor_b = _face_strip_neighbor(
                    face_b, center_vertex, other_vertex
                )

                if (
                    neighbor_a is None
                    or neighbor_b is None
                    or neighbor_a == neighbor_b
                ):
                    continue

                triples_by_center[center_vertex] = (
                    neighbor_a,
                    center_vertex,
                    neighbor_b,
                )

        if not triples_by_center:
            raise ZiSpreadError(
                "没有可处理的内部四边面边环；边界边、三角面和非流形边会被跳过。"
            )

        self.center_vertices = sorted(triples_by_center.keys())
        self.triples = [
            triples_by_center[index] for index in self.center_vertices
        ]

    def _ensure_cache(self, target_iterations):
        while len(self.cache) <= target_iterations:
            source_points = self.cache[-1]
            result_points = _copy_points(source_points)

            for neighbor_a, center_vertex, neighbor_b in self.triples:
                result_points[center_vertex] = _equalized_position(
                    source_points[neighbor_a],
                    source_points[center_vertex],
                    source_points[neighbor_b],
                )

            self.cache.append(result_points)

    def _apply_points(self, points):
        if self.mesh_fn is None:
            return
        self.mesh_fn.setPoints(points, om.MSpace.kObject)
        self.mesh_fn.updateSurface()

    def _restore_original(self):
        if self.original_points is None or self.mesh_fn is None:
            return
        self._apply_points(self.original_points)
        cmds.refresh(force=True)
        self.applied_iterations = 0

    def _commit_undoable(self, final_points):
        final_positions = {}
        for vertex_id in self.center_vertices:
            final_positions[vertex_id] = _copy_point(final_points[vertex_id])

        chunk_open = False
        refresh_suspended = False
        try:
            cmds.refresh(suspend=True)
            refresh_suspended = True

            # 实时预览由 API 直接写入，不进入 Maya 撤销队列。
            # 在禁止视口刷新的状态下恢复原始点，再用可撤销命令提交最终点。
            self._apply_points(self.original_points)

            cmds.undoInfo(openChunk=True, chunkName="ZiSpread")
            chunk_open = True

            for vertex_id in self.center_vertices:
                point = final_positions[vertex_id]
                component = "{0}.vtx[{1}]".format(
                    self.mesh_shape, vertex_id
                )
                kwargs = {
                    "a": True,
                    "os": True,
                    "t": [point.x, point.y, point.z],
                }
                if self.preserve_uvs:
                    kwargs["puv"] = True
                cmds.xform(component, **kwargs)
        finally:
            if chunk_open:
                cmds.undoInfo(closeChunk=True)
            if refresh_suspended:
                cmds.refresh(suspend=False)

        cmds.refresh(force=True)
        self.mesh_fn = om.MFnMesh(self.dag_path)
        self.original_points = _copy_points(
            self.mesh_fn.getPoints(om.MSpace.kObject)
        )
        self.cache = [_copy_points(self.original_points)]

    def _set_overlay(self, text):
        if not self.display:
            return
        if cmds.draggerContext(_CONTEXT_NAME, exists=True):
            try:
                cmds.draggerContext(
                    _CONTEXT_NAME, edit=True, drawString=text
                )
            except Exception:
                pass

    @staticmethod
    def _show_error(message):
        om.MGlobal.displayError("ZiSpread: {0}".format(message))


def main(preserveUVs=False, realtime=True, display=True):
    global _TOOL

    if _TOOL is not None:
        try:
            _TOOL.stop(restore=True)
        except Exception:
            pass

    _TOOL = ZiSpreadTool(
        preserve_uvs=preserveUVs,
        realtime=realtime,
        display=display,
    )
    _TOOL.install_context()
    return _TOOL


def stop(restore=False):
    global _TOOL
    if _TOOL is not None:
        _TOOL.stop(restore=restore)
    _TOOL = None


main()