# -*- coding: utf-8 -*-

"""
Flatten Mesh To UV

根据所选多边形模型的 UV Set 创建一个位于 XY 平面的新网格。

主要特性：
    1. UV 接缝会按照“原顶点 ID + UV ID”正确拆点。
    2. 支持指定缩放、输出名称和 UV Set。
    3. 输出模型继承源 UV Set 名称。
    4. 使用 MPxCommand 实现完整的一步撤销和重做。
    5. 创建失败时会清理已经生成的临时节点。
    6. 兼容 Maya Python 2 / Python 3。
"""

from __future__ import print_function

import maya.cmds as cmds
import maya.api.OpenMaya as om

try:
    from itertools import izip as zip_iterator
except ImportError:
    zip_iterator = zip


COMMAND_NAME = "flattenMeshToUV"
PLUGIN_VENDOR = "Alexis_Lee"
PLUGIN_VERSION = "2.0.1"


def maya_useNewAPI():
    """通知 Maya 该插件使用 Python API 2.0。"""
    pass


def _to_mint_array(values):
    result = om.MIntArray()
    for value in values:
        result.append(int(value))
    return result


def _to_mfloat_array(values):
    result = om.MFloatArray()
    for value in values:
        result.append(float(value))
    return result


def _to_mpoint_array(values):
    result = om.MPointArray()
    for x_value, y_value, z_value in values:
        result.append(
            om.MPoint(
                float(x_value),
                float(y_value),
                float(z_value)
            )
        )
    return result


