"""MtoULiveLink Maya entry point."""

import errno
import json
import math
import re
import select
import socket
import struct
import threading
import time

__version__ = "0.2.0"
PROTOCOL_VERSION = 2
MAX_PAYLOAD_SIZE = (2 ** 31) - 9
HOST = "127.0.0.1"
PORT = 54321
SUBJECT_NAME = "MtoU_Character"
CURVE_VALUE_TOLERANCE = 1.0e-6
MIN_FRAME_RATE = 1.0
MAX_FRAME_RATE = 60.0
TIME_UNIT_FPS = {
    "game": 15.0,
    "film": 24.0,
    "pal": 25.0,
    "ntsc": 30.0,
    "show": 48.0,
    "palf": 50.0,
    "ntscf": 60.0,
    "29.97df": 29.97,
}

DIAGNOSTICS = {
    "PROTOCOL_VERSION_MISMATCH": (
        "Maya 与 UE 插件版本不匹配",
        "请安装同一版本的 Maya 和 UE MtoU_LiveLink 组件。"),
    "UNREAL_NOT_REACHABLE": (
        "无法连接 Unreal Editor",
        "请确认 UE 已启动、MtoU_LiveLink 已启用，并检查 54321 端口。"),
    "SECOND_CLIENT_REJECTED": (
        "Unreal 已连接另一个 Maya",
        "请先断开另一个 Maya 会话，再重新连接。"),
    "NO_BINDING_ACTOR": (
        "UE 关卡中没有 MtoU Binding Actor",
        "请在 UE 中将当前服装的 Binding 拖入关卡后重试。"),
    "MULTIPLE_BINDING_ACTORS": (
        "UE 关卡中有多个 MtoU Binding Actor",
        "请删除旧 Actor，仅保留当前服装的一个 Binding Actor。"),
    "INVALID_BINDING": (
        "UE Binding 资产无效",
        "请确认 Binding 已指定有效的 Skeletal Mesh。"),
    "SKELETON_MISMATCH": (
        "骨架与 Unreal Skeletal Mesh 不匹配",
        "请在 UE 中改用与当前 Maya 角色完全对应的 Skeletal Mesh。"),
    "BLENDSHAPE_MISMATCH": (
        "BlendShape 不完全匹配，连接可用但表情可能不完整",
        "请查看详情中 Maya 和 UE 各自缺少的名称，并确认 UE 服装是否正确。"),
    "NO_VISIBLE_SKINNED_MESH": (
        "没有找到可见的蒙皮网格",
        "请确认 Display 控制器已选择正确服装，并检查角色、头发和服装的显示状态。"),
    "INVALID_FRAME_RATE": (
        "Maya 场景帧率不受支持",
        "请将 Maya Time Unit 设置为 1–60 fps 范围内的动画帧率。"),
    "INVALID_MESSAGE": (
        "Maya 与 UE 的传输数据无效",
        "请确认两端插件版本一致；若仍失败，请复制诊断详情。"),
    "STREAM_INTERRUPTED": (
        "Live Link 连接已中断",
        "请确认 UE 仍在运行且 Binding Actor 有效，然后重新连接。"),
    "SAMPLING_FAILED": (
        "Maya 动画采样失败",
        "请检查骨架、BlendShape 和当前服装是否在连接期间被修改。"),
    "INTERNAL_ERROR": (
        "MtoU_LiveLink 发生内部错误",
        "请复制诊断详情并重新启动连接。"),
    "ROLE_SETUP_FAILED": (
        "角色设置未完成",
        "请按弹窗中的提示检查根骨骼和 Display 控制器。"),
    "DUPLICATE_BONE_NAMES": (
        "骨架中存在重名骨骼",
        "可以继续连接并由 UE 尝试映射；也可点击“选中重名骨骼”定位整理。"),
    "BONE_NAME_REMAP": (
        "UE 已自动映射重名骨骼",
        "连接可用；建议后续统一 Maya 与 UE 的骨骼名称，并保留本次映射详情。"),
}

try:
    import maya.api.OpenMaya as om
    import maya.cmds as cmds
except ImportError:
    om = None
    cmds = None


def normalize_name(path):
    return path.rsplit("|", 1)[-1].rsplit(":", 1)[-1]


class DuplicateBoneNamesError(ValueError):
    def __init__(self, duplicate_paths):
        self.duplicate_paths = duplicate_paths
        self.paths = [path for name in sorted(duplicate_paths)
                      for path in duplicate_paths[name]]
        names = ", ".join(sorted(duplicate_paths))
        super(DuplicateBoneNamesError, self).__init__(
            "duplicate normalized bone names: {0}".format(names))


def duplicate_bone_paths(records):
    paths_by_name = {}
    for record in records:
        paths_by_name.setdefault(record["name"], []).append(record["path"])
    return {
        name: paths for name, paths in paths_by_name.items() if len(paths) > 1
    }


def build_hierarchy(root, children, allow_duplicates=False):
    records = []
    stack = [(root, -1)]
    while stack:
        path, parent = stack.pop()
        index = len(records)
        records.append({"path": path, "name": normalize_name(path), "parent": parent})
        child_paths = list(children(path) or [])
        stack.extend((child, index) for child in reversed(child_paths))
    duplicates = duplicate_bone_paths(records)
    if duplicates and not allow_duplicates:
        raise DuplicateBoneNamesError(duplicates)
    return records


def duplicate_bone_diagnostic(error):
    lines = ["Duplicate normalized bone names:"]
    for name in sorted(error.duplicate_paths):
        lines.append("{0}:".format(name))
        lines.extend("  " + path for path in error.duplicate_paths[name])
    diagnostic = make_diagnostic(
        "DUPLICATE_BONE_NAMES", str(error), details="\n".join(lines))
    diagnostic["duplicate_paths"] = list(error.paths)
    return diagnostic


def centimeters_per_unit(unit):
    factors = {"mm": 0.1, "cm": 1.0, "m": 100.0, "km": 100000.0,
               "in": 2.54, "ft": 30.48, "yd": 91.44, "mi": 160934.4}
    if unit not in factors:
        raise ValueError("unsupported Maya linear unit: {0}".format(unit))
    return factors[unit]


def frames_per_second(unit):
    if unit in TIME_UNIT_FPS:
        return TIME_UNIT_FPS[unit]
    match = re.match(r"^([0-9]+(?:\.[0-9]+)?)fps$", str(unit))
    if match:
        return float(match.group(1))
    raise ValueError("unsupported Maya time unit: {0}".format(unit))


def validate_frame_rate(fps):
    fps = float(fps)
    if not math.isfinite(fps) or fps < MIN_FRAME_RATE or fps > MAX_FRAME_RATE:
        raise ValueError("Maya frame rate must be between 1 and 60 fps")
    return fps


def format_fps(fps):
    return "{0:g} fps".format(float(fps))


def make_diagnostic(code, message="", solution="", details=""):
    summary, default_solution = DIAGNOSTICS.get(code, DIAGNOSTICS["INTERNAL_ERROR"])
    return {
        "code": code,
        "summary": summary,
        "solution": solution or default_solution,
        "message": message or summary,
        "details": details or message,
    }


def blendshape_warning_from_reply(reply):
    missing_in_unreal = list(reply.get("missing_in_unreal")
                             or reply.get("missing_curves") or [])
    missing_in_maya = list(reply.get("missing_in_maya") or [])
    bone_name_remaps = list(reply.get("bone_name_remaps") or [])
    return {
        "missing_in_unreal": missing_in_unreal,
        "missing_in_maya": missing_in_maya,
        "bone_name_remaps": bone_name_remaps,
        "has_warning": bool(missing_in_unreal or missing_in_maya or bone_name_remaps),
    }


def _finite(values):
    return all(math.isfinite(float(value)) for value in values)


