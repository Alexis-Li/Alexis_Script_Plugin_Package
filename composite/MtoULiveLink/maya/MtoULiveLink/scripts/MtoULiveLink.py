"""MtoULiveLink Maya entry point."""

import errno
import json
import math
import os
import queue
import re
import select
import shutil
import socket
import struct
import tempfile
import threading
import time
import uuid

__version__ = "0.5.0"
PROTOCOL_VERSION = 6
WORKFLOW_ANIMATION = "animation"
WORKFLOW_MODEL = "model"
WORKFLOWS = (WORKFLOW_ANIMATION, WORKFLOW_MODEL)
# Frozen per-message framing ceiling enforced by both adapters before any
# JSON parse or allocation; oversized messages close the connection. Pinned by
# limits.max_message_bytes in the conformance corpus (asserted in tests).
MAX_MESSAGE_BYTES = 32 * 1024 * 1024
HOST = "127.0.0.1"
PORT = 54321
SUBJECT_NAME = "MtoU_Character"
CURVE_VALUE_TOLERANCE = 1.0e-6
BIND_MATRIX_TOLERANCE = 1.0e-5
MIN_FRAME_RATE = 1.0
MAX_FRAME_RATE = 60.0
PLAYBACK_CAP_OPTION_VAR = "MtoULiveLinkPlaybackCap"
DEFAULT_PLAYBACK_CAP = "20 fps"
PLAYBACK_CAP_CHOICES = ("Follow Scene", "30 fps", "20 fps", "15 fps")
CACHE_OWNER = "MtoULiveLink"
CACHE_FORMAT_VERSION = 1
CACHE_FILE_PREFIX = "MtoULiveLinkPlayback-"
CACHE_METADATA_SUFFIX = ".metadata.json"
CACHE_FRAMES_SUFFIX = ".frames"
CACHE_PARTIAL_METADATA_SUFFIX = ".partial.json"
CACHE_PARTIAL_FRAMES_SUFFIX = ".partial.frames"
CACHE_CONFIRMATION_BYTES = 1 << 30
CACHE_STALE_SECONDS = 24 * 60 * 60
# Recalibrated with the first real-project capture (320 frames at 30 fps
# exceeding the previous 64 MiB estimate); still a frozen bound, not a
# user tuning knob.
MAX_CACHE_PAYLOAD_BYTES = 1024 * 1024 * 1024
MAX_CACHE_FRAME_COUNT = 20000
UPLOAD_CHUNK_FRAMES = 64
# Watchdog for a wedged-but-open connection during the Ready wait; the upload
# itself has no real-time deadline and is never paced by this value.
UPLOAD_READY_TIMEOUT_SECONDS = 60.0
REALTIME_MODE = "realtime"
CACHED_MODE = "cached"
TOGGLE_ON_BACKGROUND = (0.16, 0.55, 0.24)
TOGGLE_OFF_BACKGROUND = (0.26, 0.26, 0.26)
# Workflow tabs use a neutral lighter tone so they never read as a green
# mode toggle or a blue primary action.
TAB_ON_BACKGROUND = (0.52, 0.52, 0.52)
TAB_OFF_BACKGROUND = (0.20, 0.20, 0.20)
LIGHT_ON_BACKGROUND = (0.08, 0.45, 0.12)
LIGHT_OFF_BACKGROUND = (0.55, 0.08, 0.08)
# Panel grid: card stack width, outer margin, and the shared 4-column grid
# inside each card (CARD_HALF = two grid columns + gutter for 2-up rows).
PANEL_WIDTH = 480
PANEL_MARGIN = 16
CARD_MARGIN = 12
GRID_GAP = 10
CARD_GAP = GRID_GAP
ROW_SPACING = 10
TAB_HEIGHT = 40
BUTTON_HEIGHT = 34
CARD_CONTENT = PANEL_WIDTH
CARD_HALF = (CARD_CONTENT - CARD_GAP) // 2
CARD_QUARTER = (CARD_CONTENT - 3 * CARD_GAP) // 4
PRIMARY_BACKGROUND = (0.20, 0.40, 0.48)
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
        "请按 UE 详情检查首个未映射路径、父级与导入改名候选；"
        "确认资产确实不一致后再重新导出或导入。"),
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
    "BIND_POSE_INVALID": (
        "无法读取可靠的 Maya 绑定姿势",
        "请检查绑定矩阵是否包含非有限值、不可逆矩阵，或存在无法可靠决胜的冲突。"),
    "INCOMPLETE_SKELETON": (
        "角色骨架存在未能发布的关节分支",
        "请检查详情中的首个缺失关节路径及原因；只有 joint 与通向关节的 transform 才能发布，中间出现其他类型节点时请整理绑定后重试。"),
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
    "CACHED_PLAYBACK_NOT_READY": (
        "缓存播放需要已连接的 Unreal",
        "请先完成角色设置并连接 Unreal，再进入缓存播放。"),
    "CACHED_PLAYBACK_NO_CACHE": (
        "没有可回放的缓存",
        "请先捕获并回放当前 Maya Playback Range。"),
    "CACHED_PLAYBACK_SPACE": (
        "临时磁盘空间不足",
        "请缩短 Playback Range 或清理当前用户的临时磁盘空间后重试。"),
    "CACHED_PLAYBACK_CANCELLED": (
        "缓存捕获已取消",
        "已删除未完成缓存并恢复到实时预览。"),
    "CACHED_PLAYBACK_INCOMPATIBLE": (
        "缓存与当前角色快照不兼容",
        "请为当前角色和服装重新捕获缓存。"),
    "CACHED_UPLOAD_FAILED": (
        "缓存上传被 Unreal 拒绝",
        "Unreal 已丢弃未完成的缓存；请重新捕获后再试。"),
    "CACHED_PLAYBACK_FRAME_LIMIT": (
        "缓存帧数超过 20,000 上限，捕获已提前停止",
        "请缩短 Playback Range 后重新捕获。"),
    "CACHED_PLAYBACK_PAYLOAD_LIMIT": (
        "缓存编码体积超过 1 GiB 上限，捕获已提前停止",
        "请缩短 Playback Range 或减少 BlendShape 数量后重新捕获。"),
    "CACHED_PLAYBACK_PERFORMANCE": (
        "缓存回放跟不上捕获帧率，已停止",
        "已完整保留缓存；请关闭占用性能的程序后点击“再次回放”重试。"),
    "PREVIEW_NOT_READY": (
        "模型预览尚未生成，无法连接模型工作流",
        "请先在 UE 中对该 Binding Actor 执行 Refresh Preview 生成预览，再重新连接。"),
    "PREVIEW_BUILD_FAILED": (
        "模型预览生成失败",
        "请按诊断详情修复输入后，在 UE 中重新执行 Refresh Preview。"),
    "PREVIEW_MORPH_MISMATCH": (
        "Maya 与模型预览的 BlendShape 没有交集",
        "请确认当前服装与 UE Binding 一致并重新生成预览；也可关闭“传递 BS”进行仅骨骼对比。"),
}

try:
    import maya.api.OpenMaya as om
    import maya.api.OpenMayaAnim as oma
    import maya.cmds as cmds
except ImportError:
    om = None
    oma = None
    cmds = None


def normalize_playback_cap(value):
    if value is None:
        return DEFAULT_PLAYBACK_CAP
    text = str(value).strip().lower()
    if text in ("follow scene", "follow_scene", "scene"):
        return "Follow Scene"
    for choice in PLAYBACK_CAP_CHOICES[1:]:
        if text in (choice.lower(), choice.split()[0]):
            return choice
    return DEFAULT_PLAYBACK_CAP


def playback_cap_fps(value):
    value = normalize_playback_cap(value)
    if value == "Follow Scene":
        return None
    return float(value.split()[0])


def save_playback_cap(value):
    value = normalize_playback_cap(value)
    if cmds is not None:
        try:
            cmds.optionVar(stringValue=(PLAYBACK_CAP_OPTION_VAR, value))
        except (AttributeError, RuntimeError, TypeError):
            pass
    return value


def load_playback_cap():
    value = DEFAULT_PLAYBACK_CAP
    if cmds is not None:
        try:
            if cmds.optionVar(exists=PLAYBACK_CAP_OPTION_VAR):
                value = cmds.optionVar(query=PLAYBACK_CAP_OPTION_VAR)
        except (AttributeError, RuntimeError, TypeError):
            value = DEFAULT_PLAYBACK_CAP
    value = normalize_playback_cap(value)
    return save_playback_cap(value)


def _maya_is_playing():
    if cmds is None:
        return False
    try:
        return bool(cmds.play(query=True, state=True))
    except (AttributeError, RuntimeError, TypeError):
        return False


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


def filter_bone_driven_differences(init_message, reply):
    """Drop UE-only Morph differences for outfits that declare no BlendShapes.

    A bone-driven outfit declares zero BlendShape names, so generated Morph
    Targets missing in Maya are expected and are not an expression-coverage
    warning.
    """
    if (init_message.get("workflow") == WORKFLOW_MODEL
            and init_message.get("blendshapes_enabled")
            and not init_message.get("curves")):
        reply = dict(reply)
        reply["missing_in_maya"] = []
    return reply


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


def make_init_message(bones, curves, revision, workflow=WORKFLOW_ANIMATION,
                      blendshapes_enabled=True):
    if workflow not in WORKFLOWS:
        raise ValueError("workflow must be 'animation' or 'model'")
    return {
        "type": "init",
        "version": PROTOCOL_VERSION,
        "revision": int(revision),
        "workflow": workflow,
        "blendshapes_enabled": bool(blendshapes_enabled),
        "bones": bones,
        "curves": curves,
    }


def make_frame_message(transforms, curves):
    flat = [value for transform in transforms for value in transform] + list(curves)
    if not _finite(flat):
        raise ValueError("frame values must be finite")
    return {"type": "frame", "transforms": transforms, "curves": curves}


def make_cache_enter_message():
    return {"type": "cache_enter"}


def make_cache_begin_message(revision, start_frame, end_frame, fps, payload_size,
                             upload_id):
    revision = int(revision)
    start_frame = int(start_frame)
    end_frame = int(end_frame)
    frame_count = int(end_frame) - int(start_frame) + 1
    fps = float(fps)
    payload_size = int(payload_size)
    upload_id = int(upload_id)
    if not math.isfinite(fps):
        raise ValueError("cache fps must be finite")
    if upload_id < 1:
        raise ValueError("cache upload id must be a positive integer")
    if frame_count < 1 or frame_count > MAX_CACHE_FRAME_COUNT:
        raise ValueError("cache frame count is outside the supported range")
    if payload_size < 1 or payload_size > MAX_CACHE_PAYLOAD_BYTES:
        raise ValueError(
            "cache encoded size {0} bytes exceeds the Unreal transient"
            " limit of {1} bytes".format(
                payload_size, MAX_CACHE_PAYLOAD_BYTES))
    return {
        "type": "cache_begin",
        "upload_id": upload_id,
        "revision": revision,
        "fps": fps,
        "start_frame": start_frame,
        "end_frame": end_frame,
        "frame_count": frame_count,
        "payload_size": payload_size,
    }


def make_cache_frame_message(index, transforms, curves):
    index = int(index)
    if index < 0:
        raise ValueError("cached frame index must not be negative")
    flat = [value for transform in transforms for value in transform] + list(curves)
    if not _finite(flat):
        raise ValueError("frame values must be finite")
    return {"type": "cache_frame", "index": index,
            "transforms": transforms, "curves": curves}


def make_cache_end_message():
    return {"type": "cache_end"}


def make_cache_play_message(play_id):
    play_id = int(play_id)
    if play_id < 1:
        raise ValueError("cache play id must be a positive integer")
    return {"type": "cache_play", "play_id": play_id}


def make_cache_stop_message():
    return {"type": "cache_stop"}


def make_cache_clear_message():
    return {"type": "cache_clear"}


def cache_frame_wire_size(index, transforms, curves):
    """Exact transmitted UTF-8 JSON byte length of one cache_frame message.

    The declared upload size must match what Unreal meters at the framing
    boundary: the JSON payload only, without the length prefix or the
    cache-file newline.
    """
    payload = json.dumps(
        make_cache_frame_message(index, transforms, curves),
        ensure_ascii=False, allow_nan=False, separators=(",", ":"))
    return len(payload.encode("utf-8"))


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
    if size > MAX_MESSAGE_BYTES:
        raise ValueError("protocol message length exceeds the supported message size")
    return json.loads(_recv_exact(sock, size).decode("utf-8"))


def _exact_int(value):
    """Return value when it is a non-bool integer JSON number, else None."""
    if isinstance(value, bool) or not isinstance(value, int):
        return None
    return value