class FlattenMeshToUVCommand(om.MPxCommand):
    """把所选模型的 UV 布局转换成平面网格。"""

    SCALE_SHORT_FLAG = "-s"
    SCALE_LONG_FLAG = "-scale"

    NAME_SHORT_FLAG = "-n"
    NAME_LONG_FLAG = "-name"

    UV_SET_SHORT_FLAG = "-uv"
    UV_SET_LONG_FLAG = "-uvSet"

    def __init__(self):
        om.MPxCommand.__init__(self)

        self._scale = 1.0
        self._requested_output_name = None
        self._requested_uv_set = None

        self._source_transform = None
        self._source_uv_set = None

        self._point_data = []
        self._polygon_counts = []
        self._polygon_connects = []
        self._u_values = []
        self._v_values = []

        self._previous_selection = None

        self._output_handle = None
        self._output_transform_path = None

    @staticmethod
    def creator():
        return FlattenMeshToUVCommand()

    @staticmethod
    def create_syntax():
        syntax = om.MSyntax()

        syntax.addFlag(
            FlattenMeshToUVCommand.SCALE_SHORT_FLAG,
            FlattenMeshToUVCommand.SCALE_LONG_FLAG,
            om.MSyntax.kDouble
        )

        syntax.addFlag(
            FlattenMeshToUVCommand.NAME_SHORT_FLAG,
            FlattenMeshToUVCommand.NAME_LONG_FLAG,
            om.MSyntax.kString
        )

        syntax.addFlag(
            FlattenMeshToUVCommand.UV_SET_SHORT_FLAG,
            FlattenMeshToUVCommand.UV_SET_LONG_FLAG,
            om.MSyntax.kString
        )

        return syntax

    def isUndoable(self):
        return True

    def doIt(self, arguments):
        """解析参数、缓存源数据，并执行第一次创建。"""

        argument_database = om.MArgDatabase(
            self.syntax(),
            arguments
        )

        if argument_database.isFlagSet(self.SCALE_SHORT_FLAG):
            self._scale = argument_database.flagArgumentDouble(
                self.SCALE_SHORT_FLAG,
                0
            )

        if argument_database.isFlagSet(self.NAME_SHORT_FLAG):
            self._requested_output_name = (
                argument_database.flagArgumentString(
                    self.NAME_SHORT_FLAG,
                    0
                )
            )

        if argument_database.isFlagSet(self.UV_SET_SHORT_FLAG):
            self._requested_uv_set = (
                argument_database.flagArgumentString(
                    self.UV_SET_SHORT_FLAG,
                    0
                )
            )

        if self._scale <= 0.0:
            raise RuntimeError(u"scale 必须大于 0。")

        if (
            self._requested_output_name is not None
            and not self._requested_output_name.strip()
        ):
            raise RuntimeError(u"输出名称不能为空。")

        if (
            self._requested_output_name is not None
            and "|" in self._requested_output_name
        ):
            raise RuntimeError(
                u"输出名称只能填写节点短名称，不能包含 DAG 路径分隔符 |。"
            )

        self._previous_selection = (
            om.MGlobal.getActiveSelectionList()
        )

        self._capture_source_mesh_data()
        self.redoIt()

    def redoIt(self):
        """创建或重建输出模型。"""

        self._delete_output_immediately()

        try:
            points = _to_mpoint_array(self._point_data)
            polygon_counts = _to_mint_array(
                self._polygon_counts
            )
            polygon_connects = _to_mint_array(
                self._polygon_connects
            )
            u_values = _to_mfloat_array(self._u_values)
            v_values = _to_mfloat_array(self._v_values)

            new_mesh_fn = om.MFnMesh()

            # 未指定 parent 时，create() 会创建新的 Transform 和 Mesh Shape。
            # 返回值是新 Transform 的 MObject；MFnMesh 本身绑定到新 Shape。
            new_transform_object = new_mesh_fn.create(
                points,
                polygon_counts,
                polygon_connects,
                u_values,
                v_values
            )

            self._output_handle = om.MObjectHandle(
                new_transform_object
            )

            new_shape_object = new_mesh_fn.object()

            # 新几何顶点 ID 与新 UV ID 一一对应，
            # 因此 polygon_connects 同时也是面顶点 UV ID。
            new_mesh_fn.assignUVs(
                polygon_counts,
                polygon_connects
            )

            output_uv_set = new_mesh_fn.currentUVSetName()

            if output_uv_set != self._source_uv_set:
                new_mesh_fn.renameUVSet(
                    output_uv_set,
                    self._source_uv_set
                )

            transform_fn = om.MFnDagNode(
                new_transform_object
            )

            actual_transform_name = transform_fn.setName(
                self._requested_output_name
            )

            shape_fn = om.MFnDagNode(new_shape_object)
            shape_fn.setName(
                actual_transform_name.rsplit("|", 1)[-1]
                + "Shape"
            )

            self._assign_initial_material(new_shape_object)

            output_path = om.MDagPath.getAPathTo(
                new_transform_object
            )

            self._output_transform_path = (
                output_path.fullPathName()
            )

            new_selection = om.MSelectionList()
            new_selection.add(output_path)

            om.MGlobal.setActiveSelectionList(
                new_selection,
                om.MGlobal.kReplaceList
            )

            self.setResult(self._output_transform_path)

            om.MGlobal.displayInfo(
                u"已创建 UV 平面模型：{} | UV Set：{} | 缩放：{}".format(
                    self._output_transform_path,
                    self._source_uv_set,
                    self._scale
                )
            )

        except Exception:
            self._delete_output_immediately()
            self._restore_previous_selection()
            raise

    def undoIt(self):
        """删除输出模型并恢复执行前的选择。"""

        output_path = self._output_transform_path

        self._delete_output_immediately()
        self._restore_previous_selection()

        if output_path:
            om.MGlobal.displayInfo(
                u"已撤销 UV 平面模型：{}".format(output_path)
            )

    def _capture_source_mesh_data(self):
        """读取并缓存源模型拓扑和 UV 数据。"""

        selected_objects = cmds.ls(
            selection=True,
            objectsOnly=True,
            long=True
        ) or []

        unique_selected_objects = []
        seen_objects = set()

        for selected_object in selected_objects:
            if selected_object in seen_objects:
                continue

            seen_objects.add(selected_object)
            unique_selected_objects.append(selected_object)

        if len(unique_selected_objects) != 1:
            raise RuntimeError(u"请只选择一个多边形模型。")

        selected_node = unique_selected_objects[0]
        selected_type = cmds.nodeType(selected_node)

        if selected_type == "mesh":
            if cmds.getAttr(
                selected_node + ".intermediateObject"
            ):
                raise RuntimeError(
                    u"不能处理 Intermediate Mesh Shape。"
                )

            mesh_shape = selected_node

            parents = cmds.listRelatives(
                mesh_shape,
                parent=True,
                fullPath=True
            ) or []

            if not parents:
                raise RuntimeError(
                    u"无法取得模型的 Transform 节点。"
                )

            source_transform = parents[0]

        elif selected_type == "transform":
            source_transform = selected_node

            mesh_shapes = cmds.listRelatives(
                source_transform,
                shapes=True,
                noIntermediate=True,
                fullPath=True,
                type="mesh"
            ) or []

            if not mesh_shapes:
                raise RuntimeError(
                    u"选择的 Transform 下没有有效的 Mesh Shape。"
                )

            if len(mesh_shapes) > 1:
                raise RuntimeError(
                    u"选择的 Transform 下存在多个 Mesh Shape。"
                    u"请直接选择需要处理的 Mesh Shape。"
                )

            mesh_shape = mesh_shapes[0]

        else:
            raise RuntimeError(
                u"选择的对象不是有效的多边形模型。"
            )

        source_selection = om.MSelectionList()
        source_selection.add(mesh_shape)

        source_mesh_path = source_selection.getDagPath(0)
        source_mesh_fn = om.MFnMesh(source_mesh_path)

        uv_set_names = list(source_mesh_fn.getUVSetNames())

        if self._requested_uv_set:
            uv_set = self._requested_uv_set

            if uv_set not in uv_set_names:
                raise RuntimeError(
                    u'模型中不存在 UV Set："{}"。'.format(
                        uv_set
                    )
                )
        else:
            uv_set = source_mesh_fn.currentUVSetName()

        if not uv_set:
            raise RuntimeError(
                u"模型没有可用的当前 UV Set。"
            )

        u_values, v_values = source_mesh_fn.getUVs(uv_set)

        if len(u_values) == 0:
            raise RuntimeError(
                u'UV Set "{}" 中没有 UV。'.format(uv_set)
            )

        polygon_counts, polygon_vertex_ids = (
            source_mesh_fn.getVertices()
        )

        uv_counts, face_vertex_uv_ids = (
            source_mesh_fn.getAssignedUVs(uv_set)
        )

        if list(polygon_counts) != list(uv_counts):
            raise RuntimeError(
                u'UV Set "{}" 中存在没有完整分配 UV 的面。'.format(
                    uv_set
                )
            )

        if len(polygon_vertex_ids) != len(face_vertex_uv_ids):
            raise RuntimeError(
                u"模型拓扑数据与 UV 分配数据不一致。"
            )

        point_data = []
        polygon_connects = []
        new_u_values = []
        new_v_values = []

        vertex_uv_to_new_id = {}

        for original_vertex_id, uv_id in zip_iterator(
            polygon_vertex_ids,
            face_vertex_uv_ids
        ):
            original_vertex_id = int(original_vertex_id)
            uv_id = int(uv_id)

            if uv_id < 0 or uv_id >= len(u_values):
                raise RuntimeError(
                    u"检测到无效的 UV ID：{}".format(uv_id)
                )

            key = (original_vertex_id, uv_id)
            new_vertex_id = vertex_uv_to_new_id.get(key)

            if new_vertex_id is None:
                new_vertex_id = len(point_data)
                vertex_uv_to_new_id[key] = new_vertex_id

                u_value = float(u_values[uv_id])
                v_value = float(v_values[uv_id])

                point_data.append(
                    (
                        u_value * self._scale,
                        v_value * self._scale,
                        0.0
                    )
                )

                # 新顶点 ID 与新 UV ID 保持一致。
                new_u_values.append(u_value)
                new_v_values.append(v_value)

            polygon_connects.append(new_vertex_id)

        if not point_data:
            raise RuntimeError(
                u"没有生成可用的新顶点。"
            )

        if self._requested_output_name is None:
            source_short_name = source_transform.rsplit(
                "|",
                1
            )[-1]

            source_base_name = source_short_name.rsplit(
                ":",
                1
            )[-1]

            self._requested_output_name = (
                source_base_name + "_UVFlat"
            )

        self._source_transform = source_transform
        self._source_uv_set = uv_set

        self._point_data = point_data
        self._polygon_counts = [
            int(value) for value in polygon_counts
        ]
        self._polygon_connects = polygon_connects
        self._u_values = new_u_values
        self._v_values = new_v_values

    def _assign_initial_material(self, mesh_shape_object):
        """把新 Mesh Shape 加入 initialShadingGroup。"""

        shading_group_selection = om.MSelectionList()
        shading_group_selection.add("initialShadingGroup")

        shading_group_object = (
            shading_group_selection.getDependNode(0)
        )

        shading_group_fn = om.MFnSet(shading_group_object)

        # Maya 2020 的 API 2.0 中，MFnSet.isMember() 不能直接接收
        # 单独的 MDagPath。这里直接传入 Mesh Shape 的 MObject，
        # 同时用于成员检查与材质集合分配。
        if not shading_group_fn.isMember(mesh_shape_object):
            shading_group_fn.addMember(mesh_shape_object)

    def _delete_output_immediately(self):
        """直接删除输出节点，不依赖 Maya 命令撤销队列。"""

        output_handle = self._output_handle

        if output_handle is None:
            self._output_transform_path = None
            return

        try:
            if (
                output_handle.isValid()
                and output_handle.isAlive()
            ):
                output_object = output_handle.object()

                if not output_object.isNull():
                    modifier = om.MDagModifier()
                    modifier.deleteNode(output_object)
                    modifier.doIt()

        finally:
            self._output_handle = None
            self._output_transform_path = None

    def _restore_previous_selection(self):
        """恢复命令执行前的选择状态。"""

        try:
            if self._previous_selection is None:
                empty_selection = om.MSelectionList()

                om.MGlobal.setActiveSelectionList(
                    empty_selection,
                    om.MGlobal.kReplaceList
                )
            else:
                om.MGlobal.setActiveSelectionList(
                    self._previous_selection,
                    om.MGlobal.kReplaceList
                )

        except Exception:
            empty_selection = om.MSelectionList()

            om.MGlobal.setActiveSelectionList(
                empty_selection,
                om.MGlobal.kReplaceList
            )