def convert_transform(translation, quaternion, scale, unit_scale):
    qx, qy, qz, qw = (float(value) for value in quaternion)
    length = math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw)
    if not length or not _finite(translation) or not _finite(quaternion) or not _finite(scale):
        raise ValueError("transform values must be finite and quaternion length must be non-zero")
    qx, qy, qz, qw = qx / length, qy / length, qz / length, qw / length
    tx, ty, tz = (float(value) * unit_scale for value in translation)
    sx, sy, sz = (float(value) for value in scale)
    return [tx, tz, ty, -qx, -qz, -qy, qw, sx, sz, sy]


def make_init_message(bones, curves):
    return {"type": "init", "version": PROTOCOL_VERSION, "bones": bones, "curves": curves}


def make_frame_message(transforms, curves):
    flat = [value for transform in transforms for value in transform] + list(curves)
    if not _finite(flat):
        raise ValueError("frame values must be finite")
    return {"type": "frame", "transforms": transforms, "curves": curves}


def encode_message(message):
    payload = json.dumps(message, ensure_ascii=False, allow_nan=False,
                         separators=(",", ":")).encode("utf-8")
    return struct.pack(">Q", len(payload)) + payload


def _recv_exact(sock, size):
    chunks = []
    remaining = size
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            raise EOFError("connection closed while receiving a protocol message")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def recv_message(sock):
    size = struct.unpack(">Q", _recv_exact(sock, 8))[0]
    if size > MAX_PAYLOAD_SIZE:
        raise ValueError("protocol message length exceeds the supported container range")
    return json.loads(_recv_exact(sock, size).decode("utf-8"))


def validate_reply(reply):
    if not isinstance(reply, dict):
        raise ValueError("protocol reply must be a JSON object")
    reply_type = reply.get("type")
    required = {
        "ready": {
            "missing_in_unreal": list,
            "missing_in_maya": list,
            "bone_name_remaps": list,
        },
        "error": {"code": str, "message": str, "details": str},
    }
    fields = required.get(reply_type)
    if fields is None:
        raise ValueError("protocol reply type must be 'ready' or 'error'")
    for name, expected_type in fields.items():
        if name not in reply:
            raise ValueError("protocol reply requires field '{0}'".format(name))
        if not isinstance(reply[name], expected_type):
            raise ValueError("protocol reply field '{0}' has the wrong JSON type".format(name))
        if expected_type is list and any(not isinstance(value, str) for value in reply[name]):
            raise ValueError("protocol reply field '{0}' must contain strings".format(name))
    return reply


def _require_maya():
    if cmds is None or om is None:
        raise RuntimeError("MtoU_LiveLink must be run inside Maya 2022.4")


def _dag_path(path):
    selection = om.MSelectionList()
    selection.add(path)
    return selection.getDagPath(0)


def _maya_children(path):
    return cmds.listRelatives(path, children=True, fullPath=True, type="joint") or []


def _selected_root():
    selected = cmds.ls(selection=True, long=True) or []
    if len(selected) != 1 or cmds.nodeType(selected[0]) != "joint":
        raise ValueError("Select exactly one deformation root joint before connecting.")
    return selected[0]


def _long_transform(node):
    matches = cmds.ls(node, long=True) or []
    if len(matches) != 1:
        raise ValueError("Expected exactly one Maya node: {0}".format(node))
    path = matches[0]
    if cmds.nodeType(path) in ("mesh", "nurbsCurve"):
        parents = cmds.listRelatives(path, parent=True, fullPath=True) or []
        if not parents:
            raise ValueError("The selected shape has no transform parent: {0}".format(path))
        path = parents[0]
    if cmds.nodeType(path) != "transform":
        raise ValueError("The Display controller must be a transform or curve shape.")
    return path


def _namespace(path):
    leaf = path.rsplit("|", 1)[-1]
    return leaf.rsplit(":", 1)[0] if ":" in leaf else ""


def _top_level(path):
    parts = [part for part in path.split("|") if part]
    return "|" + parts[0] if parts else path


def _enum_attributes(node):
    candidates = []
    for attribute in cmds.listAttr(node, userDefined=True) or []:
        plug = node + "." + attribute
        if cmds.getAttr(plug, type=True) != "enum":
            continue
        definitions = cmds.attributeQuery(attribute, node=node, listEnum=True) or []
        entries = definitions[0].split(":") if definitions else []
        if any(re.match(r"(?i)^clothes", entry or "") for entry in entries):
            candidates.append({"attribute": attribute, "entries": entries, "plug": plug})
    return candidates


def _display_candidates(root):
    top = _top_level(root)
    descendants = cmds.listRelatives(top, allDescendents=True, fullPath=True,
                                     type="transform") or []
    expected_namespace = _namespace(root)
    candidates = []
    for node in descendants:
        leaf = node.rsplit("|", 1)[-1]
        if normalize_name(leaf).lower() != "display_ctrl":
            continue
        if _namespace(node) == expected_namespace:
            candidates.append(node)
    return sorted(candidates)


def _current_enum_label(display):
    value = int(cmds.getAttr(display["plug"]))
    entries = display["entries"]
    return entries[value] if 0 <= value < len(entries) else str(value)


def _effective_mesh_transform(geometry):
    paths = cmds.ls(geometry, long=True) or []
    if not paths:
        return None
    path = paths[0]
    if cmds.nodeType(path) == "mesh":
        shape = path
        parents = cmds.listRelatives(shape, parent=True, fullPath=True) or []
        if not parents:
            return None
        transform = parents[0]
    else:
        transform = path
        shapes = cmds.listRelatives(transform, shapes=True, noIntermediate=True,
                                    fullPath=True, type="mesh") or []
        if not shapes:
            return None
        shape = shapes[0]
    if cmds.getAttr(shape + ".intermediateObject"):
        return None
    if not (cmds.ls(shape, visible=True, long=True) or []):
        return None
    if not cmds.getAttr(shape + ".visibility"):
        return None
    current = transform
    while current:
        if cmds.attributeQuery("visibility", node=current, exists=True) \
                and not cmds.getAttr(current + ".visibility"):
            return None
        if cmds.attributeQuery("lodVisibility", node=current, exists=True) \
                and not cmds.getAttr(current + ".lodVisibility"):
            return None
        if cmds.attributeQuery("overrideEnabled", node=current, exists=True) \
                and cmds.getAttr(current + ".overrideEnabled"):
            if cmds.attributeQuery("overrideVisibility", node=current, exists=True) \
                    and not cmds.getAttr(current + ".overrideVisibility"):
                return None
            if cmds.attributeQuery("overrideDisplayType", node=current, exists=True) \
                    and cmds.getAttr(current + ".overrideDisplayType") != 0:
                return None
        parents = cmds.listRelatives(current, parent=True, fullPath=True) or []
        current = parents[0] if parents else None
    return transform


def _visible_skinned_meshes(bone_paths):
    skin_clusters = set()
    for path in bone_paths:
        skin_clusters.update(cmds.listConnections(path, type="skinCluster") or [])
    meshes = []
    seen = set()
    for skin_cluster in sorted(skin_clusters):
        for geometry in cmds.skinCluster(skin_cluster, query=True, geometry=True) or []:
            transform = _effective_mesh_transform(geometry)
            if transform and transform not in seen:
                seen.add(transform)
                meshes.append(transform)
    return sorted(meshes)


def _discover_curve_plugs(bone_paths, visible_meshes=None):
    if visible_meshes is None:
        visible_meshes = _visible_skinned_meshes(bone_paths)
    deformers = set()
    for geometry in visible_meshes:
        deformers.update(node for node in cmds.listHistory(geometry, pruneDagObjects=True) or []
                         if cmds.nodeType(node) == "blendShape")
    curves = []
    for deformer in sorted(deformers):
        aliases = cmds.aliasAttr(deformer, query=True) or []
        for alias, attribute in zip(aliases[0::2], aliases[1::2]):
            match = re.match(r"(?:weight|w)\[(\d+)\]$", attribute)
            if match:
                curves.append({"name": alias, "plug": deformer + "." + attribute,
                               "sort": (deformer, int(match.group(1)))})
    curves.sort(key=lambda curve: curve["sort"])
    grouped = []
    by_name = {}
    for curve in curves:
        group = by_name.get(curve["name"])
        if group is None:
            group = {"name": curve["name"], "plugs": [], "sort": curve["sort"]}
            by_name[curve["name"]] = group
            grouped.append(group)
        group["plugs"].append(curve["plug"])
    return grouped