def _finite_float(value):
    """Return value as float when it is a non-bool finite JSON number."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    number = float(value)
    return number if math.isfinite(number) else None


def validate_reply(reply):
    if not isinstance(reply, dict):
        raise ValueError("protocol reply must be a JSON object")
    reply_type = reply.get("type")
    required = {
        "ready": {
            "revision": (int, float),
            "missing_in_unreal": list,
            "missing_in_maya": list,
            "bone_name_remaps": list,
            "workflow": str,
            "target_morph_count": (int, float),
            "accepted_morph_count": (int, float),
        },
        "error": {"code": str, "message": str, "details": str},
        "cache_ready": {
            "upload_id": (int, float),
            "revision": (int, float),
            "frame_count": (int, float),
        },
        "cache_progress": {"play_id": (int, float), "applied": (int, float)},
        "cache_complete": {
            "play_id": (int, float),
            "applied_frame_count": (int, float),
            "elapsed_seconds": (int, float),
        },
        "cache_stopped": {"play_id": (int, float)},
        "cache_cleared": {"upload_id": (int, float), "play_id": (int, float)},
    }
    fields = required.get(reply_type)
    if fields is None:
        raise ValueError(
            "protocol reply type must be 'ready', 'error' or a cache outcome")
    for name, expected_type in fields.items():
        if name not in reply:
            raise ValueError("protocol reply requires field '{0}'".format(name))
        if not isinstance(reply[name], expected_type):
            raise ValueError("protocol reply field '{0}' has the wrong JSON type".format(name))
        if expected_type is list and any(not isinstance(value, str) for value in reply[name]):
            raise ValueError("protocol reply field '{0}' must contain strings".format(name))
    for name in ("revision", "target_morph_count", "accepted_morph_count",
                 "upload_id", "play_id", "applied", "applied_frame_count"):
        if name not in reply:
            continue
        value = reply[name]
        if isinstance(value, bool) or not float(value).is_integer():
            raise ValueError(
                "protocol reply field '{0}' must be an integer count".format(name))
    if "elapsed_seconds" in reply and _finite_float(reply["elapsed_seconds"]) is None:
        raise ValueError(
            "protocol reply field 'elapsed_seconds' must be a finite number")
    return reply


def _require_maya():
    if cmds is None or om is None:
        raise RuntimeError("MtoU_LiveLink must be run inside Maya 2022.4")


def _dag_path(path):
    selection = om.MSelectionList()
    selection.add(path)
    return selection.getDagPath(0)


def _transform_leads_to_joint(path):
    return bool(cmds.listRelatives(
        path, allDescendents=True, fullPath=True, type="joint"))


def _maya_children(path):
    children = cmds.listRelatives(path, children=True, fullPath=True) or []
    result = []
    for child in children:
        try:
            node_type = cmds.nodeType(child)
        except (RuntimeError, ValueError):
            continue
        if node_type == "joint":
            result.append(child)
        elif node_type == "transform":
            if _transform_leads_to_joint(child):
                result.append(child)
        # Other DAG types (mesh shapes, constraints, controllers) never
        # publish; joints beneath them are reported by
        # _ensure_complete_capture instead of being silently pruned.
    return result


def _describe_incomplete_joint(root, joint_path, published):
    current = joint_path
    while current != root:
        parents = cmds.listRelatives(current, parent=True, fullPath=True) or []
        if not parents:
            return ("joint {0} has no parent chain reaching selected root {1}".format(
                joint_path, root))
        parent = parents[0]
        if parent not in published:
            try:
                parent_type = cmds.nodeType(parent)
            except (RuntimeError, ValueError):
                parent_type = "unknown"
            if parent_type not in ("joint", "transform"):
                return ("joint {0} passes through {1} ({2}); "
                        "only joint/transform intermediates can be published".format(
                    joint_path, parent, parent_type))
            return ("joint {0} via unpublished {1} ({2}) was not captured".format(
                joint_path, parent, parent_type))
        current = parent
    return "joint {0} was not captured".format(joint_path)


def _ensure_complete_capture(root, bones):
    published = set(bone["path"] for bone in bones)
    joints = cmds.listRelatives(
        root, allDescendents=True, fullPath=True, type="joint") or []
    for joint_path in sorted([root] + list(joints)):
        if joint_path in published:
            continue
        reason = _describe_incomplete_joint(root, joint_path, published)
        message = "skeleton under {0} is missing joint {1}".format(root, joint_path)
        details = "{0}; {1}".format(message, reason)
        raise _CharacterSceneError(
            "INCOMPLETE_SKELETON", message, details=details,
            context={"root": root, "missing_joint": joint_path})


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


def _skin_clusters_for_meshes(mesh_paths):
    skin_clusters = set()
    for mesh_path in mesh_paths:
        skin_clusters.update(
            node for node in cmds.listHistory(mesh_path, pruneDagObjects=True) or []
            if cmds.nodeType(node) == "skinCluster")
    return sorted(skin_clusters)


def _matrix_attr(plug):
    value = cmds.getAttr(plug)
    if isinstance(value, (list, tuple)) and len(value) == 1:
        value = value[0]
    return om.MMatrix(value)


def _matrix_is_finite(matrix):
    return all(math.isfinite(float(value)) for value in matrix)


def _matrix_is_equivalent(left, right, tolerance=BIND_MATRIX_TOLERANCE):
    return all(abs(float(a) - float(b)) <= tolerance for a, b in zip(left, right))


class _BindPoseError(ValueError):
    pass


class _BindMatrixCandidate(object):
    SKIN_CLUSTER = "skinCluster"
    DAG_POSE = "dagPose"

    def __init__(self, kind, source, matrix, influence_count=0):
        self.kind = kind
        self.source = source
        self.matrix = matrix
        self.influence_count = int(influence_count)


def _matrix_inverse(matrix, bone_path, source):
    if not _matrix_is_finite(matrix):
        raise _BindPoseError(
            "bone {0} has non-finite bind matrix from {1}".format(
                bone_path, source))
    determinant = float(matrix.det4x4())
    if not math.isfinite(determinant) or determinant == 0.0:
        raise _BindPoseError(
            "bone {0} has non-invertible bind matrix from {1}".format(
                bone_path, source))
    inverse = matrix.inverse()
    if not _matrix_is_finite(inverse):
        raise _BindPoseError(
            "bone {0} has non-finite inverse bind matrix from {1}".format(
                bone_path, source))
    return inverse


def _highest_priority_bind_candidates(records):
    dag_pose_records = [
        record for record in records
        if record.kind == _BindMatrixCandidate.DAG_POSE]
    if dag_pose_records:
        return dag_pose_records
    largest_influence_count = max(record.influence_count for record in records)
    return [record for record in records
            if record.influence_count == largest_influence_count]


def _select_bind_world_candidate(bone_path, records):
    preferred = _highest_priority_bind_candidates(records)
    selected = preferred[0]
    if any(not _matrix_is_equivalent(selected.matrix, record.matrix)
           for record in preferred[1:]):
        sources = ", ".join(sorted(record.source for record in preferred))
        raise _BindPoseError(
            "bone {0} has conflicting equally ranked bind matrices: {1}".format(
                bone_path, sources))
    conflict = any(not _matrix_is_equivalent(selected.matrix, record.matrix)
                   for record in records)
    return selected.matrix, conflict


def _bind_world_candidates(subject):
    candidates = dict((bone["path"], []) for bone in subject["bones"])
    bone_paths = set(candidates)
    for skin_cluster in _skin_clusters_for_meshes(subject["meshes"]):
        selection = om.MSelectionList()
        selection.add(skin_cluster)
        function = oma.MFnSkinCluster(selection.getDependNode(0))
        influences = function.influenceObjects()
        for influence in influences:
            path = influence.fullPathName()
            if path not in bone_paths:
                continue
            logical_index = function.indexForInfluenceObject(influence)
            plug = "{0}.bindPreMatrix[{1}]".format(skin_cluster, logical_index)
            candidates[path].append(_BindMatrixCandidate(
                _BindMatrixCandidate.SKIN_CLUSTER,
                plug,
                _matrix_inverse(_matrix_attr(plug), path, plug),
                influence_count=len(influences)))

    for bone in subject["bones"]:
        if cmds.nodeType(bone["path"]) != "joint":
            # Structural transforms have no joint bindPose/dagPose; when they
            # are not skin influences they fall back to the capture-time
            # local re-anchored to the parent bind frame in
            # _capture_bind_local_transforms.
            continue
        plug = bone["path"] + ".bindPose"
        for pose_plug in cmds.listConnections(
                plug, source=False, destination=True, plugs=True) or []:
            pose = pose_plug.split(".", 1)[0]
            if cmds.nodeType(pose) == "dagPose" and cmds.getAttr(pose + ".bindPose"):
                # The joint-connected dagPose is the pose Go to Bind Pose
                # restores and the most likely Skeletal Mesh export pose.
                matrix = _matrix_attr(pose_plug)
                _matrix_inverse(matrix, bone["path"], pose_plug)
                candidates[bone["path"]].append(_BindMatrixCandidate(
                    _BindMatrixCandidate.DAG_POSE, pose_plug, matrix))
    return candidates


def _capture_bind_local_transforms(subject):
    candidates = _bind_world_candidates(subject)
    current_world = [bone["dag_path"].inclusiveMatrix() for bone in subject["bones"]]
    bind_world = []
    conflicts = 0
    for index, bone in enumerate(subject["bones"]):
        records = candidates[bone["path"]]
        if records:
            # Skin clusters can legitimately disagree when outfits were bound
            # at different poses; the joint-connected dagPose wins, otherwise
            # the skin cluster with the most influences. Equally ranked
            # candidates must agree so node naming cannot choose the pose.
            matrix, conflict = _select_bind_world_candidate(bone["path"], records)
            conflicts += int(conflict)
        elif bone["parent"] >= 0:
            # Bones with no stored bind data (for example corrective slider
            # joints added after binding) use their capture-time local offset
            # re-anchored to the parent's bind frame.
            parent = bone["parent"]
            parent_inverse = _matrix_inverse(
                current_world[parent], bone["path"],
                "capture-time parent matrix")
            matrix = current_world[index] * parent_inverse \
                * bind_world[parent]
        else:
            matrix = current_world[index]
        bind_world.append(matrix)

    bind_world_inverse = [
        _matrix_inverse(matrix, bone["path"], "resolved bind world matrix")
        for bone, matrix in zip(subject["bones"], bind_world)]
    bind_local = []
    for index, bone in enumerate(subject["bones"]):
        matrix = bind_world[index]
        if bone["parent"] >= 0:
            matrix = matrix * bind_world_inverse[bone["parent"]]
        bind_local.append(_sample_matrix(matrix, subject["unit_scale"]))
    return bind_local, conflicts


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
    _ensure_complete_capture(root, bones)
    for bone in bones:
        bone["dag_path"] = _dag_path(bone["path"])
    mesh_paths = _visible_skinned_meshes([bone["path"] for bone in bones])
    if not mesh_paths:
        raise ValueError("NO_VISIBLE_SKINNED_MESH")
    subject = {"root": root, "bones": bones,
               "meshes": mesh_paths,
               "curves": _discover_curve_plugs(
                   [bone["path"] for bone in bones], mesh_paths),
               "unit_scale": centimeters_per_unit(
                   cmds.currentUnit(query=True, linear=True))}
    subject["bind_local_transforms"], subject["bind_conflict_count"] = \
        _capture_bind_local_transforms(subject)
    return subject


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
                 duplicate_paths=(), mesh_paths=(), bind_local_transforms=(),
                 bind_conflict_count=0):
        self._revision = int(revision)
        self._root = root
        self._outfit = outfit
        self._bones = tuple((name, int(parent)) for name, parent in bones)
        self._curve_names = tuple(curve_names)
        self._duplicate_paths = tuple(duplicate_paths)
        self._mesh_paths = tuple(mesh_paths)
        self._bind_conflict_count = int(bind_conflict_count)
        self._bind_local_transforms = tuple(
            tuple(value for value in transform) for transform in bind_local_transforms)
        if len(self._bind_local_transforms) != len(self._bones):
            raise ValueError("bind pose count must match skeleton bone count")

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

    @property
    def bind_local_transforms(self):
        return self._bind_local_transforms

    @property
    def bind_conflict_count(self):
        return self._bind_conflict_count


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
        except _BindPoseError as exc:
            raise self._wrap_error("BIND_POSE_INVALID", exc)
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
            subject["meshes"],
            subject["bind_local_transforms"],
            subject.get("bind_conflict_count", 0))
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
                candidate.mesh_paths, candidate.bind_local_transforms,
                candidate.bind_conflict_count)
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


class _PlaybackCacheError(RuntimeError):
    """Raised when a temporary cached-playback file cannot be used."""


class _PlaybackCache(object):
    """Incremental, atomically finalized storage for captured frame messages."""

    _TOKEN_PATTERN = re.compile(r"^[0-9a-f]{32}$")

    def __init__(self, metadata_path, frames_path, metadata, writer=None,
                 partial_metadata_path=None, partial_frames_path=None):
        self._metadata_path = os.fspath(metadata_path)
        self._frames_path = os.fspath(frames_path)
        self._metadata = dict(metadata)
        self._writer = writer
        base = self._metadata_path
        if base.endswith(CACHE_METADATA_SUFFIX):
            base = base[:-len(CACHE_METADATA_SUFFIX)]
        self._partial_metadata_path = os.fspath(
            partial_metadata_path or base + CACHE_PARTIAL_METADATA_SUFFIX)
        self._partial_frames_path = os.fspath(
            partial_frames_path or base + CACHE_PARTIAL_FRAMES_SUFFIX)
        self._deleted = False

    @classmethod
    def begin(cls, snapshot_revision, capture_start, capture_end, scene_fps,
              temp_dir=None, capture_time=None):
        directory = os.fspath(temp_dir or tempfile.gettempdir())
        os.makedirs(directory, exist_ok=True)
        cache_id = uuid.uuid4().hex
        if not cls._TOKEN_PATTERN.match(cache_id):
            raise _PlaybackCacheError("generated cache id is invalid")
        base = os.path.join(directory, CACHE_FILE_PREFIX + cache_id)
        metadata_path = base + CACHE_METADATA_SUFFIX
        frames_path = base + CACHE_FRAMES_SUFFIX
        partial_metadata_path = base + CACHE_PARTIAL_METADATA_SUFFIX
        partial_frames_path = base + CACHE_PARTIAL_FRAMES_SUFFIX
        metadata = {
            "owner": CACHE_OWNER,
            "format_version": CACHE_FORMAT_VERSION,
            "cache_id": cache_id,
            "snapshot_revision": int(snapshot_revision),
            "revision": int(snapshot_revision),
            "capture_range": [int(capture_start), int(capture_end)],
            "capture_start": int(capture_start),
            "capture_end": int(capture_end),
            "frame_count": 0,
            "scene_fps": float(scene_fps),
            "captured_scene_rate": float(scene_fps),
            "capture_time": float(time.time() if capture_time is None else capture_time),
            "completion_state": "partial",
            "completed": False,
            "frames_file": os.path.basename(frames_path),
        }
        try:
            writer = open(partial_frames_path, "w", encoding="utf-8", newline="\n")
            with open(partial_metadata_path, "w", encoding="utf-8",
                      newline="\n") as stream:
                json.dump(
                    metadata, stream, ensure_ascii=False,
                    allow_nan=False, separators=(",", ":"))
                stream.write("\n")
        except (OSError, TypeError, ValueError) as exc:
            try:
                writer.close()
            except (UnboundLocalError, OSError):
                pass
            for path in (partial_metadata_path, partial_frames_path):
                try:
                    os.remove(path)
                except (FileNotFoundError, OSError):
                    pass
            raise _PlaybackCacheError(str(exc))
        return cls(
            metadata_path, frames_path, metadata, writer,
            partial_metadata_path, partial_frames_path)

    @classmethod
    def load(cls, metadata_path):
        metadata_path = os.fspath(metadata_path)
        try:
            with open(metadata_path, "r", encoding="utf-8") as stream:
                metadata = json.load(stream)
        except (OSError, ValueError, TypeError) as exc:
            raise _PlaybackCacheError("invalid cache metadata: {0}".format(exc))
        cache_id = metadata.get("cache_id") if isinstance(metadata, dict) else None
        expected_metadata_name = CACHE_FILE_PREFIX + str(cache_id) + CACHE_METADATA_SUFFIX
        if os.path.basename(metadata_path) != expected_metadata_name:
            raise _PlaybackCacheError("cache metadata filename is not owned by MtoU")
        cls._validate_metadata(metadata, require_complete=True)
        frames_file = metadata.get("frames_file")
        if not frames_file:
            frames_file = os.path.basename(metadata_path).replace(
                CACHE_METADATA_SUFFIX, CACHE_FRAMES_SUFFIX)
        frames_path = os.path.join(os.path.dirname(metadata_path), frames_file)
        if not os.path.isfile(frames_path):
            raise _PlaybackCacheError("completed cache frame file is missing")
        return cls(metadata_path, frames_path, metadata)

    @staticmethod
    def _validate_metadata(metadata, require_complete=False):
        if not isinstance(metadata, dict):
            raise _PlaybackCacheError("cache metadata must be a JSON object")
        if metadata.get("owner") != CACHE_OWNER:
            raise _PlaybackCacheError("cache metadata has an unknown owner")
        if int(metadata.get("format_version", -1)) != CACHE_FORMAT_VERSION:
            raise _PlaybackCacheError("unsupported cache format")
        cache_id = metadata.get("cache_id")
        if not isinstance(cache_id, str) or not _PlaybackCache._TOKEN_PATTERN.match(cache_id):
            raise _PlaybackCacheError("cache metadata has an invalid cache id")
        expected_frames_name = CACHE_FILE_PREFIX + cache_id + CACHE_FRAMES_SUFFIX
        if metadata.get("frames_file", expected_frames_name) != expected_frames_name:
            raise _PlaybackCacheError("cache frame filename is not owned by MtoU")
        if require_complete and metadata.get("completed") is not True:
            raise _PlaybackCacheError("cache is not complete")
        if require_complete and metadata.get("completion_state") != "complete":
            raise _PlaybackCacheError("cache completion state is invalid")
        for name in ("snapshot_revision", "capture_start", "capture_end",
                     "frame_count", "scene_fps", "capture_time"):
            if name not in metadata:
                raise _PlaybackCacheError("cache metadata requires '{0}'".format(name))
        if int(metadata["capture_end"]) < int(metadata["capture_start"]):
            raise _PlaybackCacheError("cache capture range is invalid")
        if int(metadata["frame_count"]) < 0:
            raise _PlaybackCacheError("cache frame count is invalid")
        if (require_complete
                and int(metadata["frame_count"])
                != int(metadata["capture_end"]) - int(metadata["capture_start"]) + 1):
            raise _PlaybackCacheError("cache frame count does not cover the capture range")

    @staticmethod
    def serialized_frame_size(frame_message):
        payload = json.dumps(
            frame_message, ensure_ascii=False, allow_nan=False,
            separators=(",", ":"),
        ).encode("utf-8")
        return len(payload) + 1

    @property
    def path(self):
        return self._metadata_path

    @property
    def metadata_path(self):
        return self._metadata_path

    @property
    def frames_path(self):
        return self._frames_path

    @property
    def metadata(self):
        return dict(self._metadata)

    @property
    def cache_id(self):
        return self._metadata.get("cache_id")

    @property
    def snapshot_revision(self):
        return int(self._metadata["snapshot_revision"])

    @property
    def revision(self):
        return self.snapshot_revision

    @property
    def capture_range(self):
        return (
            int(self._metadata["capture_start"]),
            int(self._metadata["capture_end"]),
        )

    @property
    def frame_count(self):
        return int(self._metadata["frame_count"])

    @property
    def scene_fps(self):
        return float(self._metadata["scene_fps"])

    @property
    def captured_scene_rate(self):
        return self.scene_fps

    @property
    def capture_time(self):
        return float(self._metadata["capture_time"])

    @property
    def completion_state(self):
        return self._metadata.get("completion_state", "partial")

    @property
    def completed(self):
        return bool(self._metadata.get("completed"))

    def append(self, frame_message):
        if self._writer is None or self._deleted:
            raise _PlaybackCacheError("cache is not accepting frames")
        try:
            payload = json.dumps(
                frame_message, ensure_ascii=False, allow_nan=False,
                separators=(",", ":"),
            )
            self._writer.write(payload + "\n")
            self._writer.flush()
        except (OSError, TypeError, ValueError) as exc:
            raise _PlaybackCacheError(str(exc))
        self._metadata["frame_count"] = int(self._metadata["frame_count"]) + 1

    append_frame = append

    def finalize(self):
        if self._deleted:
            raise _PlaybackCacheError("cache has been deleted")
        if self._writer is None:
            if self.completed:
                return self
            raise _PlaybackCacheError("cache is already finalized")
        expected_frames = (
            int(self._metadata["capture_end"])
            - int(self._metadata["capture_start"])
            + 1)
        if int(self._metadata["frame_count"]) != expected_frames:
            self.delete()
            raise _PlaybackCacheError(
                "cache frame count does not cover the capture range")
        try:
            self._writer.flush()
            self._writer.close()
            self._writer = None
            self._metadata["completion_state"] = "complete"
            self._metadata["completed"] = True
            with open(self._partial_metadata_path, "w", encoding="utf-8",
                      newline="\n") as stream:
                json.dump(
                    self._metadata, stream, ensure_ascii=False,
                    allow_nan=False, separators=(",", ":"))
                stream.write("\n")
                stream.flush()
            os.replace(self._partial_frames_path, self._frames_path)
            os.replace(self._partial_metadata_path, self._metadata_path)
        except (OSError, TypeError, ValueError) as exc:
            self.delete()
            raise _PlaybackCacheError(str(exc))
        return self

    def iter_frames(self):
        if not self.completed or self._deleted:
            raise _PlaybackCacheError("only a completed cache can be replayed")
        expected_frames = self.frame_count

        def frames():
            seen_frames = 0
            try:
                stream = open(self._frames_path, "r", encoding="utf-8")
            except OSError as exc:
                raise _PlaybackCacheError(str(exc))
            with stream:
                for line in stream:
                    if not line.strip():
                        continue
                    if seen_frames >= expected_frames:
                        raise _PlaybackCacheError(
                            "cached frame file contains too many frames")
                    try:
                        frame = json.loads(line)
                    except (TypeError, ValueError) as exc:
                        raise _PlaybackCacheError(
                            "invalid cached frame: {0}".format(exc))
                    if not isinstance(frame, dict) or frame.get("type") != "frame":
                        raise _PlaybackCacheError("cached frame is not a captured frame message")
                    seen_frames += 1
                    yield frame
                if seen_frames != expected_frames:
                    raise _PlaybackCacheError(
                        "cached frame file does not contain the complete sequence")

        return frames()

    def compatible_with(self, snapshot_revision):
        return int(snapshot_revision) == self.snapshot_revision

    def delete(self):
        if self._deleted:
            return
        self._deleted = True
        writer, self._writer = self._writer, None
        if writer is not None:
            try:
                writer.close()
            except OSError:
                pass
        paths = (
            self._metadata_path,
            self._frames_path,
            self._partial_metadata_path,
            self._partial_frames_path,
        )
        for path in paths:
            try:
                os.remove(path)
            except FileNotFoundError:
                pass
            except OSError:
                pass

    @classmethod
    def cleanup_stale(cls, temp_dir=None, now=None, age_seconds=CACHE_STALE_SECONDS):
        directory = os.fspath(temp_dir or tempfile.gettempdir())
        cutoff = float(time.time() if now is None else now) - float(age_seconds)
        if not os.path.isdir(directory):
            return []
        removed = []
        names = os.listdir(directory)
        metadata_names = [
            name for name in names
            if name.endswith(CACHE_METADATA_SUFFIX)
            or name.endswith(CACHE_PARTIAL_METADATA_SUFFIX)
        ]
        for name in metadata_names:
            path = os.path.join(directory, name)
            try:
                with open(path, "r", encoding="utf-8") as stream:
                    metadata = json.load(stream)
                cls._validate_metadata(metadata, require_complete=False)
            except (OSError, ValueError, TypeError, _PlaybackCacheError):
                continue
            token = metadata.get("cache_id")
            if not isinstance(token, str) or not cls._TOKEN_PATTERN.match(token):
                continue
            if name not in (
                    CACHE_FILE_PREFIX + token + CACHE_METADATA_SUFFIX,
                    CACHE_FILE_PREFIX + token + CACHE_PARTIAL_METADATA_SUFFIX):
                continue
            base = os.path.join(directory, CACHE_FILE_PREFIX + token)
            owned_paths = [
                base + CACHE_METADATA_SUFFIX,
                base + CACHE_FRAMES_SUFFIX,
                base + CACHE_PARTIAL_METADATA_SUFFIX,
                base + CACHE_PARTIAL_FRAMES_SUFFIX,
            ]
            existing = [candidate for candidate in owned_paths if os.path.exists(candidate)]
            if not existing:
                continue
            try:
                newest = max(os.path.getmtime(candidate) for candidate in existing)
            except OSError:
                continue
            if newest >= cutoff:
                continue
            for candidate in existing:
                try:
                    os.remove(candidate)
                    removed.append(candidate)
                except OSError:
                    pass
        return removed


class _MayaTimeline(object):
    """Small adapter around Maya's current frame and Playback Range APIs."""

    def current_frame(self):
        _require_maya()
        return cmds.currentTime(query=True)

    def playback_range(self):
        _require_maya()
        return (
            cmds.playbackOptions(query=True, min=True),
            cmds.playbackOptions(query=True, max=True),
        )

    def is_playing(self):
        return _maya_is_playing()

    def stop(self):
        if self.is_playing():
            cmds.play(state=False)

    def set_frame(self, frame):
        _require_maya()
        cmds.currentTime(frame, edit=True)