def flatten_selected_mesh_to_uv(
    scale=1.0,
    output_name=None,
    uv_set=None
):
    """
    Python 包装函数。

    参数：
        scale:
            一个 UV Tile 对应的 Maya 世界单位。

        output_name:
            输出 Transform 名称。

        uv_set:
            指定源 UV Set；不填写时使用当前 UV Set。

    返回：
        新模型 Transform 的完整 DAG 路径。
    """

    command_arguments = {
        "scale": float(scale)
    }

    if output_name is not None:
        command_arguments["name"] = output_name

    if uv_set is not None:
        command_arguments["uvSet"] = uv_set

    return cmds.flattenMeshToUV(**command_arguments)


def initializePlugin(plugin_object):
    plugin_fn = om.MFnPlugin(
        plugin_object,
        PLUGIN_VENDOR,
        PLUGIN_VERSION,
        "Any"
    )

    try:
        plugin_fn.registerCommand(
            COMMAND_NAME,
            FlattenMeshToUVCommand.creator,
            FlattenMeshToUVCommand.create_syntax
        )
    except Exception:
        om.MGlobal.displayError(
            u"注册命令失败：{}".format(COMMAND_NAME)
        )
        raise


def uninitializePlugin(plugin_object):
    plugin_fn = om.MFnPlugin(plugin_object)

    try:
        plugin_fn.deregisterCommand(COMMAND_NAME)
    except Exception:
        om.MGlobal.displayError(
            u"注销命令失败：{}".format(COMMAND_NAME)
        )
        raise