def _capture_subject(root=None):
    _require_maya()
    root = root or _selected_root()
    bones = build_hierarchy(root, _maya_children, allow_duplicates=True)
    for bone in bones:
        bone["dag_path"] = _dag_path(bone["path"])
    mesh_paths = _visible_skinned_meshes([bone["path"] for bone in bones])
    if not mesh_paths:
        raise ValueError("NO_VISIBLE_SKINNED_MESH")
    return {"root": root, "bones": bones,
            "meshes": mesh_paths,
            "curves": _discover_curve_plugs(
                [bone["path"] for bone in bones], mesh_paths),
            "unit_scale": centimeters_per_unit(cmds.currentUnit(query=True, linear=True))}


def _sample_matrix(matrix, unit_scale):
    transform = om.MTransformationMatrix(matrix)
    translation = transform.translation(om.MSpace.kTransform)
    rotation = transform.rotation(asQuaternion=True)
    scale = transform.scale(om.MSpace.kTransform)
    return convert_transform(translation, rotation, scale, unit_scale)


def _sample_pose(subject):
    world_matrices = [bone["dag_path"].inclusiveMatrix() for bone in subject["bones"]]
    transforms = []
    for index, bone in enumerate(subject["bones"]):
        matrix = world_matrices[index]
        if bone["parent"] >= 0:
            matrix = matrix * world_matrices[bone["parent"]].inverse()
        transforms.append(_sample_matrix(matrix, subject["unit_scale"]))
    curves = []
    for curve in subject["curves"]:
        values = [float(cmds.getAttr(plug)) for plug in curve["plugs"]]
        reference = values[0]
        if any(abs(value - reference) > CURVE_VALUE_TOLERANCE
               for value in values[1:]):
            details = ", ".join("{0}={1}".format(plug, value)
                                for plug, value in zip(curve["plugs"], values))
            raise ValueError(
                "conflicting values for BlendShape alias {0}: {1}".format(
                    curve["name"], details))
        curves.append(reference)
    make_frame_message(transforms, curves)
    return transforms, curves


class _CharacterSceneError(RuntimeError):
    def __init__(self, code, message="", details="", context=None,
                 duplicate_paths=None):
        self._code = code
        self._message = message or code
        self._details = details or self._message
        self._context = dict(context or {})
        self._duplicate_paths = tuple(duplicate_paths or ())
        super(_CharacterSceneError, self).__init__(self._message)

    @property
    def code(self):
        return self._code

    @property
    def message(self):
        return self._message

    @property
    def details(self):
        return self._details

    @property
    def context(self):
        return dict(self._context)

    @property
    def duplicate_paths(self):
        return self._duplicate_paths


class _CharacterSnapshot(object):
    def __init__(self, revision, root, outfit, bones, curve_names,
                 duplicate_paths=(), mesh_paths=()):
        self._revision = int(revision)
        self._root = root
        self._outfit = outfit
        self._bones = tuple((name, int(parent)) for name, parent in bones)
        self._curve_names = tuple(curve_names)
        self._duplicate_paths = tuple(duplicate_paths)
        self._mesh_paths = tuple(mesh_paths)

    @property
    def revision(self):
        return self._revision

    @property
    def root(self):
        return self._root

    @property
    def outfit(self):
        return self._outfit

    @property
    def bones(self):
        return self._bones

    @property
    def curve_names(self):
        return self._curve_names

    @property
    def duplicate_paths(self):
        return self._duplicate_paths

    @property
    def mesh_paths(self):
        return self._mesh_paths


class _CharacterFrame(object):
    def __init__(self, revision, transforms, curves):
        self._revision = int(revision)
        self._transforms = tuple(tuple(value for value in transform)
                                 for transform in transforms)
        self._curves = tuple(curves)

    @property
    def revision(self):
        return self._revision

    @property
    def transforms(self):
        return self._transforms

    @property
    def curves(self):
        return self._curves


class _CharacterSceneEvent(object):
    def __init__(self, kind, snapshot=None, error=None):
        self._kind = kind
        self._snapshot = snapshot
        self._error = error

    @property
    def kind(self):
        return self._kind

    @property
    def snapshot(self):
        return self._snapshot

    @property
    def error(self):
        return self._error