class _CachedPlaybackCacheSummary(object):
    __slots__ = ("_capture_start", "_capture_end", "_frame_count", "_scene_fps",
                 "_capture_time")

    def __init__(self, cache):
        self._capture_start, self._capture_end = cache.capture_range
        self._frame_count = cache.frame_count
        self._scene_fps = cache.scene_fps
        self._capture_time = cache.capture_time

    def __setattr__(self, name, value):
        if hasattr(self, name):
            raise AttributeError("Cached Playback cache summaries are immutable")
        object.__setattr__(self, name, value)

    capture_start = property(lambda self: self._capture_start)
    capture_end = property(lambda self: self._capture_end)
    capture_range = property(lambda self: (self._capture_start, self._capture_end))
    frame_count = property(lambda self: self._frame_count)
    scene_fps = property(lambda self: self._scene_fps)
    capture_time = property(lambda self: self._capture_time)

    def _values(self):
        return (self._capture_start, self._capture_end, self._frame_count,
                self._scene_fps, self._capture_time)

    def __eq__(self, other):
        return (isinstance(other, _CachedPlaybackCacheSummary)
                and self._values() == other._values())

    def __ne__(self, other):
        return not self == other


class _CachedPlaybackView(object):
    __slots__ = (
        "_state", "_current", "_total", "_cache_summary", "_diagnostic",
        "_can_capture", "_can_replay", "_can_stop", "_can_cancel", "_can_leave",
    )

    def __init__(self, state, current=0, total=0, cache_summary=None, diagnostic=None,
                 can_capture=False, can_replay=False, can_stop=False,
                 can_cancel=False, can_leave=False):
        self._state = state
        self._current = int(current or 0)
        self._total = int(total or 0)
        self._cache_summary = cache_summary
        self._diagnostic = _copy_session_payload(diagnostic) if diagnostic else None
        self._can_capture = bool(can_capture)
        self._can_replay = bool(can_replay)
        self._can_stop = bool(can_stop)
        self._can_cancel = bool(can_cancel)
        self._can_leave = bool(can_leave)

    def __setattr__(self, name, value):
        if hasattr(self, name):
            raise AttributeError("Cached Playback views are immutable")
        object.__setattr__(self, name, value)

    state = property(lambda self: self._state)
    current = property(lambda self: self._current)
    total = property(lambda self: self._total)
    cache_summary = property(lambda self: self._cache_summary)
    diagnostic = property(
        lambda self: _copy_session_payload(self._diagnostic) if self._diagnostic else None)
    can_capture = property(lambda self: self._can_capture)
    can_replay = property(lambda self: self._can_replay)
    can_stop = property(lambda self: self._can_stop)
    can_cancel = property(lambda self: self._can_cancel)
    can_leave = property(lambda self: self._can_leave)

    def _values(self):
        diagnostic = tuple(sorted((self._diagnostic or {}).items()))
        return (self._state, self._current, self._total, self._cache_summary,
                diagnostic, self._can_capture, self._can_replay, self._can_stop,
                self._can_cancel, self._can_leave)

    def __eq__(self, other):
        return isinstance(other, _CachedPlaybackView) and self._values() == other._values()

    def __ne__(self, other):
        return not self == other


class _CachedPlaybackError(RuntimeError):
    def __init__(self, code, message="", details=""):
        self._code = code
        self._message = message or code
        self._details = details or self._message
        super(_CachedPlaybackError, self).__init__(self._message)

    @property
    def code(self):
        return self._code

    @property
    def message(self):
        return self._message

    @property
    def details(self):
        return self._details


class _CachedPlaybackRetention(object):
    __slots__ = ("__cache",)

    def __init__(self, cache):
        self.__cache = cache if cache is not None and cache.completed else None

    def _consume(self, revision):
        cache, self.__cache = self.__cache, None
        if cache is None:
            return None
        try:
            if not cache.compatible_with(revision):
                cache.delete()
                return None
        except (_PlaybackCacheError, AttributeError, TypeError, ValueError):
            cache.delete()
            return None
        return cache

    def _discard(self):
        cache, self.__cache = self.__cache, None
        if cache is not None:
            cache.delete()

    def _summary(self):
        return (_CachedPlaybackCacheSummary(self.__cache)
                if self.__cache is not None else None)


class _CacheUploadResult(object):
    """One bounded step of a single cache upload.

    ``processed`` is True when this call moved or measured frames.
    ``action`` is None for a plain step (optionally
    carrying ``progress`` as a ``(current, total)`` pair for the owner's view)
    or one of ``_CacheUpload.CORRUPT`` / ``TRANSPORT`` / ``FAILED`` carrying
    the terminal ``detail`` for the owner to route.
    """

    __slots__ = ("processed", "action", "detail", "progress")

    def __init__(self, processed, action=None, detail=None, progress=None):
        self.processed = processed
        self.action = action
        self.detail = detail
        self.progress = progress


class _CacheUpload(object):
    """Owns one upload pass inside the directly runnable Maya module.

    Single owner of the exact byte declaration, the bounded chunk pump, the
    frames-file iterator lifetime, the backpressure wait, and the
    post-drain Ready deadline. The owning ``_CachedPlayback`` keeps cache
    retention, upload/play ID allocation, Ready identity verification, and
    the user-facing capture/replay/detach states; it only routes the
    returned ``_CacheUploadResult`` to its existing terminal paths.
    """

    CORRUPT = "corrupt"
    TRANSPORT = "transport"
    FAILED = "failed"

    def __init__(self, submit, is_drained):
        self._submit = submit
        self._is_drained = is_drained
        self._cache = None
        self._upload_id = None
        self._revision = None
        self._declared_bytes = 0
        self._declaring = False
        self._frame_iter = None
        self._submitted = 0
        self._end_sent = False
        self._deadline = None

    @property
    def end_sent(self):
        return self._end_sent

    def start(self, cache, identity, declared_hint_bytes=0):
        """Begin one upload: declare bytes, send cache_begin, open the pass."""
        self._cache = cache
        self._upload_id, self._revision = identity
        declared = int(declared_hint_bytes or 0)
        if declared <= 0 and cache.frame_count:
            # Adopted cache from another session instance: derive the exact
            # encoded total inside bounded poller chunks so the declaring
            # pass never blocks Maya's thread on a whole-cache encode.
            self._declaring = True
            declared = 0
        else:
            self._declaring = False
        self._declared_bytes = declared
        self._submitted = 0
        self._end_sent = False
        self._deadline = None
        if not self._declaring:
            outcome = self._send_begin()
            if outcome is not None:
                return outcome
        opened = self._open_iter()
        if opened is not None:
            return opened
        return _CacheUploadResult(False)

    def advance(self):
        """Push at most one bounded chunk; Maya's thread never stalls."""
        if (not self._declaring and self._end_sent
                and self._is_drained() and self._deadline is None):
            self._deadline = time.time() + UPLOAD_READY_TIMEOUT_SECONDS
        if self._deadline is not None and time.time() > self._deadline:
            return _CacheUploadResult(False, self.TRANSPORT, make_diagnostic(
                "STREAM_INTERRUPTED",
                "Timed out waiting for Unreal to accept the uploaded cache."))
        if self._declaring:
            return self._pump_declaration()
        return self._pump_frames()

    def close(self):
        iterator, self._frame_iter = self._frame_iter, None
        if iterator is None:
            return
        close = getattr(iterator, "close", None)
        if close is not None:
            try:
                close()
            except (RuntimeError, TypeError):
                pass

    def _send_begin(self):
        cache = self._cache
        start_frame, end_frame = cache.capture_range
        try:
            begin_message = make_cache_begin_message(
                self._revision, start_frame, end_frame,
                cache.scene_fps, self._declared_bytes, self._upload_id)
        except ValueError as exc:
            return _CacheUploadResult(False, self.FAILED, make_diagnostic(
                "CACHED_UPLOAD_FAILED",
                "The captured cache exceeds the Unreal transient cache limit.",
                details=str(exc)))
        try:
            self._submit(begin_message)
        except (_CachedPlaybackError, _StreamingSessionError) as error:
            return _CacheUploadResult(False, self.TRANSPORT, error)
        return None

    def _open_iter(self):
        try:
            self._frame_iter = self._cache.iter_frames()
        except (_PlaybackCacheError, OSError) as error:
            self.close()
            return _CacheUploadResult(False, self.CORRUPT, error)
        return None

    def _pump_frames(self):
        """Push at most one bounded chunk of cached frames per call."""
        if self._frame_iter is None or not self._is_drained():
            return _CacheUploadResult(False)  # previous chunk still draining
        total_frames = self._cache.frame_count
        submitted = 0
        progress = None
        while submitted < UPLOAD_CHUNK_FRAMES:
            try:
                frame = next(self._frame_iter)
            except StopIteration:
                # Release the frames-file handle before declaring wire done.
                self.close()
                if not self._end_sent:
                    try:
                        self._submit(make_cache_end_message())
                    except (_CachedPlaybackError,
                            _StreamingSessionError) as error:
                        return _CacheUploadResult(False, self.TRANSPORT, error)
                    self._end_sent = True
                break
            except (_PlaybackCacheError, UnicodeDecodeError, OSError,
                    TypeError, ValueError) as error:
                # Any decode or iteration failure is deterministic cache
                # corruption: invalidate the whole cache instead of
                # masquerading as a transport failure.
                self.close()
                return _CacheUploadResult(submitted > 0, self.CORRUPT, error)
            try:
                message = make_cache_frame_message(
                    self._submitted,
                    frame["transforms"], frame["curves"])
            except (TypeError, ValueError) as error:
                # A cached frame that cannot be re-encoded is deterministic
                # cache corruption, never a transport failure.
                self.close()
                return _CacheUploadResult(submitted > 0, self.CORRUPT, error)
            try:
                self._submit(message)
            except (_CachedPlaybackError,
                    _StreamingSessionError) as error:
                return _CacheUploadResult(False, self.TRANSPORT, error)
            self._submitted += 1
            submitted += 1
            if (self._submitted % UPLOAD_CHUNK_FRAMES == 0
                    and self._submitted < total_frames):
                progress = (self._submitted, total_frames)
        return _CacheUploadResult(submitted > 0, progress=progress)

    def _pump_declaration(self):
        """Measure at most one bounded chunk of cached frames per call.

        Adopted caches have no captured wire sizes, so the exact encoded
        declaration is derived here — in bounded poller chunks instead of one
        blocking whole-cache pass.
        """
        if self._frame_iter is None:
            return _CacheUploadResult(False)
        measured = 0
        while measured < UPLOAD_CHUNK_FRAMES:
            try:
                frame = next(self._frame_iter)
            except StopIteration:
                self.close()
                self._declaring = False
                begin_outcome = self._send_begin()
                if begin_outcome is not None:
                    return _CacheUploadResult(
                        measured > 0, begin_outcome.action, begin_outcome.detail)
                # Reopen the frames file for the pipelined submission pass;
                # indices and progress restart from zero for the transfer.
                reopened = self._open_iter()
                if reopened is not None:
                    return _CacheUploadResult(
                        measured > 0, reopened.action, reopened.detail)
                self._end_sent = False
                self._submitted = 0
                break
            except (_PlaybackCacheError, UnicodeDecodeError, OSError,
                    TypeError, ValueError) as error:
                self.close()
                return _CacheUploadResult(measured > 0, self.CORRUPT, error)
            try:
                frame_bytes = cache_frame_wire_size(
                    self._submitted,
                    frame["transforms"], frame["curves"])
            except (TypeError, ValueError) as error:
                self.close()
                return _CacheUploadResult(measured > 0, self.CORRUPT, error)
            # Overflow-safe accumulation against the frozen wire limit; an
            # adopted cache larger than the limit fails fast, exactly like a
            # captured one would at declaration time.
            if (frame_bytes < 0
                    or self._declared_bytes
                    > MAX_CACHE_PAYLOAD_BYTES - frame_bytes):
                self.close()
                self._declaring = False
                return _CacheUploadResult(measured > 0, self.FAILED,
                    make_diagnostic(
                        "CACHED_UPLOAD_FAILED",
                        "The captured cache exceeds the Unreal transient cache"
                        " limit.",
                        details="declared {0} bytes plus frame {1} exceeds the"
                                " limit of {2} bytes".format(
                                    self._declared_bytes,
                                    self._submitted,
                                    MAX_CACHE_PAYLOAD_BYTES)))
            self._declared_bytes += frame_bytes
            self._submitted += 1
            measured += 1
        return _CacheUploadResult(measured > 0)


class _CachedPlayback(object):
    """Owns cached capture, upload orchestration, and Unreal-driven replay control.

    Capture stores every inclusive Playback Range display frame in the
    temporary disk cache. Completion uploads the whole cache without a
    real-time deadline, waits for Unreal's Ready reply, then only sends
    playback controls: Unreal applies each buffered frame exactly once, in
    order, on its own monotonic clock.
    """

    REALTIME = "REALTIME"
    CACHED_IDLE = "CACHED_IDLE"
    CAPTURING = "CAPTURING"
    UPLOADING = "UPLOADING"
    REPLAYING = "REPLAYING"
    STOPPING = "STOPPING"
    COMPLETED = "COMPLETED"
    STOPPED = "STOPPED"
    FAILED = "FAILED"
    DETACHED = "DETACHED"

    def __init__(self, scene, streaming_session, on_change=None, timeline=None,
                 cache_factory=None, temp_dir=None, disk_usage=None,
                 timer_api=None, retention=None):
        self._scene = scene
        self._streaming_session = streaming_session
        self._on_change = on_change
        self._timeline = timeline or _MayaTimeline()
        self._cache_factory = cache_factory or _PlaybackCache
        self._temp_dir = os.fspath(temp_dir or tempfile.gettempdir())
        self._disk_usage = disk_usage or shutil.disk_usage
        self._timer_api = timer_api if timer_api is not None else getattr(om, "MTimerMessage", None)
        self._cache = None
        self._phase = "idle"
        self._paused_streaming = False
        self._capture_cache = None
        self._capture_cancel_requested = False
        self._capture_generation = 0
        self._capture_timer_id = None
        self._capture_revision = None
        self._capture_start = None
        self._capture_end = None
        self._capture_next_frame = None
        self._capture_total_frames = None
        self._capture_original_frame = None
        self._capture_confirmation_callback = None
        self._capture_estimated_size = None
        self._capture_encoded_bytes = 0
        self._capture_large_cache_confirmed = False
        self._replies = queue.Queue()
        self._poller_generation = 0
        self._poller_timer_id = None
        self._uploaded_revision = None
        self._applied_frames = None
        self._upload_id = 0
        self._play_id = 0
        self._active_upload_id = None
        self._active_upload_revision = None
        self._active_play_id = None
        self._upload = None
        self._closed = False
        self._progress_current = 0
        self._progress_total = 0
        self._issued_retention = None
        revision = self._ensure_ready()
        if retention is not None:
            self._cache = retention._consume(revision)
        self._last_view = self._make_view()

    def _semantic_state(self):
        if self._closed or self._phase in ("closed", "detached", "transport_failed"):
            return self.DETACHED
        if self._phase == "capturing":
            return self.CAPTURING
        if self._phase in ("captured", "uploading", "ready_to_play"):
            return self.UPLOADING
        if self._phase == "replaying":
            return self.REPLAYING
        if self._phase == "stopping":
            return self.STOPPING
        if self._phase == "completed":
            return self.COMPLETED
        if self._phase == "stopped":
            return self.STOPPED
        if self._phase in ("failed", "upload_failed", "playback_failed", "cancelled"):
            # One truthful post-failure transition: a failure that already
            # resumed Real-time Preview and dropped Unreal cache ownership is
            # Real-time state carrying a diagnostic; only a failure that
            # remains cached renders as FAILED.
            return self.FAILED if self._paused_streaming else self.REALTIME
        return self.CACHED_IDLE if self._paused_streaming else self.REALTIME

    def _cache_summary(self):
        if self._cache is not None and self._cache.completed:
            return _CachedPlaybackCacheSummary(self._cache)
        if self._issued_retention is not None:
            return self._issued_retention._summary()
        return None

    def _make_view(self, diagnostic=None):
        state = self._semantic_state()
        session = self._streaming_session
        attached = (not self._closed and session is not None and session.is_ready)
        entered = attached and self._paused_streaming
        summary = self._cache_summary()
        return _CachedPlaybackView(
            state, self._progress_current, self._progress_total, summary, diagnostic,
            can_capture=(entered
                         and state not in (self.CAPTURING, self.STOPPING, self.DETACHED)),
            can_replay=(entered and summary is not None
                        and state not in (self.CAPTURING, self.UPLOADING,
                                          self.REPLAYING, self.STOPPING)),
            can_stop=state in (self.REPLAYING, self.STOPPING),
            can_cancel=state == self.CAPTURING,
            can_leave=entered and state not in (self.CAPTURING, self.DETACHED),
        )

    @property
    def view(self):
        return self._make_view()

    def _publish(self, current=None, total=None, diagnostic=None):
        if current is not None:
            self._progress_current = current
        if total is not None:
            self._progress_total = total
        view = self._make_view(diagnostic)
        if view == self._last_view:
            return
        self._last_view = self._make_view()
        if self._on_change is None:
            return
        try:
            self._on_change(view)
        except Exception as exc:
            if cmds is not None:
                try:
                    cmds.warning("MtoU_LiveLink cached playback callback failed: {0}".format(exc))
                except Exception:
                    pass

    def _current_revision(self):
        try:
            return int(self._scene.snapshot().revision)
        except (AttributeError, RuntimeError, TypeError, ValueError):
            session = self._streaming_session
            revision = session.revision if session is not None else None
            return None if revision is None else int(revision)

    def _ensure_ready(self):
        session = self._streaming_session
        if self._closed or session is None or not session.is_ready:
            raise _CachedPlaybackError(
                "CACHED_PLAYBACK_NOT_READY",
                "Cached Playback requires a ready negotiated connection.")
        expected = session.revision
        current = self._current_revision()
        if expected is not None and current is not None and expected != current:
            raise _CachedPlaybackError(
                "CACHED_PLAYBACK_INCOMPATIBLE",
                "The character snapshot changed during cached playback.",
                "streaming revision {0}, current revision {1}".format(expected, current))
        return expected if expected is not None else current

    def _pause_streaming(self):
        if self._paused_streaming:
            return
        if self._streaming_session is not None:
            self._streaming_session.pause_for_cached(self._replies.put)
        self._paused_streaming = True

    def _resume_streaming(self):
        if not self._paused_streaming:
            return
        if self._streaming_session is not None:
            self._streaming_session.resume_from_cached()
        self._paused_streaming = False

    def enter(self):
        self._ensure_ready()
        if self._paused_streaming:
            return self
        self._pause_streaming()
        self._phase = "idle"
        self._send_enter_best_effort()
        # Every Cached Playback state that owns the negotiated session keeps
        # the lightweight transport observer running, so a disconnected
        # socket is detected while idle instead of surviving until the next
        # user action.
        self._add_playback_poller()
        self._publish(current=0, total=0)
        return self

    def _send_enter_best_effort(self):
        """Establish cached ownership in Unreal before any capture begins."""
        session = self._streaming_session
        if session is None or not session.is_ready:
            return
        try:
            session.submit_cached(make_cache_enter_message())
        except (_CachedPlaybackError, _StreamingSessionError,
                RuntimeError, TypeError, ValueError) as exc:
            if cmds is not None:
                try:
                    cmds.warning(
                        "MtoU_LiveLink could not send the cache entry"
                        " control: {0}".format(exc))
                except Exception:
                    pass

    def leave(self):
        if self._closed or not self._paused_streaming:
            return
        if self._phase == "capturing":
            self._capture_failure(_CachedPlaybackError(
                "CACHED_PLAYBACK_CANCELLED", "Cached capture was cancelled."))
        self.stop_replay()
        # Queue the Unreal cache clear while ordered delivery is active, then
        # resume streaming so live frames follow the clear in order.
        self._teardown_cached_runtime(send_clear=True, resume_streaming=True)
        self._phase = "idle"
        self._publish(current=0, total=0)

    def cancel_capture(self):
        if self._phase == "capturing":
            self._capture_cancel_requested = True

    def _disk_free_bytes(self):
        try:
            usage = self._disk_usage(self._temp_dir)
            if hasattr(usage, "free"):
                return int(usage.free)
            if isinstance(usage, (tuple, list)) and len(usage) >= 3:
                return int(usage[2])
            return int(usage)
        except (OSError, TypeError, ValueError) as exc:
            raise _CachedPlaybackError(
                "CACHED_PLAYBACK_SPACE",
                "Unable to determine available temporary-disk space.",
                details=str(exc))

    @staticmethod
    def _invoke_confirmation(callback, estimated_size, total_frames):
        if callback is None:
            return True
        try:
            return bool(callback(estimated_size, total_frames))
        except TypeError:
            return bool(callback(estimated_size))

    def _make_cache(self, revision, capture_start, capture_end, scene_fps, capture_time):
        begin = getattr(self._cache_factory, "begin", None)
        if begin is None:
            raise _PlaybackCacheError("cache factory must provide begin()")
        return begin(
            snapshot_revision=revision,
            capture_start=capture_start,
            capture_end=capture_end,
            scene_fps=scene_fps,
            temp_dir=self._temp_dir,
            capture_time=capture_time,
        )


    def _capture_timer_interval(self):
        return 0.001

    def _add_capture_timer(self):
        if self._timer_api is None or not hasattr(self._timer_api, "addTimerCallback"):
            return
        self._capture_generation += 1
        generation = self._capture_generation

        def on_timer(*args):
            del args
            if generation != self._capture_generation:
                return
            self._capture_step()

        self._capture_timer_id = self._timer_api.addTimerCallback(
            self._capture_timer_interval(), on_timer)

    def _remove_capture_timer(self):
        self._capture_generation += 1
        timer_id, self._capture_timer_id = self._capture_timer_id, None
        self._remove_timer_callback(timer_id)

    def _remove_timer_callback(self, timer_id):
        if timer_id is None:
            return
        remove = getattr(self._timer_api, "removeCallback", None)
        if remove is None:
            remove = getattr(getattr(om, "MMessage", None), "removeCallback", None)
        if remove is not None:
            try:
                remove(timer_id)
            except RuntimeError:
                pass

    def _restore_capture_frame(self):
        original_frame = self._capture_original_frame
        if original_frame is None:
            return
        try:
            self._timeline.set_frame(original_frame)
        except (AttributeError, RuntimeError, TypeError, ValueError):
            pass
        self._capture_original_frame = None

    def capture(self, scene_fps, confirm_large_cache=None):
        if self._phase == "capturing":
            return self
        revision = self._ensure_ready()
        start_frame, end_frame = self._timeline.playback_range()
        start_frame = int(round(float(start_frame)))
        end_frame = int(round(float(end_frame)))
        if end_frame < start_frame:
            raise _CachedPlaybackError(
                "CACHED_PLAYBACK_CAPTURE_RANGE",
                "Maya Playback Range must end at or after its start.")
        if end_frame - start_frame + 1 > MAX_CACHE_FRAME_COUNT:
            # Enforce the frozen frame limit before any capture work: a range
            # that Unreal must reject never costs time or disk.
            raise _CachedPlaybackError(
                "CACHED_PLAYBACK_FRAME_LIMIT",
                "The Playback Range spans {0} frames; the fixed cache limit"
                " is {1} frames.".format(
                    end_frame - start_frame + 1, MAX_CACHE_FRAME_COUNT),
                details="start {0}, end {1}, limit {2}".format(
                    start_frame, end_frame, MAX_CACHE_FRAME_COUNT))
        scene_fps = validate_frame_rate(scene_fps)
        self.enter()
        if self._phase in ("replaying", "stopping"):
            self.stop_replay()
        self._remove_playback_poller()
        if self._phase == "uploading":
            # Abort the in-flight transfer; the fresh cache_begin below resets
            # Unreal's buffer coherently, so stale frames cannot mix.
            self._discard_upload()
        if self._cache is not None:
            self._cache.delete()
            self._cache = None
        original_frame = self._timeline.current_frame()
        self._capture_original_frame = original_frame
        if self._timeline.is_playing():
            self._timeline.stop()
        total_frames = end_frame - start_frame + 1
        try:
            cache = self._make_cache(
                revision, start_frame, end_frame, scene_fps, time.time())
        except (_PlaybackCacheError, OSError, TypeError, ValueError) as exc:
            self._restore_capture_frame()
            self._resume_streaming()
            raise _CachedPlaybackError(
                "CACHED_PLAYBACK_SPACE", str(exc), details=str(exc))
        self._capture_cache = cache
        self._capture_revision = revision
        self._capture_start = start_frame
        self._capture_end = end_frame
        self._capture_next_frame = start_frame
        self._capture_total_frames = total_frames
        self._capture_original_frame = original_frame
        self._capture_confirmation_callback = confirm_large_cache
        self._capture_estimated_size = None
        self._capture_encoded_bytes = 0
        self._capture_large_cache_confirmed = False
        self._capture_cancel_requested = False
        self._phase = "capturing"
        self._publish(current=0, total=total_frames)
        try:
            self._add_capture_timer()
        except (RuntimeError, TypeError, ValueError) as exc:
            error = _CachedPlaybackError(
                "INTERNAL_ERROR", str(exc), details=str(exc))
            failure = self._capture_failure(error)
            if failure is not None:
                raise failure
        return self

    def _teardown_cached_runtime(self, send_clear=False, resume_streaming=False):
        """Single terminal cleanup seam for every cached-mode exit.

        Removes both timers, closes any upload iterator, optionally queues an
        ordered ``cache_clear`` while ordered delivery is still active, and
        only then resumes live streaming so the clear stays ahead of the
        first resumed live pose.
        """
        self._remove_capture_timer()
        self._remove_playback_poller()
        self._discard_upload()
        if send_clear:
            self._send_clear_best_effort()
        if resume_streaming:
            self._resume_streaming()

    def _capture_failure(self, error, transport_lost=False):
        # Timers and iterators die first so no further capture work fires.
        self._teardown_cached_runtime()
        if self._capture_cache is not None:
            self._capture_cache.delete()
        self._capture_cache = None
        self._restore_capture_frame()
        diagnostic = make_diagnostic(
            error.code if error.code in DIAGNOSTICS else "INTERNAL_ERROR",
            error.message, details=error.details)
        if transport_lost:
            self._phase = "transport_failed"
            self._publish(current=0, total=0, diagnostic=diagnostic)
            if self._streaming_session is not None:
                try:
                    self._streaming_session.stop()
                except (RuntimeError, TypeError):
                    pass
            return error
        self._phase = "cancelled" if error.code == "CACHED_PLAYBACK_CANCELLED" else "failed"
        # A failed capture leaves Unreal owning cached entry state; drop it in
        # order before live poses flow again. Resuming Real-time Preview here
        # and clearing Unreal ownership is the complete, truthful transition:
        # the published view renders as REALTIME with this diagnostic.
        self._send_clear_best_effort()
        self._resume_streaming()
        self._publish(current=0, total=0, diagnostic=diagnostic)
        if error.code == "CACHED_PLAYBACK_CANCELLED":
            return None
        return error

    def _capture_step(self):
        if self._phase != "capturing":
            return False
        try:
            session = self._streaming_session
            if session is None or not session.is_ready:
                self._handle_transport_failure(_CachedPlaybackError(
                    "STREAM_INTERRUPTED",
                    "The streaming connection ended during capture."))
                return True
            if self._capture_cancel_requested:
                error = _CachedPlaybackError(
                    "CACHED_PLAYBACK_CANCELLED", "Cached capture was cancelled.")
                self._capture_failure(error)
                return True
            frame_number = self._capture_next_frame
            self._timeline.set_frame(frame_number)
            frame = self._scene.sample()
            if int(frame.revision) != int(self._capture_revision):
                raise _CachedPlaybackError(
                    "CACHED_PLAYBACK_INCOMPATIBLE",
                    "The character snapshot changed during capture.",
                    "captured revision {0}, expected {1}".format(
                        frame.revision, self._capture_revision))
            frame_message = make_frame_message(
                list(frame.transforms), list(frame.curves))
            frame_size = _PlaybackCache.serialized_frame_size(frame_message)
            # Exact wire size of this frame's cache_frame message (real index
            # width included, no file newline) accumulated for the upload
            # declaration so it matches what Unreal meters on the socket.
            encoded_size = cache_frame_wire_size(
                frame_number - self._capture_start,
                frame_message["transforms"],
                frame_message["curves"])
            if (encoded_size < 0
                    or encoded_size > MAX_MESSAGE_BYTES
                    or self._capture_encoded_bytes + encoded_size
                    > MAX_CACHE_PAYLOAD_BYTES):
                # Stop as soon as a single frame would cross the per-message
                # framing ceiling or the encoded total would cross the frozen
                # 1 GiB budget; the partial owned cache is deleted below.
                raise _CachedPlaybackError(
                    "CACHED_PLAYBACK_PAYLOAD_LIMIT",
                    "Captured frames would exceed the fixed cache limits.",
                    details="frame {0} bytes, encoded total {1} + {2} bytes"
                            " against limits {3} per message and {4} per"
                            " cache".format(
                                encoded_size, self._capture_encoded_bytes,
                                encoded_size, MAX_MESSAGE_BYTES,
                                MAX_CACHE_PAYLOAD_BYTES))
            self._capture_encoded_bytes += encoded_size
            if self._capture_estimated_size is None:
                self._capture_estimated_size = frame_size
            else:
                self._capture_estimated_size = max(self._capture_estimated_size, frame_size)
            estimated_total = self._capture_estimated_size * self._capture_total_frames
            frames_written = frame_number - self._capture_start
            remaining_estimate = self._capture_estimated_size * (
                self._capture_total_frames - frames_written)
            free_bytes = self._disk_free_bytes()
            if remaining_estimate > free_bytes:
                raise _CachedPlaybackError(
                    "CACHED_PLAYBACK_SPACE",
                    "The temporary directory does not have enough free space.",
                    "estimated remaining {0} bytes, free {1} bytes".format(
                        remaining_estimate, free_bytes))
            if (estimated_total > CACHE_CONFIRMATION_BYTES
                    and not self._capture_large_cache_confirmed):
                if not self._invoke_confirmation(
                        self._capture_confirmation_callback,
                        estimated_total, self._capture_total_frames):
                    raise _CachedPlaybackError(
                        "CACHED_PLAYBACK_CANCELLED",
                        "Large cache confirmation was declined.")
                self._capture_large_cache_confirmed = True
            self._capture_cache.append(frame_message)
            offset = frame_number - self._capture_start + 1
            self._publish(current=offset, total=self._capture_total_frames)
            if self._capture_cancel_requested:
                error = _CachedPlaybackError(
                    "CACHED_PLAYBACK_CANCELLED", "Cached capture was cancelled.")
                self._capture_failure(error)
                return True
            self._capture_next_frame += 1
            if frame_number < self._capture_end:
                return True
            cache = self._capture_cache
            cache.finalize()
            self._remove_capture_timer()
            self._capture_cache = None
            self._cache = cache
            self._restore_capture_frame()
            self._publish(current=self._capture_total_frames,
                          total=self._capture_total_frames)
            self._phase = "captured"
            self._begin_upload()
            return True
        except _CachedPlaybackError as error:
            self._capture_failure(error)
            return True
        except (_CharacterSceneError, _PlaybackCacheError, RuntimeError, ValueError) as exc:
            error = _CachedPlaybackError(
                "SAMPLING_FAILED", str(exc), details=str(exc))
            self._capture_failure(error)
            return True

    def _submit_cached(self, message):
        if self._streaming_session is None:
            raise _CachedPlaybackError(
                "CACHED_PLAYBACK_NOT_READY",
                "The streaming session cannot submit cached messages.")
        self._streaming_session.submit_cached(message)

    def _delivery_drained(self):
        session = self._streaming_session
        return True if session is None else session.cached_delivery_drained()

    def _accept_cache_ready(self, reply):
        if self._phase != "uploading" or self._cache is None:
            return
        if _exact_int(reply.get("upload_id")) != self._active_upload_id:
            return  # stale outcome from an older upload
        upload = self._upload
        if upload is None or not upload.end_sent or not self._delivery_drained():
            # Unreal cannot truthfully report Ready before cache_end was sent
            # and drained. Put it back and stop draining this round so the
            # same outcome is not re-consumed in a tight loop.
            self._replies.put(reply)
            self._stop_draining_this_round = True
            return
        total_frames = self._cache.frame_count
        echoed_count = _exact_int(reply.get("frame_count"))
        echoed_revision = _exact_int(reply.get("revision"))
        if (echoed_count != total_frames
                or echoed_revision != self._active_upload_revision):
            self._fail_upload(make_diagnostic(
                "CACHED_UPLOAD_FAILED",
                "Unreal reported inconsistent cache evidence.",
                details="ready frame_count={0} revision={1}, expected"
                        " frame_count={2} revision={3}".format(
                            echoed_count, echoed_revision,
                            total_frames, self._active_upload_revision)))
            return
        self._remove_playback_poller()
        self._uploaded_revision = self._cache.snapshot_revision
        self._phase = "ready_to_play"
        self._publish(current=total_frames, total=total_frames)
        self._request_play()

    def _route_outcome(self, reply):
        """Identity-aware routing: late outcomes from older operations are
        dropped without touching current state."""
        reply_type = reply.get("type")
        if reply_type == "error":
            play_id = _exact_int(reply.get("play_id"))
            upload_id = _exact_int(reply.get("upload_id"))
            if self._phase == "uploading":
                if upload_id == self._active_upload_id:
                    self._handle_error_reply(reply)
            elif self._phase in ("ready_to_play", "replaying", "stopping"):
                if play_id == self._active_play_id:
                    self._handle_error_reply(reply)
            return
        if reply_type == "cache_progress":
            play_id = _exact_int(reply.get("play_id"))
            applied = _exact_int(reply.get("applied"))
            if self._phase == "replaying" and play_id == self._active_play_id:
                # Informational only; never completion evidence. Throttled to
                # roughly 2% steps so the UI is not flooded at scene rate.
                total_frames = self._cache.frame_count if self._cache else None
                if applied is None or (
                        total_frames is not None and not 0 <= applied <= total_frames):
                    return
                step = max(1, round(total_frames / 50)) if total_frames else 1
                if total_frames is None or applied % step == 0 \
                        or applied >= total_frames:
                    self._publish(current=applied, total=total_frames)
            return
        if reply_type == "cache_complete":
            play_id = _exact_int(reply.get("play_id"))
            if self._phase != "replaying" or play_id != self._active_play_id:
                return
            total_frames = self._cache.frame_count if self._cache else None
            applied = _exact_int(reply.get("applied_frame_count"))
            elapsed = _finite_float(reply.get("elapsed_seconds"))
            if (total_frames is None or applied != total_frames
                    or elapsed is None or elapsed <= 0.0):
                self._phase = "failed"
                self._publish(diagnostic=make_diagnostic(
                    "INTERNAL_ERROR",
                    "Unreal reported inconsistent playback evidence.",
                    details="completion applied_frame_count={0}"
                            " elapsed_seconds={1}, expected {2} applied"
                            " frames".format(applied, elapsed, total_frames)))
                return
            self._applied_frames = applied
            self._phase = "completed"
            self._publish(current=applied, total=total_frames)
            return
        if reply_type == "cache_stopped":
            play_id = _exact_int(reply.get("play_id"))
            if self._phase == "stopping" and play_id == self._active_play_id:
                # Success is reported only once Unreal acknowledges this
                # exact attempt; a late ack for an older attempt is dropped.
                # The poller keeps running so completed and stopped states
                # still observe transport termination.
                self._phase = "stopped"
                self._applied_frames = None
                self._publish(current=0,
                              total=self._cache.frame_count if self._cache else 0)
            return
        if reply_type == "cache_ready":
            # A late duplicate Ready can only belong to an older upload;
            # the current upload consumes its Ready synchronously.
            if self._phase == "uploading":
                self._accept_cache_ready(reply)
            return
        # cache_cleared and any unknown well-formed outcome are informational.

    def _handle_error_reply(self, reply):
        code = str(reply.get("code") or "")
        message = str(reply.get("message") or code)
        details = str(reply.get("details") or message)
        if self._phase == "uploading":
            self._fail_upload(make_diagnostic(
                "CACHED_UPLOAD_FAILED", message, details=details))
            return
        if self._phase in ("replaying", "stopping"):
            # Runtime playback failures keep the negotiated cached session:
            # the poller stays running so this state also observes transport
            # termination, and leave/retry remain available capabilities.
            if code == "CACHED_PLAYBACK_PERFORMANCE":
                self._phase = "playback_failed"
                self._publish(diagnostic=make_diagnostic(
                    "CACHED_PLAYBACK_PERFORMANCE", message, details=details))
                return
            mapped = {
                "CACHE_NOT_READY": "CACHED_PLAYBACK_NO_CACHE",
                "CACHE_REVISION_MISMATCH": "CACHED_PLAYBACK_INCOMPATIBLE",
            }.get(code, "INTERNAL_ERROR")
            self._phase = "failed"
            self._publish(diagnostic=make_diagnostic(mapped, message, details=details))

    def _fail_upload(self, diagnostic):
        """Terminal path for an asynchronously rejected upload.

        Removes the poller and upload transfer, drops Unreal's cache
        ownership in order, and only then resumes the current live pose.
        """
        self._teardown_cached_runtime(send_clear=True, resume_streaming=True)
        self._phase = "upload_failed"
        self._publish(current=0, total=0, diagnostic=diagnostic)

    def _discard_upload(self):
        """Close the in-flight upload transfer and drop its ownership."""
        upload, self._upload = self._upload, None
        if upload is not None:
            upload.close()

    def _begin_upload(self):
        try:
            revision = self._ensure_ready()
        except _CachedPlaybackError as error:
            if error.code == "CACHED_PLAYBACK_INCOMPATIBLE":
                self._invalidate_incompatible_cache(error)
                return
            raise
        cache = self._cache
        if revision is not None and not cache.compatible_with(revision):
            self._invalidate_incompatible_cache(_CachedPlaybackError(
                "CACHED_PLAYBACK_INCOMPATIBLE",
                "The completed cache does not match the current character snapshot.",
                "streaming revision {0}, cache revision {1}".format(
                    revision, cache.snapshot_revision)))
            return
        self._pause_streaming()
        total_frames = cache.frame_count
        self._upload_id += 1
        self._active_upload_id = self._upload_id
        self._phase = "uploading"
        self._publish(current=0, total=total_frames)
        # Ready arrives asynchronously: Unreal parses and buffers without any
        # real-time deadline while the poller drains the identity-matched
        # outcome, so Maya's thread never blocks on Unreal's cache parsing.
        upload = _CacheUpload(
            submit=self._submit_cached, is_drained=self._delivery_drained)
        outcome = upload.start(
            cache, (self._active_upload_id, revision),
            self._capture_encoded_bytes)
        if outcome.action == _CacheUpload.CORRUPT:
            self._invalidate_corrupt_cache(outcome.detail)
            return
        if outcome.action == _CacheUpload.TRANSPORT:
            self._handle_transport_failure(outcome.detail)
            return
        if outcome.action == _CacheUpload.FAILED:
            self._fail_upload(outcome.detail)
            return
        self._upload = upload
        self._active_upload_revision = revision
        self._add_playback_poller()

    def _can_reuse_upload(self):
        return (
            self._phase in ("ready_to_play", "stopped", "stopping",
                            "completed", "playback_failed")
            and self._uploaded_revision is not None
            and self._cache is not None
            and int(self._uploaded_revision) == self._cache.snapshot_revision)

    def _request_play(self):
        self._play_id += 1
        self._active_play_id = self._play_id
        try:
            self._submit_cached(make_cache_play_message(self._play_id))
        except (_CachedPlaybackError, _StreamingSessionError,
                RuntimeError, TypeError, ValueError) as exc:
            self._handle_transport_failure(exc)
            return
        self._phase = "replaying"
        self._applied_frames = None
        self._publish(current=0, total=self._cache.frame_count)
        self._add_playback_poller()

    def _playback_poll_interval(self):
        return 0.05

    def _add_playback_poller(self):
        if self._timer_api is None or not hasattr(self._timer_api, "addTimerCallback"):
            return
        if self._poller_timer_id is not None:
            return
        self._poller_generation += 1
        generation = self._poller_generation

        def on_timer(*args):
            del args
            if generation != self._poller_generation:
                return
            self._poll()

        self._poller_timer_id = self._timer_api.addTimerCallback(
            self._playback_poll_interval(), on_timer)

    def _remove_playback_poller(self):
        self._poller_generation += 1
        timer_id, self._poller_timer_id = self._poller_timer_id, None
        self._remove_timer_callback(timer_id)

    def _poll(self):
        """Drain outcomes and advance the pipelined cache upload.

        Each call pushes at most one bounded chunk of cached frames (or
        declaration measurements) toward Unreal; Maya's thread never stalls
        for the duration of an upload.
        """
        processed = False
        self._stop_draining_this_round = False
        if self._phase == "uploading":
            session = self._streaming_session
            if session is None or not session.is_ready:
                self._handle_transport_failure(make_diagnostic(
                    "STREAM_INTERRUPTED",
                    "The streaming connection ended during the cache upload."))
                return processed
            upload = self._upload
            if upload is not None:
                result = upload.advance()
                if result.action == _CacheUpload.TRANSPORT:
                    self._handle_transport_failure(result.detail)
                    return processed
                if result.action == _CacheUpload.CORRUPT:
                    self._invalidate_corrupt_cache(result.detail)
                elif result.action == _CacheUpload.FAILED:
                    self._fail_upload(result.detail)
                elif result.progress is not None:
                    self._publish(current=result.progress[0],
                                  total=result.progress[1])
                processed = result.processed or processed
        while not self._stop_draining_this_round:
            try:
                reply = self._replies.get_nowait()
            except queue.Empty:
                break
            self._route_outcome(reply)
            processed = True
        self._stop_draining_this_round = False
        session = self._streaming_session
        if (self._paused_streaming and not self._closed
                and self._phase != "transport_failed"
                and (session is None or not session.is_ready)):
            # Continuous observation in every state that owns the negotiated
            # session: idle, captured, uploading, replaying, completed, or
            # stopped all surface transport termination immediately instead of
            # rendering a stale connected indication until the next action.
            self._handle_transport_failure(make_diagnostic(
                "STREAM_INTERRUPTED", "The streaming connection ended."))
            return processed
        return processed

    def _send_clear_best_effort(self):
        session = self._streaming_session
        if session is None or not session.is_ready:
            return
        try:
            session.submit_cached(make_cache_clear_message())
        except (_CachedPlaybackError, _StreamingSessionError,
                RuntimeError, TypeError, ValueError):
            pass

    def _invalidate_incompatible_cache(self, error):
        self._remove_playback_poller()
        self._discard_upload()
        if self._cache is not None:
            self._cache.delete()
        self._cache = None
        self._uploaded_revision = None
        self._phase = "failed"
        diagnostic = make_diagnostic(
            "CACHED_PLAYBACK_INCOMPATIBLE", error.message, details=error.details)
        # Unreal may still own entered or partially received state; drop it in
        # order before live poses resume.
        self._send_clear_best_effort()
        self._resume_streaming()
        self._publish(current=0, total=0, diagnostic=diagnostic)

    def _invalidate_corrupt_cache(self, error):
        self._remove_playback_poller()
        self._discard_upload()
        if self._cache is not None:
            self._cache.delete()
        self._cache = None
        self._uploaded_revision = None
        self._phase = "failed"
        diagnostic = make_diagnostic(
            "CACHED_PLAYBACK_NO_CACHE",
            "The cached frame file is incomplete or invalid.",
            details=str(error))
        # A mid-upload corruption leaves Unreal holding a partial buffer;
        # clear that ownership in order before streaming resumes.
        self._send_clear_best_effort()
        self._resume_streaming()
        self._publish(current=0, total=0, diagnostic=diagnostic)

    def _handle_transport_failure(self, error):
        if self._closed:
            return
        if not self._paused_streaming:
            return
        if self._phase == "capturing":
            # A capture interrupted by transport loss is a capture failure:
            # it owns the partial-cache cleanup and the clear-before-resume
            # ordering.
            self._capture_failure(_CachedPlaybackError(
                "STREAM_INTERRUPTED",
                "The streaming connection ended during capture."),
                transport_lost=True)
            return
        self._remove_playback_poller()
        self._discard_upload()
        self._phase = "transport_failed"
        diagnostic = error if isinstance(error, dict) else make_diagnostic(
            "STREAM_INTERRUPTED", str(getattr(error, "message", error)),
            details=str(getattr(error, "details", error)))
        self._publish(diagnostic=diagnostic)
        if self._streaming_session is not None:
            try:
                self._streaming_session.stop()
            except (RuntimeError, TypeError):
                pass

    def replay(self):
        """Upload the completed cache if needed, then ask Unreal to play it."""
        try:
            revision = self._ensure_ready()
        except _CachedPlaybackError as error:
            if error.code == "CACHED_PLAYBACK_INCOMPATIBLE":
                self._invalidate_incompatible_cache(error)
            raise
        if self._cache is None or not self._cache.completed:
            raise _CachedPlaybackError(
                "CACHED_PLAYBACK_NO_CACHE", "There is no completed cache to replay.")
        if revision is not None and not self._cache.compatible_with(revision):
            error = _CachedPlaybackError(
                "CACHED_PLAYBACK_INCOMPATIBLE",
                "The completed cache does not match the current character snapshot.")
            self._invalidate_incompatible_cache(error)
            raise error
        self.enter()
        if self._can_reuse_upload():
            self._request_play()
        else:
            self._begin_upload()

    def stop_replay(self):
        if self._phase != "replaying":
            return
        # Stop reports success only after Unreal's identity-matched
        # cache_stopped acknowledgement; the poller keeps draining outcomes.
        self._phase = "stopping"
        try:
            self._submit_cached(make_cache_stop_message())
        except (_CachedPlaybackError, _StreamingSessionError,
                RuntimeError, TypeError, ValueError):
            self._handle_transport_failure(make_diagnostic(
                "STREAM_INTERRUPTED",
                "The streaming connection ended while stopping playback."))
            return
        self._applied_frames = None
        self._publish()

    def detach(self, outcome):
        if self._closed:
            return self._issued_retention
        self._remove_capture_timer()
        self._remove_playback_poller()
        self._discard_upload()
        self._restore_capture_frame()
        if self._capture_cache is not None:
            self._capture_cache.delete()
            self._capture_cache = None
        kind = getattr(outcome, "kind", None)
        recapture_scene = bool(getattr(outcome, "recapture_scene", False))
        diagnostic = getattr(outcome, "diagnostic", None)
        retain = ((kind == "failed" and not recapture_scene)
                  or self._phase == "transport_failed")
        if retain and self._cache is not None and self._cache.completed:
            try:
                retain = self._cache.compatible_with(self._current_revision())
            except (_PlaybackCacheError, AttributeError, TypeError, ValueError):
                retain = False
        else:
            retain = False
        if retain:
            self._issued_retention = _CachedPlaybackRetention(self._cache)
        elif self._cache is not None:
            self._cache.delete()
        self._cache = None
        self._uploaded_revision = None
        self._streaming_session = None
        self._paused_streaming = False
        self._closed = True
        self._phase = "detached"
        self._publish(diagnostic=diagnostic)
        return self._issued_retention

    def discard(self):
        if not self._closed:
            self._remove_capture_timer()
            self._remove_playback_poller()
            self._discard_upload()
            self._restore_capture_frame()
            self._send_clear_best_effort()
            self._resume_streaming()
        for cache in (self._capture_cache, self._cache):
            if cache is not None:
                cache.delete()
        self._capture_cache = None
        self._cache = None
        if self._issued_retention is not None:
            self._issued_retention._discard()
        self._issued_retention = None
        self._uploaded_revision = None
        self._streaming_session = None
        self._paused_streaming = False
        self._closed = True
        self._phase = "detached"
        self._publish(current=0, total=0)


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
        self._init_message = init_message
        self._init_packet = encode_message(init_message)
        self._latest = _LatestFrame()
        self._ordered = queue.Queue()
        # Controls queued in ordered mode survive the switch back to Latest
        # mode here, so a mode transition can never strand queued work.
        self._carryover = queue.Queue()
        self._ordered_mode = False
        self._stop_event = threading.Event()
        self._lock = threading.Lock()
        self._send_lock = threading.Lock()
        self._socket = None
        self._state = "connecting"
        self._detail = "Connecting to Unreal..."
        self._warning = {"missing_in_unreal": [], "missing_in_maya": [],
                         "bone_name_remaps": [],
                         "has_warning": False}
        self._diagnostic = None
        self._reply_listener = None

    def submit(self, frame_message):
        with self._lock:
            ordered_mode = self._ordered_mode
        if ordered_mode:
            self.submit_ordered(frame_message)
            return
        self._latest.put(encode_message(frame_message))

    def begin_ordered(self):
        with self._send_lock:
            self._begin_ordered_locked()

    def _begin_ordered_locked(self):
        with self._lock:
            self._ordered_mode = True
            latest = self._latest.take()
        while True:
            try:
                self._ordered.get_nowait()
                self._ordered.task_done()
            except queue.Empty:
                break
        if latest is not None:
            self._ordered.put(latest)

    def submit_ordered(self, frame_message):
        self._ordered.put(encode_message(frame_message))

    def ordered_pending(self):
        """Approximate count of queued packets not yet sent to Unreal."""
        return self._ordered.unfinished_tasks

    def end_ordered(self, discard_pending=True):
        with self._send_lock:
            with self._lock:
                self._ordered_mode = False
            if discard_pending:
                while True:
                    try:
                        self._ordered.get_nowait()
                        self._ordered.task_done()
                    except queue.Empty:
                        break
            else:
                # Lossless transition: queued ordered packets keep their place
                # ahead of any subsequent Latest-mode live frames.
                while True:
                    try:
                        packet = self._ordered.get_nowait()
                    except queue.Empty:
                        break
                    self._carryover.put(packet)
                    self._ordered.task_done()

    def status(self):
        with self._lock:
            return (self._state, self._detail, dict(self._warning),
                    dict(self._diagnostic) if self._diagnostic else None)

    def set_reply_listener(self, listener):
        """Route post-ready Unreal replies to listener(reply) on the sender thread."""
        with self._lock:
            self._reply_listener = listener

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
            expected_revision = _exact_int(self._init_message.get("revision"))
            if (expected_revision is None
                    or _exact_int(reply.get("revision")) != expected_revision):
                diagnostic = make_diagnostic(
                    "INVALID_MESSAGE",
                    "The ready reply does not echo the negotiated revision.",
                    details="init revision {0!r}, ready revision {1!r}".format(
                        self._init_message.get("revision"),
                        reply.get("revision")))
                self._set_status("error", diagnostic["summary"], diagnostic=diagnostic)
                return
            connected = True
            warning = blendshape_warning_from_reply(
                filter_bone_driven_differences(self._init_message, reply))
            self._set_status("ready", "Connected", warning=warning)
            sock.settimeout(0.25)
            while not self._stop_event.is_set():
                with self._lock:
                    ordered_mode = self._ordered_mode
                    packet = None
                    try:
                        packet = self._carryover.get_nowait()
                        from_carryover = True
                    except queue.Empty:
                        from_carryover = False
                    if packet is None:
                        if ordered_mode:
                            try:
                                packet = self._ordered.get_nowait()
                            except queue.Empty:
                                packet = None
                        else:
                            packet = self._latest.take()
                if packet is not None:
                    with self._send_lock:
                        sock.sendall(packet)
                    if from_carryover:
                        self._carryover.task_done()
                    elif ordered_mode:
                        self._ordered.task_done()
                readable, _, _ = select.select([sock], [], [], 0.01)
                if readable:
                    try:
                        reply = validate_reply(recv_message(sock))
                    except (ValueError, UnicodeError) as exc:
                        diagnostic = make_diagnostic(
                            "INVALID_MESSAGE", str(exc), details=str(exc))
                        self._set_status(
                            "error", diagnostic["summary"], diagnostic=diagnostic)
                        return
                    with self._lock:
                        listener = self._reply_listener
                    if listener is None:
                        if reply.get("type") == "error":
                            diagnostic = make_diagnostic(
                                reply.get("code") or "STREAM_INTERRUPTED",
                                reply.get("message") or "Unreal closed the connection",
                                details=reply.get("details")
                                or reply.get("message") or "")
                            self._set_status(
                                "error", diagnostic["summary"], diagnostic=diagnostic)
                            return
                        diagnostic = make_diagnostic(
                            "INVALID_MESSAGE",
                            "unexpected runtime reply: {0}".format(reply.get("type")))
                        self._set_status("error", diagnostic["summary"], diagnostic=diagnostic)
                        return
                    try:
                        listener(reply)
                    except Exception as exc:
                        if cmds is not None:
                            try:
                                cmds.warning(
                                    "MtoU_LiveLink cached reply listener failed: {0}".format(
                                        exc))
                            except Exception:
                                pass
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

    def __init__(self, scene, fps, on_event, playback_cap=DEFAULT_PLAYBACK_CAP,
                 playback_state=None, workflow=WORKFLOW_ANIMATION,
                 blendshapes_enabled=True):
        if workflow not in WORKFLOWS:
            raise _StreamingSessionError(
                "INVALID_MESSAGE",
                "workflow must be 'animation' or 'model'.",
                "requested workflow: {0}".format(workflow))
        self._scene = scene
        self._scene_fps = fps
        self._playback_cap = normalize_playback_cap(playback_cap)
        self._playback_state = playback_state or _maya_is_playing
        self._is_playing = self._read_playback_state()
        self._on_event = on_event
        self._workflow = workflow
        self._blendshapes_enabled = bool(blendshapes_enabled)
        self._worker = None
        self._worker_started = False
        self._callback_ids = []
        self._timer_id = None
        self._timer_generation = 0
        self._revision = None
        self._last_sample_time = None
        self._phase = "starting"
        self._outcome = None
        self._terminal_event = None
        self._cleanup_pending = False
        self._cleaned = False
        self._cleanup_error = None
        self._paused_for_cached = False

    @property
    def revision(self):
        return self._revision

    @property
    def workflow(self):
        return self._workflow

    @property
    def blendshapes_enabled(self):
        return self._blendshapes_enabled

    @property
    def is_ready(self):
        if self._outcome is not None or self._worker is None:
            return False
        try:
            return self._worker.status()[0] == "ready"
        except (AttributeError, IndexError, TypeError):
            return self._phase == "ready"

    @classmethod
    def start(cls, scene, fps, on_event=None, playback_cap=DEFAULT_PLAYBACK_CAP,
              playback_state=None, workflow=WORKFLOW_ANIMATION,
              blendshapes_enabled=True):
        try:
            fps = validate_frame_rate(fps)
            snapshot = scene.snapshot()
        except _CharacterSceneError as error:
            raise _StreamingSessionError(
                error.code, error.message, error.details, recapture_scene=True)
        except (RuntimeError, ValueError) as exc:
            raise _StreamingSessionError(
                "INVALID_FRAME_RATE", str(exc), details=str(exc))
        session = cls(scene, fps, on_event, playback_cap, playback_state,
                      workflow, blendshapes_enabled)
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
        if len(snapshot.bind_local_transforms) != len(snapshot.bones):
            raise _StreamingSessionError(
                "BIND_POSE_INVALID",
                "The captured bind pose does not match the captured skeleton.")
        bones = [
            [name, parent, list(snapshot.bind_local_transforms[index])]
            for index, (name, parent) in enumerate(snapshot.bones)
        ]
        init_message = make_init_message(
            bones, list(snapshot.curve_names), snapshot.revision,
            workflow=self._workflow,
            blendshapes_enabled=self._blendshapes_enabled)
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
        condition_message = getattr(om, "MConditionMessage", None)
        if condition_message is not None:
            self._callback_ids.append(
                condition_message.addConditionCallback(
                    "playingBack", self._on_playback_condition_changed))
        self._timer_id = self._add_timer(self._effective_rate())
        self._phase = "connecting"

    def _add_timer(self, fps):
        self._timer_generation += 1
        generation = self._timer_generation

        def on_timer(*args):
            self._on_timer(generation, *args)

        return om.MTimerMessage.addTimerCallback(1.0 / fps, on_timer)

    def _replace_timer(self, fps):
        old_timer = self._timer_id
        try:
            new_timer = self._add_timer(fps)
        except (RuntimeError, ValueError) as exc:
            self._timer_generation += 1
            self._request_failure(
                make_diagnostic("INTERNAL_ERROR", str(exc), details=str(exc)),
                recapture_scene=False)
            return False
        if old_timer is not None:
            try:
                om.MMessage.removeCallback(old_timer)
            except RuntimeError as exc:
                self._remove_callback(new_timer)
                self._timer_generation += 1
                self._request_failure(
                    make_diagnostic("INTERNAL_ERROR", str(exc), details=str(exc)),
                    recapture_scene=False)
                return False
        self._timer_id = new_timer
        self._last_sample_time = None
        return True

    def pause_for_cached(self, reply_listener=None):
        """Suspend live sampling, switch to ordered delivery, and route
        Unreal's post-ready replies to ``reply_listener`` atomically, so no
        submission can precede listener installation."""
        if self._outcome is not None or self._worker is None or not self.is_ready:
            raise _StreamingSessionError(
                "CACHED_PLAYBACK_NOT_READY",
                "Cached Playback requires a ready streaming session.")
        if reply_listener is not None:
            self._worker.set_reply_listener(reply_listener)
        if self._paused_for_cached:
            return
        self._paused_for_cached = True
        self._timer_generation += 1
        timer_id, self._timer_id = self._timer_id, None
        if timer_id is not None:
            self._remove_callback(timer_id)
        self._worker.begin_ordered()
        self._last_sample_time = None

    def set_cached_reply_listener(self, listener):
        """Route Unreal's post-ready replies to the cached playback session."""
        if self._worker is not None:
            self._worker.set_reply_listener(listener)

    def cached_delivery_drained(self):
        """True once every ordered cached message has reached the socket.

        Meaningful between pause_for_cached() and resume_from_cached().
        An empty backlog proves wire delivery, never Unreal-side parsing;
        only an identity-matched cache_ready outcome confirms acceptance.
        Terminal or unstarted sessions are vacuously drained so a drain
        gate can never wedge.
        """
        worker = self._worker
        if worker is None:
            return True
        return worker.ordered_pending() == 0

    def submit_cached(self, frame_message):
        if not self.is_ready:
            raise _StreamingSessionError(
                "CACHED_PLAYBACK_NOT_READY",
                "The streaming session is not ready for cached messages.")
        self._worker.submit_ordered(frame_message)

    def end_cached_replay(self, discard_pending=False):
        if self._worker is None:
            return
        self._worker.end_ordered(discard_pending=discard_pending)

    def resume_from_cached(self):
        if not self._paused_for_cached:
            return
        self.end_cached_replay()
        self._paused_for_cached = False
        if self._outcome is not None or self._worker is None:
            return
        self._timer_id = self._add_timer(self._effective_rate())
        self._last_sample_time = None
        self._sample_and_submit(force=True)

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
        self._scene_fps = fps
        if not self._paused_for_cached:
            self._replace_timer(self._effective_rate())

    def change_cap(self, playback_cap):
        if self._outcome is not None:
            return
        self._playback_cap = normalize_playback_cap(playback_cap)
        if self._paused_for_cached:
            return
        if not self._replace_timer(self._effective_rate()):
            return
        self._sample_and_submit(force=True)

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

    def _on_timer(self, generation, *unused):
        if generation != self._timer_generation or self._paused_for_cached:
            return
        self._update_playback_state()
        self._sample_and_submit()

    def _on_time_changed(self, *unused):
        del unused
        if self._paused_for_cached:
            return
        self._update_playback_state()
        self._sample_and_submit()

    def _on_playback_condition_changed(self, *args):
        if self._paused_for_cached:
            return
        playing = None
        for value in args:
            if isinstance(value, bool):
                playing = value
                break
        self._update_playback_state(playing)

    def _read_playback_state(self):
        try:
            return bool(self._playback_state())
        except (AttributeError, RuntimeError, TypeError):
            return False

    def _effective_rate(self):
        cap = playback_cap_fps(self._playback_cap)
        if not self._is_playing or cap is None:
            return self._scene_fps
        return min(self._scene_fps, cap)

    def _update_playback_state(self, playing=None):
        if self._outcome is not None:
            return False
        next_state = self._read_playback_state() if playing is None else bool(playing)
        if next_state == self._is_playing:
            return False
        was_playing = self._is_playing
        self._is_playing = next_state
        self._last_sample_time = None
        if not self._replace_timer(self._effective_rate()):
            return True
        if was_playing and not next_state:
            self._sample_and_submit(force=True)
        return True

    def _sample_and_submit(self, force=False):
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
        sample_interval = 1.0 / self._effective_rate()
        if (not force and self._last_sample_time is not None
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
        self._timer_generation += 1
        self.end_cached_replay(discard_pending=True)
        self._paused_for_cached = False
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


def _bind_conflict_status(snapshot):
    if not snapshot.bind_conflict_count:
        return ""
    return "（已按绑定姿势解析 {0} 处蒙皮绑定矩阵冲突）".format(
        snapshot.bind_conflict_count)


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
        self._playback_cap = DEFAULT_PLAYBACK_CAP
        self._playback_cap_menu = None
        self._mode = REALTIME_MODE
        self._realtime_mode_button = None
        self._cached_mode_button = None
        self._capture_button = None
        self._replay_button = None
        self._stop_replay_button = None
        self._cancel_capture_button = None
        self._cache_text = None
        self._cached_playback = None
        self._cached_view = None
        self._cached_retention = None
        self._maya_exit_callback = None
        self._workflow = WORKFLOW_ANIMATION
        self._blendshapes_enabled = True
        self._animation_workflow_button = None
        self._model_workflow_button = None
        self._bs_checkbox = None
        self._bone_text = None
        self._curve_text = None
        self._status_text = None
        self._cache_row = None
        self._connect_button = None
        self._disconnect_button = None
        self._shown_warning_signatures = set()
        self._warning_checkbox = None
        self._duplicate_button = None

    @staticmethod
    def _grid_row(controls, spans=None, height=BUTTON_HEIGHT):
        """Attach controls of the current formLayout to equal grid columns.

        ``spans`` gives each control's column count (default one each). Every
        row shares the same columns and GRID_GAP gutter, so edges line up.
        """
        form = cmds.setParent(query=True)
        spans = spans or [1] * len(controls)
        columns = sum(spans)
        positions, attach_form, start = [], [], 0
        for control, span in zip(controls, spans):
            end = start + span
            positions.append((control, "left", 0 if start == 0 else GRID_GAP // 2,
                              start * 100 // columns))
            positions.append((control, "right", 0 if end == columns else GRID_GAP // 2,
                              end * 100 // columns))
            attach_form.append((control, "top", 0))
            start = end
        cmds.formLayout(form, edit=True, height=height, attachPosition=positions,
                        attachForm=attach_form)
        cmds.setParent("..")
        return form

    def build_ui(self):
        cmds.window(WINDOW_NAME, title="MtoU Live Link", closeCommand=self.close,
                    sizeable=False, width=PANEL_WIDTH + 2 * PANEL_MARGIN,
                    resizeToFitChildren=True)
        cmds.columnLayout(adjustableColumn=True, rowSpacing=CARD_MARGIN,
                          columnAttach=("both", PANEL_MARGIN))
        cmds.separator(height=4, style="none")

        # Card 1 · workflow tabs.
        cmds.frameLayout(label="工作流", collapsable=False, marginWidth=CARD_MARGIN,
                         marginHeight=CARD_MARGIN)
        cmds.rowLayout(numberOfColumns=2, columnWidth2=(CARD_HALF, CARD_HALF + CARD_GAP),
                       columnAttach2=("both", "both"))
        self._animation_workflow_button = cmds.button(
            label="动画", height=TAB_HEIGHT, backgroundColor=TAB_ON_BACKGROUND,
            annotation="动画工作流：实时预览或缓存播放角色动画。",
            command=lambda *_: self._on_workflow_changed(WORKFLOW_ANIMATION))
        self._model_workflow_button = cmds.button(
            label="模型", height=TAB_HEIGHT, backgroundColor=TAB_OFF_BACKGROUND,
            annotation="模型工作流：在 UE 用 Generated Preview 检查服装。",
            command=lambda *_: self._on_workflow_changed(WORKFLOW_MODEL))
        cmds.setParent("..")
        cmds.setParent("..")

        # Card 2 · connect.
        cmds.frameLayout(label="连接控制", collapsable=False, marginWidth=CARD_MARGIN,
                         marginHeight=CARD_MARGIN)
        cmds.formLayout(width=CARD_CONTENT)
        set_role = cmds.button(label="设置角色", height=BUTTON_HEIGHT,
                               annotation="先在 Maya 中选择变形根骨骼，再设置角色。",
                               command=lambda *_: self.set_role())
        self._connect_button = cmds.button(
            label="连接", height=BUTTON_HEIGHT, backgroundColor=PRIMARY_BACKGROUND,
            annotation="把当前角色连接到 Unreal Binding Actor。",
            command=lambda *_: self.connect())
        self._disconnect_button = cmds.button(
            label="断开", height=BUTTON_HEIGHT, command=lambda *_: self.disconnect())
        self._light = cmds.text(label="●  未连接", align="center", height=BUTTON_HEIGHT,
                                font="boldLabelFont", backgroundColor=LIGHT_OFF_BACKGROUND)
        self._grid_row([set_role, self._connect_button, self._disconnect_button, self._light])
        cmds.setParent("..")

        # Card 3 · scene info, two label columns.
        cmds.frameLayout(label="场景信息", collapsable=False, marginWidth=CARD_MARGIN,
                         marginHeight=CARD_MARGIN)
        cmds.rowLayout(numberOfColumns=2, columnWidth2=(CARD_HALF - 8, CARD_HALF - 8),
                       columnAttach2=("left", "left"),
                       columnOffset2=(8, CARD_HALF + CARD_GAP))
        cmds.columnLayout(adjustableColumn=False, rowSpacing=4)
        self._root_text = cmds.text(label="角色根骨骼：—", align="left")
        self._outfit_text = cmds.text(label="当前衣服：—", align="left")
        self._fps_text = cmds.text(label="场景帧率：—", align="left")
        cmds.setParent("..")
        cmds.columnLayout(adjustableColumn=False, rowSpacing=4)
        self._bone_text = cmds.text(label="骨骼数：0", align="left")
        self._curve_text = cmds.text(label="BlendShape 数：0", align="left")
        self._cache_text = cmds.text(label="缓存：无", align="left", height=18)
        cmds.setParent("..")
        cmds.setParent("..")
        cmds.setParent("..")

        # Card 4 · preview modes, transfer cap, and the cache action row.
        cmds.frameLayout(label="预览与播放", collapsable=False, marginWidth=CARD_MARGIN,
                         marginHeight=CARD_MARGIN)
        cmds.columnLayout(adjustableColumn=True, rowSpacing=CARD_MARGIN)
        cmds.formLayout(width=CARD_CONTENT)
        self._realtime_mode_button = cmds.button(
            label="实时预览", height=BUTTON_HEIGHT, backgroundColor=TOGGLE_ON_BACKGROUND,
            command=lambda *_: self._on_mode_changed(REALTIME_MODE))
        self._cached_mode_button = cmds.button(
            label="缓存播放", height=BUTTON_HEIGHT, backgroundColor=TOGGLE_OFF_BACKGROUND,
            command=lambda *_: self._on_mode_changed(CACHED_MODE))
        self._bs_checkbox = cmds.checkBox(
            label="传递 BS", value=True, height=BUTTON_HEIGHT,
            annotation="连接时把 Maya BlendShape 值一起传给 Unreal。",
            changeCommand=lambda *_: self._on_blendshapes_toggled())
        self._playback_cap = load_playback_cap()
        self._playback_cap_menu = cmds.optionMenu(
            label="上限", height=BUTTON_HEIGHT, changeCommand=self._on_playback_cap_changed)
        for choice in PLAYBACK_CAP_CHOICES:
            cmds.menuItem(label=choice)
        cmds.optionMenu(self._playback_cap_menu, edit=True, value=self._playback_cap)
        # The mode buttons and the BS switch share the first two columns; only
        # one of them is managed per workflow.
        self._grid_row([self._realtime_mode_button, self._cached_mode_button,
                        self._playback_cap_menu])
        self._cache_row = cmds.formLayout(width=CARD_CONTENT)
        self._capture_button = cmds.button(
            label="捕获并回放", height=BUTTON_HEIGHT, enable=False,
            command=lambda *_: self._capture_cached_playback())
        self._replay_button = cmds.button(
            label="再次回放", height=BUTTON_HEIGHT, enable=False,
            command=lambda *_: self._replay_cached_playback())
        self._stop_replay_button = cmds.button(
            label="停止回放", height=BUTTON_HEIGHT, enable=False,
            command=lambda *_: self._stop_cached_replay())
        self._cancel_capture_button = cmds.button(
            label="取消捕获", height=BUTTON_HEIGHT, enable=False,
            command=lambda *_: self._cancel_cached_capture())
        self._grid_row([self._capture_button, self._replay_button,
                        self._stop_replay_button, self._cancel_capture_button])
        cmds.setParent("..")
        cmds.setParent("..")

        # Card 5 · advanced tools.
        cmds.frameLayout(label="工具", collapsable=False, marginWidth=CARD_MARGIN,
                         marginHeight=CARD_MARGIN)
        cmds.formLayout(width=CARD_CONTENT)
        display = cmds.button(label="选择 Display 控制器", height=BUTTON_HEIGHT,
                              annotation="检测到多个服装属性时，选中 Display 控制器后点击。",
                              command=lambda *_: self.set_display_controller())
        self._duplicate_button = cmds.button(
            label="选中重名骨骼（0）", height=BUTTON_HEIGHT, enable=False,
            annotation="重名会被 UE 自动映射；点击可在场景中选中所有冲突骨骼。",
            command=lambda *_: self.select_duplicate_bones())
        self._grid_row([display, self._duplicate_button])
        cmds.setParent("..")

        # Card 6 · diagnostics.
        cmds.frameLayout(label="诊断", collapsable=False, marginWidth=CARD_MARGIN,
                         marginHeight=CARD_MARGIN)
        cmds.rowLayout(numberOfColumns=2, adjustableColumn=1,
                       columnWidth2=(CARD_CONTENT - 110, 110),
                       columnAttach2=("both", "both"), columnOffset2=(0, 4))
        self._status_text = cmds.text(label="未设置角色：选择根骨骼后点击“设置角色”",
                                      align="left", wordWrap=True, font="boldLabelFont")
        cmds.button(label="诊断详情", height=BUTTON_HEIGHT,
                    command=lambda *_: self.show_diagnostics())
        cmds.setParent("..")
        cmds.setParent("..")

        cmds.rowLayout(numberOfColumns=1, columnWidth1=CARD_CONTENT)
        self._warning_checkbox = cmds.checkBox(
            label="连接成功后弹出差异警告", value=True)
        cmds.setParent("..")
        self._refresh_fps()
        self._update_mode_controls()
        self._update_workflow_controls()
        self._script_jobs.append(cmds.scriptJob(
            event=["timeUnitChanged", self._on_time_unit_changed], parent=WINDOW_NAME))
        self._script_jobs.append(cmds.scriptJob(
            event=["SceneOpened", self._on_scene_change], parent=WINDOW_NAME))
        self._script_jobs.append(cmds.scriptJob(
            event=["NewSceneOpened", self._on_scene_change], parent=WINDOW_NAME))
        if om is not None:
            try:
                self._maya_exit_callback = om.MSceneMessage.addCallback(
                    om.MSceneMessage.kMayaExiting, self._on_maya_exiting)
            except (AttributeError, RuntimeError, TypeError):
                self._maya_exit_callback = None
        cmds.showWindow(WINDOW_NAME)



    def _control_exists(self, control):
        if not control or cmds is None:
            return False
        try:
            return bool(cmds.control(control, exists=True))
        except (AttributeError, RuntimeError, TypeError):
            return False

    def _set_enabled(self, control, enabled):
        if self._control_exists(control):
            try:
                cmds.control(control, edit=True, enable=bool(enabled))
                if control in (self._connect_button, self._capture_button):
                    cmds.button(control, edit=True, backgroundColor=(
                        PRIMARY_BACKGROUND if enabled else TOGGLE_OFF_BACKGROUND))
            except (AttributeError, RuntimeError, TypeError):
                pass

    def _set_visible(self, control, visible):
        if self._control_exists(control):
            try:
                cmds.control(control, edit=True, visible=bool(visible),
                             manage=bool(visible))
            except (AttributeError, RuntimeError, TypeError):
                pass

    def _set_tooltip(self, control, tooltip):
        if self._control_exists(control):
            try:
                cmds.control(control, edit=True, annotation=tooltip or "")
            except (AttributeError, RuntimeError, TypeError):
                pass

    @staticmethod
    def _disabled_reason(enabled, action):
        if enabled:
            return ""
        reasons = {
            "capture": "先连接 Unreal 并进入缓存播放，再捕获当前 Maya Playback Range。",
            "replay": "需要 UE 已就绪的完整缓存；先捕获并等待上传完成。",
            "stop": "仅在 UE 本地回放运行时可停止。",
            "cancel": "仅在捕获过程中可取消。",
        }
        return reasons.get(action, "")


    def _warning_signature(self, warning):
        missing_unreal = tuple(sorted(warning.get("missing_in_unreal") or ()))
        missing_maya = tuple(sorted(warning.get("missing_in_maya") or ()))
        remaps = tuple(sorted(warning.get("bone_name_remaps") or ()))
        return (missing_unreal, missing_maya, remaps)

    def _session_ready(self):
        session = self._session
        if session is None:
            return False
        ready = getattr(session, "is_ready", None)
        if callable(ready):
            try:
                return bool(ready())
            except (AttributeError, RuntimeError, TypeError):
                return False
        if ready is not None:
            return bool(ready)
        return False

    def _apply_toggle_background(self, control, selected):
        if not self._control_exists(control):
            return
        try:
            cmds.button(control, edit=True, backgroundColor=(
                TOGGLE_ON_BACKGROUND if selected else TOGGLE_OFF_BACKGROUND))
        except (AttributeError, RuntimeError, TypeError):
            pass

    def _update_mode_selection(self):
        self._apply_toggle_background(
            self._realtime_mode_button, self._mode == REALTIME_MODE)
        self._apply_toggle_background(
            self._cached_mode_button, self._mode == CACHED_MODE)

    def _update_mode_controls(self):
        realtime = self._mode == REALTIME_MODE
        view = self._cached_view
        can_capture = bool(view and view.can_capture)
        can_replay = bool(view and view.can_replay)
        can_stop = bool(view and view.can_stop)
        can_cancel = bool(view and view.can_cancel)
        can_leave = bool(view and view.can_leave)
        connected = self._session_ready()
        self._set_enabled(self._playback_cap_menu, realtime and not can_cancel)
        self._set_enabled(self._capture_button, not realtime and can_capture)
        self._set_enabled(self._replay_button, not realtime and can_replay)
        self._set_enabled(self._stop_replay_button, not realtime and can_stop)
        self._set_enabled(self._cancel_capture_button, not realtime and can_cancel)
        self._set_enabled(self._realtime_mode_button, realtime or can_leave)
        self._set_enabled(self._cached_mode_button, connected and not can_cancel)
        self._set_enabled(self._connect_button, not connected)
        self._set_enabled(self._disconnect_button, connected or self._scene is not None)
        self._set_tooltip(
            self._capture_button, self._disabled_reason(can_capture, "capture"))
        self._set_tooltip(
            self._replay_button, self._disabled_reason(can_replay, "replay"))
        self._set_tooltip(
            self._stop_replay_button, self._disabled_reason(can_stop, "stop"))
        self._set_tooltip(
            self._cancel_capture_button, self._disabled_reason(can_cancel, "cancel"))
        self._set_tooltip(
            self._cached_mode_button,
            "先设置角色并连接 Unreal。" if not connected else
            ("等待捕获完成，或取消捕获。" if can_cancel else "捕获 Maya 播放范围，在 UE 本地回放。"))
        self._set_tooltip(
            self._realtime_mode_button,
            "在 Maya 摆姿或播放，实时更新 UE。" if realtime or can_leave
            else "等待当前缓存操作完成后返回实时预览。")
        self._set_tooltip(
            self._playback_cap_menu,
            "限制实时预览的传输帧率，不改变 Maya 场景帧率。"
            if realtime and not can_cancel else "返回实时预览后可调整传输上限。")
        self._update_mode_selection()
        self._update_context_layout()

    def _update_workflow_controls(self):
        animation = self._workflow == WORKFLOW_ANIMATION
        self._apply_tab_background(self._animation_workflow_button, animation)
        self._apply_tab_background(self._model_workflow_button, not animation)
        for control in (self._realtime_mode_button, self._cached_mode_button,
                        self._cache_text):
            self._set_visible(control, animation)
        self._set_visible(self._bs_checkbox, not animation)
        self._update_context_layout()

    def _apply_tab_background(self, control, selected):
        if self._control_exists(control):
            try:
                cmds.button(control, edit=True, backgroundColor=(
                    TAB_ON_BACKGROUND if selected else TAB_OFF_BACKGROUND))
            except (AttributeError, RuntimeError, TypeError):
                pass

    def _update_context_layout(self):
        visible = self._workflow == WORKFLOW_ANIMATION and self._mode == CACHED_MODE
        layout = self._cache_row
        if layout and cmds is not None and cmds.layout(layout, exists=True):
            if cmds.layout(layout, query=True, manage=True) != visible:
                cmds.layout(layout, edit=True, visible=visible, manage=visible)

    def _on_workflow_changed(self, workflow):
        if workflow == self._workflow or workflow not in WORKFLOWS:
            return
        self._workflow = workflow
        # Switching workflows disconnects and clears Animation cached
        # playback while retaining the captured root, Display control, and
        # current outfit. The Maya scene is never edited here.
        self.disconnect(status="已切换到{0}工作流，请重新连接".format(
            "模型" if workflow == WORKFLOW_MODEL else "动画"))
        self._mode = REALTIME_MODE
        self._update_mode_controls()
        self._update_workflow_controls()

    def _on_blendshapes_toggled(self):
        value = True
        if self._bs_checkbox and cmds.control(self._bs_checkbox, exists=True):
            try:
                value = bool(cmds.checkBox(self._bs_checkbox, query=True, value=True))
            except (AttributeError, RuntimeError, TypeError):
                value = True
        if value == self._blendshapes_enabled:
            return
        self._blendshapes_enabled = value
        if self._session is not None:
            self.disconnect(status="传递 BS 已切换，请重新连接")

    def _update_cache_text(self, summary=None):
        if summary is None:
            self._set_text(self._cache_text, "缓存：无")
            return
        start, end = summary.capture_range
        capture_time = time.strftime(
            "%Y-%m-%d %H:%M:%S", time.localtime(summary.capture_time))
        self._set_text(
            self._cache_text,
            "缓存：{0}-{1}，{2} 帧，{3:g} fps，捕获于 {4}".format(
                start, end, summary.frame_count, summary.scene_fps, capture_time))

    def _ensure_cached_playback(self):
        if self._workflow != WORKFLOW_ANIMATION or self._session is None \
                or self._scene is None or self._cached_playback is None:
            raise _CachedPlaybackError(
                "CACHED_PLAYBACK_NOT_READY",
                "请先完成角色设置并连接 Unreal。")
        return self._cached_playback

    def _on_mode_changed(self, mode):
        if mode == CACHED_MODE and self._workflow != WORKFLOW_ANIMATION:
            return
        if mode == CACHED_MODE:
            if not self._session_ready():
                self._mode = REALTIME_MODE
                self._update_mode_selection()
                self._update_mode_controls()
                self._show_error(make_diagnostic(
                    "CACHED_PLAYBACK_NOT_READY",
                    "请先完成角色设置并连接 Unreal。"))
                return
            try:
                self._ensure_cached_playback().enter()
                self._mode = CACHED_MODE
                self._set_connected(
                    True,
                    "缓存播放：实时采样已暂停，点击“捕获并回放”")
            except _CachedPlaybackError as error:
                self._mode = REALTIME_MODE
                self._update_mode_selection()
                self._show_error(make_diagnostic(
                    error.code if error.code in DIAGNOSTICS else "INTERNAL_ERROR",
                    error.message, details=error.details))
        else:
            self._mode = REALTIME_MODE
            if self._cached_playback is not None:
                self._cached_playback.leave()
            if self._session_ready():
                self._set_connected(True, "已连接：UE 显示实时姿势")
            else:
                self._set_connected(False, "未连接")
        self._update_mode_controls()

    def _confirm_large_cache(self, estimated_size, total_frames):
        gib = float(estimated_size) / float(1 << 30)
        result = cmds.confirmDialog(
            title="确认大型缓存",
            message="本次捕获约 {0:.2f} GiB（{1} 帧），是否继续？".format(
                gib, total_frames),
            button=["继续", "取消"], defaultButton="继续", cancelButton="取消",
            dismissString="取消")
        return result == "继续"

    def _capture_cached_playback(self):
        if self._mode != CACHED_MODE:
            return
        try:
            cached = self._ensure_cached_playback()
            fps = validate_frame_rate(self._refresh_fps())
            self._set_text(self._status_text, "准备捕获缓存…")
            cached.capture(fps, self._confirm_large_cache)
        except _CachedPlaybackError as error:
            diagnostic = make_diagnostic(
                error.code if error.code in DIAGNOSTICS else "INTERNAL_ERROR",
                error.message, details=error.details)
            self._last_diagnostic = diagnostic
            self._show_error(diagnostic)
        except (RuntimeError, ValueError) as error:
            diagnostic = make_diagnostic("INTERNAL_ERROR", str(error), details=str(error))
            self._last_diagnostic = diagnostic
            self._show_error(diagnostic)
        finally:
            self._update_mode_controls()


    def _replay_cached_playback(self):
        if self._mode != CACHED_MODE:
            return
        try:
            cached = self._ensure_cached_playback()
            cached.replay()
        except _CachedPlaybackError as error:
            diagnostic = make_diagnostic(
                error.code if error.code in DIAGNOSTICS else "INTERNAL_ERROR",
                error.message, details=error.details)
            self._last_diagnostic = diagnostic
            self._show_error(diagnostic)
        finally:
            self._update_mode_controls()

    def _stop_cached_replay(self):
        if self._mode != CACHED_MODE or self._cached_playback is None:
            return
        self._cached_playback.stop_replay()
        self._update_mode_controls()

    def _cancel_cached_capture(self):
        if self._cached_playback is not None:
            self._cached_playback.cancel_capture()

    def _on_cached_playback_view(self, view):
        self._cached_view = view
        self._update_cache_text(view.cache_summary)
        if (view.state == _CachedPlayback.CAPTURING and view.total
                and view.current == view.total):
            self._set_text(self._status_text, "缓存完成，正在上传…")
        elif view.state == _CachedPlayback.CAPTURING and view.current == 0:
            self._set_text(self._status_text, "正在捕获缓存…")
        elif view.state == _CachedPlayback.CAPTURING:
            self._set_text(
                self._status_text,
                "正在捕获缓存 {0}/{1}".format(view.current, view.total))
        elif (view.state == _CachedPlayback.UPLOADING and view.total
              and view.current == view.total):
            self._set_connected(True, "缓存已上传，Unreal 正在本地回放")
        elif view.state == _CachedPlayback.UPLOADING and view.current == 0:
            self._set_text(self._status_text, "正在上传缓存…")
        elif view.state == _CachedPlayback.UPLOADING:
            self._set_text(
                self._status_text,
                "正在上传缓存 {0}/{1}".format(
                    view.current, view.total))
        elif view.state == _CachedPlayback.REPLAYING and view.current == 0:
            self._set_connected(
                True, "缓存回放中：Unreal 按捕获帧率本地播放 {0}/{1}".format(
                    view.current, view.total))
        elif view.state == _CachedPlayback.REPLAYING:
            self._set_text(
                self._status_text,
                "缓存播放（仅显示已捕获缓存） {0}/{1}".format(
                    view.current, view.total))
        elif view.state == _CachedPlayback.COMPLETED:
            self._set_connected(True, "缓存播放完成，已停在最后一帧")
        elif view.state == _CachedPlayback.STOPPED:
            self._set_connected(True, "缓存播放已停止，已保留缓存，可再次回放")
        elif view.state == _CachedPlayback.FAILED:
            # A FAILED cached view always keeps the negotiated connection: the
            # capture/upload paths that resume Real-time Preview now render as
            # REALTIME. This state still permits leaving or retrying.
            diagnostic = view.diagnostic or make_diagnostic("INTERNAL_ERROR")
            self._last_diagnostic = diagnostic
            self._set_connected(True, diagnostic["summary"])
        elif view.state == _CachedPlayback.REALTIME and view.diagnostic:
            # A recoverable capture/upload/invalidation failure already resumed
            # Real-time Preview and cleared Unreal ownership; complete the same
            # transition in the UI and expose the reason, leaving the cached
            # workflow available.
            diagnostic = view.diagnostic
            self._last_diagnostic = diagnostic
            self._mode = REALTIME_MODE
            self._update_mode_selection()
            self._set_connected(
                self._session_ready(),
                "{0}（已恢复实时预览）".format(diagnostic["summary"]))
        elif view.state == _CachedPlayback.DETACHED and view.diagnostic:
            diagnostic = view.diagnostic
            self._last_diagnostic = diagnostic
            self._set_connected(False, diagnostic["summary"])
        self._update_mode_controls()

    def _set_text(self, control, text):
        if control and cmds.control(control, exists=True):
            cmds.text(control, edit=True, label=text, annotation=text)

    def _set_connected(self, connected, status):
        label = "●  已连接" if connected else "●  未连接"
        if self._light and cmds.control(self._light, exists=True):
            cmds.text(self._light, edit=True, label=label, backgroundColor=(
                LIGHT_ON_BACKGROUND if connected else LIGHT_OFF_BACKGROUND))
        self._set_text(self._status_text, status)
        self._update_mode_controls()

    def _refresh_fps(self):
        try:
            fps = frames_per_second(cmds.currentUnit(query=True, time=True))
            self._set_text(self._fps_text, "帧率：" + format_fps(fps))
            return fps
        except ValueError:
            unit = cmds.currentUnit(query=True, time=True)
            self._set_text(self._fps_text, "帧率：{0}（不受支持）".format(unit))
            return None

    def _on_playback_cap_changed(self, value, *unused):
        del unused
        self._playback_cap = save_playback_cap(value)
        if self._playback_cap_menu and cmds.control(self._playback_cap_menu, exists=True):
            cmds.optionMenu(self._playback_cap_menu, edit=True, value=self._playback_cap)
        if self._session is not None and self._mode == REALTIME_MODE:
            self._session.change_cap(self._playback_cap)

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
        # A diagnostic action renders the real connection state; it never
        # falsifies the indicator independently of the Streaming session.
        connected = self._session_ready()
        existing = [path for path in self._duplicate_paths() if cmds.objExists(path)]
        if not existing:
            self._refresh_duplicate_button()
            self._set_connected(connected, "重名骨骼已不存在，请重新设置角色")
            return
        cmds.select(existing, replace=True)
        self._set_connected(connected, "已选中 {0} 个重名骨骼".format(len(existing)))

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
            status += _bind_conflict_status(snapshot)
            self._set_connected(False, status)
        except _CharacterSceneError as error:
            if error.code == "AMBIGUOUS_DISPLAY":
                self._pending_root = root
                self._set_text(self._root_text, "角色根骨骼：" + root)
                self._set_connected(
                    False, error.message + "：选中 Display 控制器后点击“选择 Display 控制器”")
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
            # Validate the requested selection first: an invalid selection
            # must never tear down the live session or a usable scene.
            display = self._selected_display()
        except (RuntimeError, ValueError) as exc:
            code = str(exc) if str(exc) in DIAGNOSTICS else "ROLE_SETUP_FAILED"
            self._show_error(make_diagnostic(code, str(exc), solution=str(exc), details=str(exc)))
            return
        try:
            # One clean termination before the replacement: the old Streaming
            # session ends while the old Character scene is still usable, so
            # a late old-session event can never close the new capture.
            self.disconnect(status="正在更换 Display 控制器，请重新连接")
            self._clear_scene(keep_pending=True)
            self._pending_root = root
            snapshot = self._capture_scene(root, display=display).snapshot()
            status = "Display 控制器已设置，可以连接"
            self._set_connected(False, status + _bind_conflict_status(snapshot))
        except _CharacterSceneError as error:
            self._show_error(self._scene_diagnostic(error))
    def _on_character_scene_event(self, event):
        if event.kind == "character_change_started":
            self._discard_cached_playback()
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
            status += _bind_conflict_status(event.snapshot)
            self._outfit_change_was_connected = False
            self._set_connected(False, status)
            return
        if event.kind == "character_invalidated":
            self._discard_cached_playback()
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
        self._shown_warning_signatures = set()
        self._set_connected(False, "正在连接 Unreal…")
        holder = {}

        def on_event(event):
            self._on_streaming_session_event(holder.get("session"), event)

        try:
            session = _StreamingSession.start(
                self._scene, fps, on_event, playback_cap=self._playback_cap,
                workflow=self._workflow,
                blendshapes_enabled=self._blendshapes_enabled)
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
        self._mode = REALTIME_MODE
        self._update_mode_controls()

    def _on_streaming_session_event(self, session, event):
        if session is None or self._session is not session:
            return
        if event.kind == "ready":
            if self._workflow == WORKFLOW_ANIMATION:
                try:
                    cached_playback = _CachedPlayback(
                        self._scene, session, on_change=self._on_cached_playback_view,
                        retention=self._cached_retention)
                except _CachedPlaybackError as error:
                    diagnostic = make_diagnostic(
                        error.code if error.code in DIAGNOSTICS else "INTERNAL_ERROR",
                        error.message, details=error.details)
                    self._last_diagnostic = diagnostic
                    self._set_connected(False, diagnostic["summary"])
                    self._show_error(diagnostic)
                    session.stop()
                    return
                self._cached_retention = None
                self._cached_playback = cached_playback
                self._on_cached_playback_view(cached_playback.view)
            warning = event.warning or {
                "missing_in_unreal": [], "missing_in_maya": [],
                "bone_name_remaps": [], "has_warning": False}
            self._last_warning = warning
            if warning["has_warning"]:
                self._set_connected(True, "已连接（有警告）：UE 显示实时姿势，请检查差异")
            else:
                self._set_connected(True, "已连接：UE 显示实时姿势")
            if warning["has_warning"] \
                    and cmds.checkBox(self._warning_checkbox, query=True, value=True):
                signature = self._warning_signature(warning)
                if signature not in self._shown_warning_signatures:
                    self._shown_warning_signatures.add(signature)
                    self._show_warning(warning)
            return
        cached_playback = self._cached_playback
        if cached_playback is not None:
            self._cached_retention = cached_playback.detach(event)
            self._cached_view = cached_playback.view
        self._session = None
        self._mode = REALTIME_MODE
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
        summary = diagnostic.get("summary") or "—"
        solution = diagnostic.get("solution") or "—"
        details = diagnostic.get("details") or diagnostic.get("message") or ""
        sections = [
            "摘要：{0}".format(summary),
            "解决办法：{0}".format(solution),
            "Error code: {0}".format(diagnostic.get("code", "—")),
        ]
        if details and details not in (summary, solution):
            sections.append("错误详情：\n{0}".format(details))
        return "\n\n".join(sections)

    def show_diagnostics(self, *unused):
        if cmds.window(DIAGNOSTIC_WINDOW_NAME, exists=True):
            cmds.deleteUI(DIAGNOSTIC_WINDOW_NAME)
        cmds.window(
            DIAGNOSTIC_WINDOW_NAME, title="MtoU 诊断详情",
            sizeable=True, widthHeight=(620, 360))
        layout = cmds.formLayout()
        field = cmds.scrollField(
            editable=False, wordWrap=True, text=self._diagnostic_text())
        copy_button = cmds.button(
            label="复制详情", command=lambda *_: self._copy_text(
                cmds.scrollField(field, query=True, text=True)))
        cmds.formLayout(
            layout, edit=True,
            attachForm=[
                (field, "top", 8),
                (field, "left", 8),
                (field, "right", 8),
                (copy_button, "left", 8),
                (copy_button, "right", 8),
                (copy_button, "bottom", 8),
            ],
            attachControl=[(field, "bottom", 6, copy_button)])
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

    def _on_maya_exiting(self, *unused):
        del unused
        self._discard_cached_playback()

    def _remove_maya_exit_callback(self):
        callback_id, self._maya_exit_callback = self._maya_exit_callback, None
        if callback_id is None or om is None:
            return
        try:
            om.MMessage.removeCallback(callback_id)
        except (AttributeError, RuntimeError, TypeError):
            pass

    def disconnect(self, keep_error=False, status="已断开连接"):
        del keep_error
        self._discard_cached_playback()
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

    def _discard_cached_playback(self):
        cached_playback, self._cached_playback = self._cached_playback, None
        if cached_playback is not None:
            cached_playback.discard()
        self._cached_retention = None
        self._cached_view = None
        self._update_cache_text()

    def _clear_scene(self, keep_pending=False, keep_capture=False):
        self._discard_cached_playback()
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
        self._remove_maya_exit_callback()
        self._clear_role()


_CONTROLLER = None
WINDOW_NAME = "MtoULiveLinkWindow"
DIAGNOSTIC_WINDOW_NAME = "MtoULiveLinkDiagnosticWindow"


def run():
    global _CONTROLLER
    _require_maya()
    try:
        _PlaybackCache.cleanup_stale()
    except (OSError, TypeError, ValueError):
        pass
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