class _CharacterScene(object):
    """Owns one configured, sampleable Maya character scene."""

    def __init__(self, root, display, display_attribute, on_event):
        self._root = root
        self._display_path = display
        self._display_attribute = display_attribute
        self._on_event = on_event
        self._subject = None
        self._snapshot = None
        self._callback_ids = []
        self._refresh_generation = 0
        self._refreshing = False
        self._closed = False
        self._terminal_error = None
        self._cleanup_error = None

    @classmethod
    def capture(cls, root, display=None, display_attribute=None, on_event=None):
        _require_maya()
        scene = None
        try:
            root = cls._resolve_root(root)
            display, display_attribute = cls._resolve_display(
                root, display, display_attribute)
            scene = cls(root, display, display_attribute, on_event)
            scene._commit_capture(revision=1)
            scene._register_callbacks()
            return scene
        except _CharacterSceneError:
            if scene is not None:
                scene.close()
            raise
        except (RuntimeError, ValueError) as exc:
            if scene is not None:
                scene.close()
            raise cls._wrap_error("ROLE_SETUP_FAILED", exc)

    @staticmethod
    def _resolve_root(root):
        matches = cmds.ls(root, long=True) or []
        if len(matches) != 1 or cmds.nodeType(matches[0]) != "joint":
            raise _CharacterSceneError(
                "INVALID_CHARACTER_ROOT",
                "Select exactly one deformation root joint before connecting.")
        return matches[0]

    @staticmethod
    def _resolve_display(root, display, display_attribute):
        if display is None:
            candidates = _display_candidates(root)
            if len(candidates) != 1:
                raise _CharacterSceneError(
                    "AMBIGUOUS_DISPLAY",
                    "Unable to identify exactly one Display controller.",
                    context={"display_candidates": tuple(candidates)})
            display = candidates[0]
        else:
            try:
                display = _long_transform(display)
            except (RuntimeError, ValueError) as exc:
                raise _CharacterScene._wrap_error("INVALID_DISPLAY", exc)
        role_top = _top_level(root)
        if display != role_top and not display.startswith(role_top + "|"):
            raise _CharacterSceneError(
                "INVALID_DISPLAY",
                "The Display controller must be inside the character's top-level group.")
        if _namespace(display) != _namespace(root):
            raise _CharacterSceneError(
                "INVALID_DISPLAY",
                "The Display controller and character root must use the same namespace.")
        attributes = _enum_attributes(display)
        by_name = dict((item["attribute"], item) for item in attributes)
        if display_attribute is None:
            if len(attributes) != 1:
                raise _CharacterSceneError(
                    "AMBIGUOUS_DISPLAY_ATTRIBUTE",
                    "Unable to identify exactly one Clothes enum attribute.",
                    context={"attribute_candidates": tuple(sorted(by_name))})
            display_attribute = attributes[0]["attribute"]
        if display_attribute not in by_name:
            raise _CharacterSceneError(
                "INVALID_DISPLAY_ATTRIBUTE",
                "The selected Display attribute is not a Clothes enum.",
                context={"attribute_candidates": tuple(sorted(by_name))})
        return display, display_attribute

    @staticmethod
    def _wrap_error(code, error):
        if isinstance(error, _CharacterSceneError):
            return error
        return _CharacterSceneError(code, str(error), details=str(error))

    def _display_record(self):
        for record in _enum_attributes(self._display_path):
            if record["attribute"] == self._display_attribute:
                return record
        raise _CharacterSceneError(
            "INVALID_DISPLAY_ATTRIBUTE",
            "The configured Clothes enum attribute is no longer available.")

    def _capture_values(self, revision):
        try:
            subject = _capture_subject(self._root)
            display = self._display_record()
            outfit = _current_enum_label(display)
        except (RuntimeError, ValueError) as exc:
            code = str(exc) if str(exc) in DIAGNOSTICS else "ROLE_SETUP_FAILED"
            raise self._wrap_error(code, exc)
        duplicates = duplicate_bone_paths(subject["bones"])
        duplicate_paths = [path for name in sorted(duplicates)
                           for path in duplicates[name]]
        snapshot = _CharacterSnapshot(
            revision,
            self._root,
            outfit,
            [(bone["name"], bone["parent"]) for bone in subject["bones"]],
            [curve["name"] for curve in subject["curves"]],
            duplicate_paths,
            subject["meshes"])
        return subject, snapshot

    def _commit_capture(self, revision):
        subject, snapshot = self._capture_values(revision)
        self._subject = subject
        self._snapshot = snapshot

    def _register_callbacks(self):
        try:
            self._callback_ids.append(om.MNodeMessage.addNodeDestroyedCallback(
                self._subject["bones"][0]["dag_path"].node(),
                self._on_root_destroyed))
            self._callback_ids.append(om.MNodeMessage.addNodeDestroyedCallback(
                _dag_path(self._display_path).node(), self._on_display_destroyed))
            self._callback_ids.append(om.MNodeMessage.addAttributeChangedCallback(
                _dag_path(self._display_path).node(), self._on_display_changed))
        except (RuntimeError, ValueError) as exc:
            self._remove_callbacks()
            raise self._wrap_error("ROLE_SETUP_FAILED", exc)

    def snapshot(self):
        self._ensure_available()
        return self._snapshot

    def sample(self):
        self._ensure_available()
        revision = self._snapshot.revision
        subject = self._subject
        try:
            transforms, curves = _sample_pose(subject)
        except (RuntimeError, ValueError) as exc:
            raise self._wrap_error("SAMPLING_FAILED", exc)
        return _CharacterFrame(revision, transforms, curves)

    def close(self):
        if self._closed:
            return
        self._closed = True
        self._refresh_generation += 1
        self._refreshing = False
        self._remove_callbacks()
        self._subject = None
        if self._terminal_error is None:
            self._terminal_error = _CharacterSceneError(
                "CHARACTER_SCENE_CLOSED", "The character scene is closed.")

    def _ensure_available(self):
        if self._terminal_error is not None:
            raise self._terminal_error
        if self._closed:
            raise _CharacterSceneError(
                "CHARACTER_SCENE_CLOSED", "The character scene is closed.")
        if self._refreshing:
            raise _CharacterSceneError(
                "CHARACTER_SCENE_REFRESHING", "The character scene is refreshing.")

    def _remove_callbacks(self):
        callback_ids, self._callback_ids = self._callback_ids, []
        for callback_id in callback_ids:
            try:
                om.MMessage.removeCallback(callback_id)
            except RuntimeError as exc:
                if self._cleanup_error is None:
                    self._cleanup_error = exc

    def _emit(self, event):
        if self._on_event is None:
            return
        try:
            self._on_event(event)
        except Exception as exc:
            if cmds is not None:
                try:
                    cmds.warning(
                        "MtoU_LiveLink character event callback failed: {0}".format(exc))
                except Exception:
                    pass

    def _on_display_changed(self, message, plug, other_plug, client_data):
        del other_plug, client_data
        if self._closed or self._terminal_error is not None:
            return
        if not (message & om.MNodeMessage.kAttributeSet):
            return
        if plug.partialName(useLongNames=True) != self._display_attribute:
            return
        try:
            if _current_enum_label(self._display_record()) == self._snapshot.outfit:
                return
        except _CharacterSceneError as error:
            self._invalidate(error)
            return
        if not self._refreshing:
            self._refreshing = True
            self._emit(_CharacterSceneEvent(
                "character_change_started", snapshot=self._snapshot))
        self._refresh_generation += 1
        generation = self._refresh_generation
        cmds.evalDeferred(lambda: self._refresh_after_display_change(generation))

    def _refresh_after_display_change(self, generation):
        if self._closed or generation != self._refresh_generation:
            return
        try:
            subject, candidate = self._capture_values(self._snapshot.revision + 1)
        except _CharacterSceneError as error:
            self._invalidate(error)
            return
        previous = self._snapshot
        changed = (
            candidate.outfit != previous.outfit
            or candidate.bones != previous.bones
            or candidate.curve_names != previous.curve_names
            or candidate.duplicate_paths != previous.duplicate_paths)
        if not changed:
            candidate = _CharacterSnapshot(
                previous.revision, candidate.root, candidate.outfit,
                candidate.bones, candidate.curve_names, candidate.duplicate_paths,
                candidate.mesh_paths)
        self._subject = subject
        self._snapshot = candidate
        self._refreshing = False
        self._emit(_CharacterSceneEvent("outfit_changed", snapshot=candidate))

    def _on_root_destroyed(self, *unused):
        self._invalidate(_CharacterSceneError(
            "CHARACTER_ROOT_DESTROYED", "The character root was deleted or unloaded."))

    def _on_display_destroyed(self, *unused):
        self._invalidate(_CharacterSceneError(
            "DISPLAY_DESTROYED", "The Display controller was deleted or unloaded."))

    def _invalidate(self, error):
        if self._closed or self._terminal_error is not None:
            return
        self._terminal_error = error
        self._closed = True
        self._refresh_generation += 1
        self._refreshing = False
        self._remove_callbacks()
        self._subject = None
        self._emit(_CharacterSceneEvent("character_invalidated", error=error))


class _LatestFrame(object):
    def __init__(self):
        self._lock = threading.Lock()
        self._value = None

    def put(self, value):
        with self._lock:
            self._value = value

    def take(self):
        with self._lock:
            value, self._value = self._value, None
            return value


class _SenderWorker(threading.Thread):
    def __init__(self, init_message):
        threading.Thread.__init__(self, name="MtoULiveLinkSender")
        self.daemon = True
        self._init_packet = encode_message(init_message)
        self._latest = _LatestFrame()
        self._stop_event = threading.Event()
        self._lock = threading.Lock()
        self._socket = None
        self._state = "connecting"
        self._detail = "Connecting to Unreal..."
        self._warning = {"missing_in_unreal": [], "missing_in_maya": [],
                         "bone_name_remaps": [],
                         "has_warning": False}
        self._diagnostic = None

    def submit(self, frame_message):
        self._latest.put(encode_message(frame_message))

    def status(self):
        with self._lock:
            return (self._state, self._detail, dict(self._warning),
                    dict(self._diagnostic) if self._diagnostic else None)

    def _set_status(self, state, detail, warning=None, diagnostic=None):
        with self._lock:
            self._state = state
            self._detail = detail
            self._warning = dict(warning or {
                "missing_in_unreal": [], "missing_in_maya": [],
                "bone_name_remaps": [], "has_warning": False})
            self._diagnostic = dict(diagnostic) if diagnostic else None

    def stop(self):
        with self._lock:
            self._stop_event.set()
            sock = self._socket
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                sock.close()
            except OSError:
                pass

    def _connect(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        with self._lock:
            self._socket = sock
        if self._stop_event.is_set():
            return None
        sock.setblocking(False)
        result = sock.connect_ex((HOST, PORT))
        pending = (errno.EINPROGRESS, errno.EWOULDBLOCK)
        if result not in (0,) + pending:
            raise OSError(result, "connection failed")
        deadline = time.time() + 2.0
        while result:
            if self._stop_event.is_set():
                return None
            timeout = deadline - time.time()
            if timeout <= 0.0:
                raise socket.timeout("timed out connecting to Unreal")
            try:
                _, writable, errors = select.select([], [sock], [sock], min(0.05, timeout))
            except OSError:
                if self._stop_event.is_set():
                    return None
                raise
            if writable or errors:
                result = sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
                if result:
                    raise OSError(result, "connection failed")
        if self._stop_event.is_set():
            return None
        return sock

    def _send_initial(self, sock):
        packet = memoryview(self._init_packet)
        deadline = time.time() + 5.0
        while packet:
            with self._lock:
                if self._stop_event.is_set():
                    return False
                timeout = deadline - time.time()
                if timeout <= 0.0:
                    raise socket.timeout("timed out sending init message")
                try:
                    sent = sock.send(packet)
                except OSError as exc:
                    if exc.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                        raise
                    sent = None
            if sent is None:
                select.select([], [sock], [], min(0.05, timeout))
            elif not sent:
                raise EOFError("connection closed while sending init message")
            else:
                packet = packet[sent:]
        return True

    def run(self):
        connected = False
        try:
            sock = self._connect()
            if sock is None or self._stop_event.is_set():
                return
            if not self._send_initial(sock):
                return
            sock.settimeout(5.0)
            reply = validate_reply(recv_message(sock))
            if reply.get("type") == "error":
                diagnostic = make_diagnostic(
                    reply.get("code") or "INTERNAL_ERROR",
                    reply.get("message") or "Unreal rejected the connection",
                    details=reply.get("details") or reply.get("message") or "")
                self._set_status("error", diagnostic["summary"], diagnostic=diagnostic)
                return
            if reply.get("type") != "ready":
                diagnostic = make_diagnostic(
                    "INVALID_MESSAGE",
                    "expected ready, received {0}".format(reply.get("type")))
                self._set_status("error", diagnostic["summary"], diagnostic=diagnostic)
                return
            connected = True
            warning = blendshape_warning_from_reply(reply)
            self._set_status("ready", "Connected", warning=warning)
            sock.settimeout(0.25)
            while not self._stop_event.is_set():
                packet = self._latest.take()
                if packet is not None:
                    sock.sendall(packet)
                readable, _, _ = select.select([sock], [], [], 0.01)
                if readable:
                    reply = validate_reply(recv_message(sock))
                    if reply.get("type") == "error":
                        diagnostic = make_diagnostic(
                            reply.get("code") or "STREAM_INTERRUPTED",
                            reply.get("message") or "Unreal closed the connection",
                            details=reply.get("details") or reply.get("message") or "")
                        self._set_status(
                            "error", diagnostic["summary"], diagnostic=diagnostic)
                        return
                    diagnostic = make_diagnostic(
                        "INVALID_MESSAGE",
                        "unexpected runtime reply: {0}".format(reply.get("type")))
                    self._set_status("error", diagnostic["summary"], diagnostic=diagnostic)
                    return
        except Exception as exc:
            if not self._stop_event.is_set():
                if isinstance(exc, (ValueError, UnicodeError, json.JSONDecodeError)):
                    code = "INVALID_MESSAGE"
                else:
                    code = "STREAM_INTERRUPTED" if connected else "UNREAL_NOT_REACHABLE"
                diagnostic = make_diagnostic(code, str(exc), details=str(exc))
                self._set_status("error", diagnostic["summary"], diagnostic=diagnostic)
        finally:
            with self._lock:
                sock, self._socket = self._socket, None
            if sock is not None:
                try:
                    sock.close()
                except OSError:
                    pass
            if self._stop_event.is_set():
                self._set_status("disconnected", "Disconnected")


class _StreamingSessionError(RuntimeError):
    def __init__(self, code, message="", details="", recapture_scene=False):
        self._code = code
        self._message = message or code
        self._details = details or self._message
        self._recapture_scene = bool(recapture_scene)
        super(_StreamingSessionError, self).__init__(self._message)

    @property
    def code(self):
        return self._code

    @property
    def message(self):
        return self._message

    @property
    def details(self):
        return self._details

    @property
    def recapture_scene(self):
        return self._recapture_scene


def _copy_session_payload(value):
    if isinstance(value, dict):
        return {key: _copy_session_payload(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return tuple(_copy_session_payload(item) for item in value)
    return value


class _StreamingSessionEvent(object):
    def __init__(self, kind, warning=None, diagnostic=None, recapture_scene=False):
        self._kind = kind
        self._warning = _copy_session_payload(warning) if warning else None
        self._diagnostic = _copy_session_payload(diagnostic) if diagnostic else None
        self._recapture_scene = bool(recapture_scene)

    @property
    def kind(self):
        return self._kind

    @property
    def warning(self):
        return _copy_session_payload(self._warning) if self._warning else None

    @property
    def diagnostic(self):
        return _copy_session_payload(self._diagnostic) if self._diagnostic else None

    @property
    def recapture_scene(self):
        return self._recapture_scene


class _StreamingSession(object):
    """Owns one Maya-to-Unreal streaming connection lifecycle."""

    def __init__(self, scene, fps, on_event):
        self._scene = scene
        self._fps = fps
        self._on_event = on_event
        self._worker = None
        self._worker_started = False
        self._callback_ids = []
        self._timer_id = None
        self._revision = None
        self._last_sample_time = None
        self._phase = "starting"
        self._outcome = None
        self._terminal_event = None
        self._cleanup_pending = False
        self._cleaned = False
        self._cleanup_error = None

    @classmethod
    def start(cls, scene, fps, on_event=None):
        try:
            fps = validate_frame_rate(fps)
            snapshot = scene.snapshot()
        except _CharacterSceneError as error:
            raise _StreamingSessionError(
                error.code, error.message, error.details, recapture_scene=True)
        except (RuntimeError, ValueError) as exc:
            raise _StreamingSessionError(
                "INVALID_FRAME_RATE", str(exc), details=str(exc))
        session = cls(scene, fps, on_event)
        try:
            session._start(snapshot)
        except Exception as exc:
            session._cleanup()
            if isinstance(exc, _StreamingSessionError):
                raise
            raise _StreamingSessionError(
                "INTERNAL_ERROR", str(exc), details=str(exc))
        return session

    def _start(self, snapshot):
        init_message = make_init_message(
            [[name, parent] for name, parent in snapshot.bones],
            list(snapshot.curve_names))
        self._worker = _SenderWorker(init_message)
        self._revision = snapshot.revision
        self._worker.start()
        self._worker_started = True
        for message in (
                om.MSceneMessage.kBeforeNew,
                om.MSceneMessage.kBeforeOpen,
                om.MSceneMessage.kMayaExiting):
            self._callback_ids.append(
                om.MSceneMessage.addCallback(message, self._on_scene_change))
        self._callback_ids.append(
            om.MEventMessage.addEventCallback(
                "timeChanged", self._on_time_changed))
        self._timer_id = om.MTimerMessage.addTimerCallback(
            1.0 / self._fps, self._on_timer)
        self._phase = "connecting"

    def change_rate(self, fps):
        if self._outcome is not None:
            return
        try:
            fps = validate_frame_rate(fps)
        except (RuntimeError, ValueError) as exc:
            self._request_failure(
                make_diagnostic("INVALID_FRAME_RATE", str(exc), details=str(exc)),
                recapture_scene=False)
            return
        old_timer = self._timer_id
        try:
            new_timer = om.MTimerMessage.addTimerCallback(
                1.0 / fps, self._on_timer)
        except (RuntimeError, ValueError) as exc:
            self._request_failure(
                make_diagnostic("INTERNAL_ERROR", str(exc), details=str(exc)),
                recapture_scene=False)
            return
        if old_timer is not None:
            try:
                om.MMessage.removeCallback(old_timer)
            except RuntimeError as exc:
                self._remove_callback(new_timer)
                self._request_failure(
                    make_diagnostic("INTERNAL_ERROR", str(exc), details=str(exc)),
                    recapture_scene=False)
                return
        self._timer_id = new_timer
        self._fps = fps

    def stop(self):
        if self._outcome is None:
            self._outcome = "stopped"
            self._terminal_event = _StreamingSessionEvent("stopped")
            self._phase = "terminal"
        elif self._cleaned:
            return
        self._finish_terminal()

    def _on_scene_change(self, *unused):
        self.stop()

    def _on_timer(self, elapsed, last_time, client_data):
        del elapsed, last_time, client_data
        self._sample_and_submit()

    def _on_time_changed(self, *unused):
        del unused
        self._sample_and_submit()

    def _sample_and_submit(self):
        if self._outcome is not None or self._worker is None:
            return
        state, detail, warning, diagnostic = self._worker.status()
        if state == "error":
            del detail, warning
            self._request_failure(
                diagnostic or make_diagnostic("INTERNAL_ERROR", "Streaming failed."),
                recapture_scene=False)
            return
        if state == "disconnected":
            self._request_failure(
                make_diagnostic(
                    "STREAM_INTERRUPTED", detail or "Live Link connection ended.",
                    details=detail or "The streaming worker disconnected unexpectedly."),
                recapture_scene=False)
            return
        if state != "ready":
            return
        if self._phase == "connecting":
            self._phase = "ready"
            self._emit(_StreamingSessionEvent("ready", warning=warning))
        now = time.time()
        sample_interval = 1.0 / self._fps
        if (self._last_sample_time is not None
                and now >= self._last_sample_time
                and now - self._last_sample_time < sample_interval):
            return
        self._last_sample_time = now
        try:
            frame = self._scene.sample()
        except _CharacterSceneError as error:
            self._request_failure(
                make_diagnostic(
                    error.code if error.code in DIAGNOSTICS else "SAMPLING_FAILED",
                    error.message, details=error.details),
                recapture_scene=True)
            return
        if frame.revision != self._revision:
            self._request_failure(
                make_diagnostic(
                    "SAMPLING_FAILED",
                    "The character description changed during this session.",
                    details="character scene revision changed from {0} to {1}".format(
                        self._revision, frame.revision)),
                recapture_scene=True)
            return
        self._worker.submit(make_frame_message(
            list(frame.transforms), list(frame.curves)))

    def _request_failure(self, diagnostic, recapture_scene):
        if self._outcome is not None:
            return
        self._outcome = "failed"
        self._phase = "terminal"
        self._terminal_event = _StreamingSessionEvent(
            "failed", diagnostic=diagnostic, recapture_scene=recapture_scene)
        if not self._cleanup_pending:
            self._cleanup_pending = True
            cmds.evalDeferred(self._finish_terminal)

    def _finish_terminal(self):
        if self._cleaned:
            return
        self._cleanup_pending = False
        self._cleanup()
        self._cleaned = True
        event, self._terminal_event = self._terminal_event, None
        if event is not None:
            self._emit(event)

    def _cleanup(self):
        if self._cleaned:
            return
        callback_ids, self._callback_ids = self._callback_ids, []
        timer_id, self._timer_id = self._timer_id, None
        for callback_id in callback_ids:
            self._remove_callback(callback_id)
        if timer_id is not None:
            self._remove_callback(timer_id)
        worker, self._worker = self._worker, None
        if worker is not None:
            try:
                worker.stop()
            except Exception as exc:
                self._record_cleanup_error(exc)
            if self._worker_started:
                try:
                    worker.join(1.0)
                except Exception as exc:
                    self._record_cleanup_error(exc)
        self._worker_started = False

    def _remove_callback(self, callback_id):
        try:
            om.MMessage.removeCallback(callback_id)
        except RuntimeError as exc:
            self._record_cleanup_error(exc)

    def _record_cleanup_error(self, error):
        if self._cleanup_error is None:
            self._cleanup_error = error

    def _emit(self, event):
        if self._on_event is None:
            return
        try:
            self._on_event(event)
        except Exception as exc:
            if cmds is not None:
                try:
                    cmds.warning(
                        "MtoU_LiveLink session event callback failed: {0}".format(exc))
                except Exception:
                    pass


class _Controller(object):
    def __init__(self):
        self._session = None
        self._scene = None
        self._pending_root = None
        self._capture_args = None
        self._script_jobs = []
        self._outfit_change_was_connected = False
        self._pending_stop_status = None
        self._last_diagnostic = make_diagnostic("INTERNAL_ERROR", "暂无诊断信息")
        self._last_warning = {"missing_in_unreal": [], "missing_in_maya": [],
                              "bone_name_remaps": [],
                              "has_warning": False}
        self._light = None
        self._root_text = None
        self._outfit_text = None
        self._fps_text = None
        self._bone_text = None
        self._curve_text = None
        self._status_text = None
        self._warning_checkbox = None
        self._duplicate_button = None

    def build_ui(self):
        cmds.window(WINDOW_NAME, title="MtoU Live Link", closeCommand=self.close,
                    sizeable=False, width=430, resizeToFitChildren=True)
        cmds.columnLayout(adjustableColumn=True, rowSpacing=7, columnAttach=("both", 10))
        cmds.text(label="MtoU Live Link", align="center", font="boldLabelFont", height=26)
        self._light = cmds.text(label="●  未连接", align="left", backgroundColor=(0.55, 0.08, 0.08),
                                height=28)
        self._root_text = cmds.text(label="角色根骨骼：—", align="left")
        self._outfit_text = cmds.text(label="当前衣服：—", align="left")
        self._fps_text = cmds.text(label="场景帧率：—", align="left")
        self._bone_text = cmds.text(label="骨骼数：0", align="left")
        self._curve_text = cmds.text(label="BlendShape 数：0", align="left")
        cmds.button(label="设置角色（请先选择根骨骼）", command=lambda *_: self.set_role())
        cmds.button(
            label="手动选择 Display 控制器",
            command=lambda *_: self.set_display_controller())
        self._duplicate_button = cmds.button(
            label="选中重名骨骼（0）", enable=False,
            command=lambda *_: self.select_duplicate_bones())
        cmds.rowLayout(numberOfColumns=2, adjustableColumn=1, columnWidth2=(200, 200))
        cmds.button(label="连接", command=lambda *_: self.connect())
        cmds.button(label="断开连接", command=lambda *_: self.disconnect())
        cmds.setParent("..")
        self._warning_checkbox = cmds.checkBox(
            label="连接成功后弹出差异警告", value=True)
        cmds.button(label="查看诊断详情", command=lambda *_: self.show_diagnostics())
        self._status_text = cmds.text(label="状态：未设置角色", align="left", wordWrap=True,
                                      height=38)
        self._refresh_fps()
        self._script_jobs.append(cmds.scriptJob(
            event=["timeUnitChanged", self._on_time_unit_changed], parent=WINDOW_NAME))
        self._script_jobs.append(cmds.scriptJob(
            event=["SceneOpened", self._on_scene_change], parent=WINDOW_NAME))
        self._script_jobs.append(cmds.scriptJob(
            event=["NewSceneOpened", self._on_scene_change], parent=WINDOW_NAME))
        cmds.showWindow(WINDOW_NAME)

    def _set_text(self, control, text):
        if control and cmds.control(control, exists=True):
            cmds.text(control, edit=True, label=text)

    def _set_connected(self, connected, status):
        color = (0.08, 0.45, 0.12) if connected else (0.55, 0.08, 0.08)
        label = "●  已连接" if connected else "●  未连接"
        if self._light and cmds.control(self._light, exists=True):
            cmds.text(self._light, edit=True, label=label, backgroundColor=color)
        self._set_text(self._status_text, "状态：" + status)

    def _refresh_fps(self):
        try:
            fps = frames_per_second(cmds.currentUnit(query=True, time=True))
            self._set_text(self._fps_text, "场景帧率：" + format_fps(fps))
            return fps
        except ValueError:
            unit = cmds.currentUnit(query=True, time=True)
            self._set_text(self._fps_text, "场景帧率：{0}（不受支持）".format(unit))
            return None

    def _selected_display(self):
        selected = cmds.ls(selection=True, long=True) or []
        if len(selected) != 1:
            raise ValueError("请只选择一个 Display 曲线控制器。")
        return _long_transform(selected[0])

    def _choose_enum(self, candidates):
        labels = list(candidates)
        if len(labels) == 1:
            return labels[0]
        if not labels:
            raise ValueError("所选控制器没有包含 Clothes 选项的 enum 属性。")
        result = cmds.confirmDialog(
            title="选择服装属性", message="找到多个候选属性，请选择：",
            button=labels + ["取消"], defaultButton=labels[0], cancelButton="取消",
            dismissString="取消")
        if result == "取消":
            raise ValueError("已取消 Display 属性选择。")
        return result

    def _duplicate_paths(self):
        if self._scene is None:
            return ()
        try:
            return self._scene.snapshot().duplicate_paths
        except _CharacterSceneError:
            return ()

    def _refresh_duplicate_button(self):
        paths = self._duplicate_paths()
        if self._duplicate_button and cmds.control(self._duplicate_button, exists=True):
            cmds.button(
                self._duplicate_button, edit=True,
                label="选中重名骨骼（{0}）".format(len(paths)),
                enable=bool(paths))

    def select_duplicate_bones(self):
        existing = [path for path in self._duplicate_paths() if cmds.objExists(path)]
        if not existing:
            self._refresh_duplicate_button()
            self._set_connected(False, "重名骨骼已不存在，请重新设置角色")
            return
        cmds.select(existing, replace=True)
        self._set_connected(False, "已选中 {0} 个重名骨骼".format(len(existing)))

    def _scene_diagnostic(self, error, default_code="ROLE_SETUP_FAILED"):
        code = error.code if error.code in DIAGNOSTICS else default_code
        diagnostic = make_diagnostic(
            code, error.message, solution=error.message, details=error.details)
        if error.duplicate_paths:
            diagnostic["duplicate_paths"] = list(error.duplicate_paths)
        return diagnostic

    def _capture_scene(self, root, display=None, display_attribute=None):
        try:
            scene = _CharacterScene.capture(
                root, display=display, display_attribute=display_attribute,
                on_event=self._on_character_scene_event)
        except _CharacterSceneError as error:
            if error.code == "AMBIGUOUS_DISPLAY_ATTRIBUTE" and display is not None:
                attribute = self._choose_enum(
                    error.context.get("attribute_candidates", ()))
                return self._capture_scene(root, display, attribute)
            raise
        self._scene = scene
        self._pending_root = None
        self._capture_args = (root, display, display_attribute)
        self._render_snapshot(scene.snapshot())
        return scene

    def _render_snapshot(self, snapshot):
        self._set_text(self._root_text, "角色根骨骼：" + snapshot.root)
        self._set_text(self._outfit_text, "当前衣服：" + snapshot.outfit)
        self._set_text(self._bone_text, "骨骼数：{0}".format(len(snapshot.bones)))
        self._set_text(self._curve_text, "BlendShape 数：{0}".format(
            len(snapshot.curve_names)))
        self._refresh_duplicate_button()
        self._refresh_fps()

    def set_role(self):
        _require_maya()
        self._clear_role()
        try:
            root = _selected_root()
            self._pending_root = root
            snapshot = self._capture_scene(root).snapshot()
            status = "角色已设置，可以连接"
            if snapshot.duplicate_paths:
                status += "（检测到 {0} 个重名骨骼，将由 UE 尝试映射）".format(
                    len(snapshot.duplicate_paths))
            self._set_connected(False, status)
        except _CharacterSceneError as error:
            if error.code == "AMBIGUOUS_DISPLAY":
                self._pending_root = root
                self._set_text(self._root_text, "角色根骨骼：" + root)
                self._set_connected(False, error.message)
            else:
                self._pending_root = None
                self._set_connected(False, error.message)
            self._show_error(self._scene_diagnostic(error))
        except (RuntimeError, ValueError) as exc:
            self._set_connected(False, str(exc))
            code = str(exc) if str(exc) in DIAGNOSTICS else "ROLE_SETUP_FAILED"
            self._show_error(make_diagnostic(code, str(exc), solution=str(exc), details=str(exc)))

    def set_display_controller(self):
        root = self._pending_root
        if root is None and self._scene is not None:
            root = self._scene.snapshot().root
        if not root:
            self._show_error(make_diagnostic(
                "INTERNAL_ERROR", "请先选择根骨骼并设置角色。"))
            return
        try:
            self._clear_scene(keep_pending=True)
            self._pending_root = root
            self._capture_scene(root, display=self._selected_display())
            self._set_connected(False, "Display 控制器已设置，可以连接")
        except _CharacterSceneError as error:
            self._show_error(self._scene_diagnostic(error))
        except (RuntimeError, ValueError) as exc:
            code = str(exc) if str(exc) in DIAGNOSTICS else "ROLE_SETUP_FAILED"
            self._show_error(make_diagnostic(code, str(exc), solution=str(exc), details=str(exc)))

    def _on_character_scene_event(self, event):
        if event.kind == "character_change_started":
            self._outfit_change_was_connected = self._session is not None
            self.disconnect(status="衣服正在切换，正在刷新角色…")
            return
        if event.kind == "outfit_changed":
            self._render_snapshot(event.snapshot)
            if self._outfit_change_was_connected:
                status = (
                    "衣服已切换为 {0}。请在 UE 删除旧 Actor，放置新 Binding 后重新连接。"
                ).format(event.snapshot.outfit)
            else:
                status = "当前衣服已切换为 {0}。".format(event.snapshot.outfit)
            self._outfit_change_was_connected = False
            self._set_connected(False, status)
            return
        if event.kind == "character_invalidated":
            was_connected = self._session is not None
            error = event.error
            self.disconnect(status=error.message)
            self._scene = None
            self._pending_root = None
            self._clear_scene_text()
            if was_connected:
                diagnostic = self._scene_diagnostic(error, "SAMPLING_FAILED")
                cmds.evalDeferred(lambda: self._show_error(diagnostic, session=True))

    def _on_time_unit_changed(self, *unused):
        fps = self._refresh_fps()
        if fps is None:
            if self._session is not None:
                self._session.change_rate(0.0)
            return
        try:
            validate_frame_rate(fps)
        except ValueError:
            if self._session is not None:
                self._session.change_rate(fps)
            return
        if self._session is not None:
            self._session.change_rate(fps)

    def connect(self):
        _require_maya()
        if self._session is not None:
            return
        try:
            if self._scene is None:
                if self._capture_args is not None:
                    self._capture_scene(*self._capture_args)
                else:
                    self.set_role()
            if self._scene is None:
                return
            fps = validate_frame_rate(frames_per_second(cmds.currentUnit(query=True, time=True)))
        except _CharacterSceneError as error:
            diagnostic = self._scene_diagnostic(error, "INTERNAL_ERROR")
            self._set_connected(False, diagnostic["summary"])
            self._show_error(diagnostic)
            return
        except (RuntimeError, ValueError) as exc:
            code = str(exc) if str(exc) in DIAGNOSTICS else "INVALID_FRAME_RATE" \
                if "frame rate" in str(exc) or "time unit" in str(exc) else "INTERNAL_ERROR"
            diagnostic = make_diagnostic(code, str(exc), details=str(exc))
            self._set_connected(False, diagnostic["summary"])
            self._show_error(diagnostic)
            return
        self._last_warning = {"missing_in_unreal": [], "missing_in_maya": [],
                              "bone_name_remaps": [],
                              "has_warning": False}
        self._set_connected(False, "正在连接 Unreal…")
        holder = {}

        def on_event(event):
            self._on_streaming_session_event(holder.get("session"), event)

        try:
            session = _StreamingSession.start(self._scene, fps, on_event)
        except _StreamingSessionError as error:
            diagnostic = make_diagnostic(
                error.code if error.code in DIAGNOSTICS else "INTERNAL_ERROR",
                error.message, details=error.details)
            if error.recapture_scene:
                self._clear_scene(keep_capture=True)
            self._set_connected(False, diagnostic["summary"])
            self._show_error(diagnostic)
            return
        holder["session"] = session
        self._session = session

    def _on_streaming_session_event(self, session, event):
        if session is None or self._session is not session:
            return
        if event.kind == "ready":
            warning = event.warning or {
                "missing_in_unreal": [], "missing_in_maya": [],
                "bone_name_remaps": [], "has_warning": False}
            self._last_warning = warning
            self._set_connected(
                True, "已连接（有警告）" if warning["has_warning"] else "已连接")
            if warning["has_warning"] \
                    and cmds.checkBox(self._warning_checkbox, query=True, value=True):
                self._show_warning(warning)
            return
        self._session = None
        if event.kind == "failed":
            diagnostic = event.diagnostic or make_diagnostic("INTERNAL_ERROR")
            self._last_diagnostic = diagnostic
            if event.recapture_scene:
                self._clear_scene(keep_capture=True)
            self._set_connected(False, diagnostic["summary"])
            self._show_error(diagnostic)
            return
        if event.kind == "stopped":
            status = self._pending_stop_status or "已断开连接"
            self._pending_stop_status = None
            self._set_connected(False, status)

    def _diagnostic_text(self, diagnostic=None):
        diagnostic = diagnostic or self._last_diagnostic
        try:
            snapshot = self._scene.snapshot() if self._scene is not None else None
        except _CharacterSceneError:
            snapshot = None
        warning = self._last_warning
        fps = self._refresh_fps()
        sections = [
            "摘要：{0}".format(diagnostic.get("summary", "—")),
            "解决办法：{0}".format(diagnostic.get("solution", "—")),
            "Error code: {0}".format(diagnostic.get("code", "—")),
            "Maya root: {0}".format(snapshot.root if snapshot else "—"),
            "Outfit: {0}".format(snapshot.outfit if snapshot else "—"),
            "FPS: {0}".format(format_fps(fps) if fps else "—"),
            "Bones: {0}".format(len(snapshot.bones) if snapshot else 0),
            "BlendShapes: {0}".format(len(snapshot.curve_names) if snapshot else 0),
            "Maya only: {0}".format(", ".join(warning.get("missing_in_unreal", [])) or "—"),
            "Unreal only: {0}".format(", ".join(warning.get("missing_in_maya", [])) or "—"),
            "Bone name remaps: {0}".format(
                ", ".join(warning.get("bone_name_remaps", [])) or "—"),
            "Meshes:\n{0}".format(
                "\n".join(snapshot.mesh_paths) if snapshot and snapshot.mesh_paths else "—"),
            "Technical details:\n{0}".format(
                diagnostic.get("details") or diagnostic.get("message") or "—"),
        ]
        return "\n\n".join(sections)

    def show_diagnostics(self, *unused):
        if cmds.window(DIAGNOSTIC_WINDOW_NAME, exists=True):
            cmds.deleteUI(DIAGNOSTIC_WINDOW_NAME)
        cmds.window(DIAGNOSTIC_WINDOW_NAME, title="MtoU 诊断详情", widthHeight=(620, 520))
        cmds.columnLayout(adjustableColumn=True, columnAttach=("both", 8), rowSpacing=6)
        field = cmds.scrollField(
            editable=False, wordWrap=False, text=self._diagnostic_text(), height=450)
        cmds.button(label="复制详情", command=lambda *_: self._copy_text(cmds.scrollField(
            field, query=True, text=True)))
        cmds.showWindow(DIAGNOSTIC_WINDOW_NAME)

    def _copy_text(self, value):
        try:
            from PySide2 import QtWidgets
            QtWidgets.QApplication.clipboard().setText(value)
        except ImportError:
            cmds.warning("无法访问系统剪贴板。")

    def _show_warning(self, warning):
        has_blendshape_warning = bool(
            warning["missing_in_unreal"] or warning["missing_in_maya"])
        diagnostic = make_diagnostic(
            "BLENDSHAPE_MISMATCH" if has_blendshape_warning else "BONE_NAME_REMAP")
        diagnostic["details"] = (
            "Maya only: {0}\nUnreal only: {1}\nBone name remaps: {2}".format(
                ", ".join(warning["missing_in_unreal"]) or "None",
                ", ".join(warning["missing_in_maya"]) or "None",
                ", ".join(warning["bone_name_remaps"]) or "None"))
        duplicate_paths = self._duplicate_paths()
        if warning["bone_name_remaps"] and duplicate_paths:
            diagnostic["duplicate_paths"] = list(duplicate_paths)
        self._last_diagnostic = diagnostic
        buttons = ["确定", "查看详情"]
        if diagnostic.get("duplicate_paths"):
            buttons.insert(0, "选中重名骨骼")
        result = cmds.confirmDialog(
            title="MtoU 连接警告",
            message=diagnostic["summary"] + "\n\n" + diagnostic["solution"],
            button=buttons, defaultButton="确定", cancelButton="确定")
        if result == "选中重名骨骼":
            self.select_duplicate_bones()
        if result == "查看详情":
            self.show_diagnostics()

    def _show_error(self, diagnostic, session=False):
        del session
        self._last_diagnostic = diagnostic
        buttons = ["确定", "查看详情"]
        if diagnostic.get("duplicate_paths"):
            buttons.insert(0, "选中重名骨骼")
        result = cmds.confirmDialog(
            title="MtoU 连接问题", message=diagnostic["summary"] + "\n\n" + diagnostic["solution"],
            button=buttons, defaultButton="确定", cancelButton="确定")
        if result == "选中重名骨骼":
            self.select_duplicate_bones()
        if result == "查看详情":
            self.show_diagnostics()

    def _on_scene_change(self, *unused):
        self._clear_role()

    def disconnect(self, keep_error=False, status="已断开连接"):
        del keep_error
        session = self._session
        if session is None:
            self._set_connected(False, status)
            return
        self._pending_stop_status = status
        session.stop()

    def _clear_scene_text(self):
        self._set_text(self._root_text, "角色根骨骼：—")
        self._set_text(self._outfit_text, "当前衣服：—")
        self._set_text(self._bone_text, "骨骼数：0")
        self._set_text(self._curve_text, "BlendShape 数：0")
        self._refresh_duplicate_button()

    def _clear_scene(self, keep_pending=False, keep_capture=False):
        scene, self._scene = self._scene, None
        if scene is not None:
            scene.close()
        if not keep_pending:
            self._pending_root = None
        if not keep_capture:
            self._capture_args = None

    def _clear_role(self):
        self.disconnect(status="未设置角色")
        self._clear_scene()
        self._clear_scene_text()

    def close(self, *unused):
        self._clear_role()


_CONTROLLER = None
WINDOW_NAME = "MtoULiveLinkWindow"
DIAGNOSTIC_WINDOW_NAME = "MtoULiveLinkDiagnosticWindow"


def run():
    global _CONTROLLER
    _require_maya()
    if cmds.window(WINDOW_NAME, exists=True):
        cmds.showWindow(WINDOW_NAME)
        cmds.window(WINDOW_NAME, edit=True, restoreCommand="")
        return _CONTROLLER
    _CONTROLLER = _Controller()
    _CONTROLLER.build_ui()
    return _CONTROLLER


def main():
    return run()


if __name__ == "__main__":
    main()
