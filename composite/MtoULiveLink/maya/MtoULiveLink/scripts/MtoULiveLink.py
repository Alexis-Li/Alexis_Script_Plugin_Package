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


class _Controller(object):
    def __init__(self):
        self._worker = None
        self._subject = None
        self._role = None
        self._connection_callback_ids = []
        self._role_callback_ids = []
        self._script_jobs = []
        self._timer_id = None
        self._ready = False
        self._warning_shown = False
        self._session_error_shown = False
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
        self._duplicate_paths = []

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

    def _choose_enum(self, node, candidates):
        if len(candidates) == 1:
            return candidates[0]
        if not candidates:
            raise ValueError("所选控制器没有包含 Clothes 选项的 enum 属性。")
        labels = [candidate["attribute"] for candidate in candidates]
        result = cmds.confirmDialog(
            title="选择服装属性", message="找到多个候选属性，请选择：",
            button=labels + ["取消"], defaultButton=labels[0], cancelButton="取消",
            dismissString="取消")
        if result == "取消":
            raise ValueError("已取消 Display 属性选择。")
        return candidates[labels.index(result)]

    def _configure_display(self, node):
        role_root = self._role["root"]
        role_top = _top_level(role_root)
        if node != role_top and not node.startswith(role_top + "|"):
            raise ValueError("Display 控制器必须位于当前角色的同一顶层组内。")
        if _namespace(node) != _namespace(role_root):
            raise ValueError("Display 控制器与角色根骨骼的 namespace 不一致。")
        display = self._choose_enum(node, _enum_attributes(node))
        display["node"] = node
        display["dag_path"] = _dag_path(node)
        self._role["display"] = display
        self._refresh_role_snapshot()
        self._register_role_callbacks()

    def _set_duplicate_paths(self, paths):
        self._duplicate_paths = list(paths)
        if self._duplicate_button and cmds.control(self._duplicate_button, exists=True):
            cmds.button(
                self._duplicate_button, edit=True,
                label="选中重名骨骼（{0}）".format(len(self._duplicate_paths)),
                enable=bool(self._duplicate_paths))

    def _handle_duplicate_bones(self, error):
        diagnostic = duplicate_bone_diagnostic(error)
        self._set_duplicate_paths(diagnostic["duplicate_paths"])
        self._set_connected(False, diagnostic["summary"])
        self._show_error(diagnostic)

    def select_duplicate_bones(self):
        existing = [path for path in self._duplicate_paths if cmds.objExists(path)]
        if not existing:
            self._set_duplicate_paths([])
            self._set_connected(False, "重名骨骼已不存在，请重新设置角色")
            return
        cmds.select(existing, replace=True)
        self._set_connected(False, "已选中 {0} 个重名骨骼".format(len(existing)))

    def set_role(self):
        _require_maya()
        try:
            root = _selected_root()
            self._clear_role()
            bones = build_hierarchy(root, _maya_children, allow_duplicates=True)
            duplicates = duplicate_bone_paths(bones)
            for bone in bones:
                bone["dag_path"] = _dag_path(bone["path"])
            self._role = {"root": root, "bones": bones, "display": None}
            self._set_duplicate_paths([
                path for name in sorted(duplicates) for path in duplicates[name]
            ])
            self._set_text(self._root_text, "角色根骨骼：" + root)
            self._set_text(self._bone_text, "骨骼数：{0}".format(len(bones)))
            candidates = _display_candidates(root)
            if len(candidates) != 1:
                raise ValueError(
                    "无法唯一确定 Display_ctrl，请选择它并点击“手动选择 Display 控制器”。")
            self._configure_display(candidates[0])
            status = "角色已设置，可以连接"
            if duplicates:
                status += "（检测到 {0} 个重名骨骼，将由 UE 尝试映射）".format(
                    len(self._duplicate_paths))
            self._set_connected(False, status)
        except DuplicateBoneNamesError as exc:
            self._handle_duplicate_bones(exc)
        except (RuntimeError, ValueError) as exc:
            self._set_connected(False, str(exc))
            code = str(exc) if str(exc) in DIAGNOSTICS else "ROLE_SETUP_FAILED"
            self._show_error(make_diagnostic(code, str(exc), solution=str(exc), details=str(exc)))

    def set_display_controller(self):
        if not self._role:
            self._show_error(make_diagnostic(
                "INTERNAL_ERROR", "请先选择根骨骼并设置角色。"))
            return
        try:
            self._configure_display(self._selected_display())
            self._set_connected(False, "Display 控制器已设置，可以连接")
        except (RuntimeError, ValueError) as exc:
            code = str(exc) if str(exc) in DIAGNOSTICS else "ROLE_SETUP_FAILED"
            self._show_error(make_diagnostic(code, str(exc), solution=str(exc), details=str(exc)))

    def _refresh_role_snapshot(self):
        if not self._role or not self._role.get("display"):
            return
        subject = _capture_subject(self._role["root"])
        self._role["subject"] = subject
        self._role["outfit"] = _current_enum_label(self._role["display"])
        self._set_text(self._root_text, "角色根骨骼：" + self._role["root"])
        self._set_text(self._outfit_text, "当前衣服：" + self._role["outfit"])
        self._set_text(self._bone_text, "骨骼数：{0}".format(len(subject["bones"])))
        self._set_text(self._curve_text, "BlendShape 数：{0}".format(len(subject["curves"])))
        self._refresh_fps()

    def _register_role_callbacks(self):
        self._remove_callbacks(self._role_callback_ids)
        self._role_callback_ids = []
        if not self._role or not self._role.get("display"):
            return
        self._role_callback_ids.append(om.MNodeMessage.addNodeDestroyedCallback(
            self._role["bones"][0]["dag_path"].node(), self._on_role_destroyed))
        self._role_callback_ids.append(om.MNodeMessage.addNodeDestroyedCallback(
            self._role["display"]["dag_path"].node(), self._on_role_destroyed))
        self._role_callback_ids.append(om.MNodeMessage.addAttributeChangedCallback(
            self._role["display"]["dag_path"].node(), self._on_display_changed))

    def _on_display_changed(self, message, plug, other_plug, client_data):
        del other_plug, client_data
        if not self._role or not self._role.get("display"):
            return
        if not (message & om.MNodeMessage.kAttributeSet):
            return
        if plug.partialName(useLongNames=True) != self._role["display"]["attribute"]:
            return
        previous = self._role.get("outfit")
        current = _current_enum_label(self._role["display"])
        if current == previous:
            return
        if self._worker is not None:
            status = (
                "衣服已从 {0} 切换为 {1}。"
                "请在 UE 删除旧 Actor，放置新 Binding 后重新连接。"
            ).format(previous, current)
        else:
            status = "当前衣服已切换为 {0}。".format(current)
        self.disconnect(status=status)
        cmds.evalDeferred(self._refresh_after_outfit_change)

    def _refresh_after_outfit_change(self):
        try:
            self._refresh_role_snapshot()
        except DuplicateBoneNamesError as exc:
            self._handle_duplicate_bones(exc)
        except (RuntimeError, ValueError) as exc:
            self._set_connected(False, str(exc))

    def _on_time_unit_changed(self, *unused):
        fps = self._refresh_fps()
        if fps is None:
            if self._worker is not None:
                diagnostic = make_diagnostic("INVALID_FRAME_RATE")
                self.disconnect(status=diagnostic["summary"])
                self._show_error(diagnostic, session=True)
            return
        try:
            validate_frame_rate(fps)
        except ValueError as exc:
            if self._worker is not None:
                diagnostic = make_diagnostic("INVALID_FRAME_RATE", str(exc), details=str(exc))
                self.disconnect(status=diagnostic["summary"])
                self._show_error(diagnostic, session=True)
            return
        if self._worker is not None:
            self._restart_timer(fps)

    def connect(self):
        _require_maya()
        if self._worker is not None:
            return
        try:
            if self._role is None:
                self.set_role()
            if not self._role or not self._role.get("display"):
                return
            fps = validate_frame_rate(frames_per_second(cmds.currentUnit(query=True, time=True)))
            subject = _capture_subject(self._role["root"])
        except DuplicateBoneNamesError as exc:
            self._handle_duplicate_bones(exc)
            return
        except (RuntimeError, ValueError) as exc:
            code = str(exc) if str(exc) in DIAGNOSTICS else "INVALID_FRAME_RATE" \
                if "frame rate" in str(exc) or "time unit" in str(exc) else "INTERNAL_ERROR"
            diagnostic = make_diagnostic(code, str(exc), details=str(exc))
            self._set_connected(False, diagnostic["summary"])
            self._show_error(diagnostic)
            return
        init_message = make_init_message(
            [[bone["name"], bone["parent"]] for bone in subject["bones"]],
            [curve["name"] for curve in subject["curves"]])
        self._subject = subject
        self._worker = _SenderWorker(init_message)
        self._ready = False
        self._warning_shown = False
        self._session_error_shown = False
        self._last_warning = {"missing_in_unreal": [], "missing_in_maya": [],
                              "bone_name_remaps": [],
                              "has_warning": False}
        self._set_connected(False, "正在连接 Unreal…")
        self._worker.start()
        try:
            self._connection_callback_ids.append(
                om.MSceneMessage.addCallback(om.MSceneMessage.kBeforeNew, self._on_scene_change))
            self._connection_callback_ids.append(
                om.MSceneMessage.addCallback(om.MSceneMessage.kBeforeOpen, self._on_scene_change))
            self._connection_callback_ids.append(
                om.MSceneMessage.addCallback(om.MSceneMessage.kMayaExiting, self._on_scene_change))
            self._restart_timer(fps)
        except Exception as exc:
            diagnostic = make_diagnostic("INTERNAL_ERROR", str(exc), details=str(exc))
            self.disconnect(status=diagnostic["summary"])
            self._show_error(diagnostic, session=True)

    def _restart_timer(self, fps):
        timer_id, self._timer_id = self._timer_id, None
        if timer_id is not None:
            try:
                om.MMessage.removeCallback(timer_id)
            except RuntimeError:
                pass
        self._timer_id = om.MTimerMessage.addTimerCallback(1.0 / fps, self._on_timer)

    def _on_timer(self, elapsed, last_time, client_data):
        del elapsed, last_time, client_data
        worker = self._worker
        if worker is None:
            return
        state, detail, warning, diagnostic = worker.status()
        if state == "error":
            diagnostic = diagnostic or make_diagnostic("INTERNAL_ERROR", detail)
            self.disconnect(status=diagnostic["summary"])
            self._show_error(diagnostic, session=True)
            return
        if state != "ready":
            self._set_connected(False, "正在连接 Unreal…")
            return
        if not self._ready:
            self._ready = True
            self._last_warning = warning
            status = "已连接（有警告）" if warning["has_warning"] else "已连接"
            self._set_connected(True, status)
            if warning["has_warning"] and not self._warning_shown \
                    and cmds.checkBox(self._warning_checkbox, query=True, value=True):
                self._warning_shown = True
                self._show_warning(warning)
        try:
            transforms, curves = _sample_pose(self._subject)
            worker.submit(make_frame_message(transforms, curves))
        except (RuntimeError, ValueError) as exc:
            diagnostic = make_diagnostic("SAMPLING_FAILED", str(exc), details=str(exc))
            self.disconnect(status=diagnostic["summary"])
            self._show_error(diagnostic, session=True)

    def _diagnostic_text(self, diagnostic=None):
        diagnostic = diagnostic or self._last_diagnostic
        role = self._role or {}
        subject = self._subject or role.get("subject") or {}
        warning = self._last_warning
        fps = self._refresh_fps()
        sections = [
            "摘要：{0}".format(diagnostic.get("summary", "—")),
            "解决办法：{0}".format(diagnostic.get("solution", "—")),
            "Error code: {0}".format(diagnostic.get("code", "—")),
            "Maya root: {0}".format(role.get("root", "—")),
            "Outfit: {0}".format(role.get("outfit", "—")),
            "FPS: {0}".format(format_fps(fps) if fps else "—"),
            "Bones: {0}".format(len(subject.get("bones", []))),
            "BlendShapes: {0}".format(len(subject.get("curves", []))),
            "Maya only: {0}".format(", ".join(warning.get("missing_in_unreal", [])) or "—"),
            "Unreal only: {0}".format(", ".join(warning.get("missing_in_maya", [])) or "—"),
            "Bone name remaps: {0}".format(
                ", ".join(warning.get("bone_name_remaps", [])) or "—"),
            "Meshes:\n{0}".format("\n".join(subject.get("meshes", [])) or "—"),
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
        if warning["bone_name_remaps"] and self._duplicate_paths:
            diagnostic["duplicate_paths"] = list(self._duplicate_paths)
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
        if session and self._session_error_shown:
            return
        if session:
            self._session_error_shown = True
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

    def _on_role_destroyed(self, *unused):
        was_connected = self._worker is not None
        self._clear_role()
        if was_connected:
            diagnostic = make_diagnostic(
                "SAMPLING_FAILED",
                "角色根骨骼或 Display 控制器已被删除或卸载。")
            cmds.evalDeferred(lambda: self._show_error(diagnostic, session=True))

    def _remove_callbacks(self, callback_ids):
        for callback_id in list(callback_ids):
            try:
                om.MMessage.removeCallback(callback_id)
            except RuntimeError:
                pass

    def disconnect(self, keep_error=False, status="已断开连接"):
        del keep_error
        timer_id, self._timer_id = self._timer_id, None
        if timer_id is not None:
            try:
                om.MMessage.removeCallback(timer_id)
            except RuntimeError:
                pass
        self._remove_callbacks(self._connection_callback_ids)
        self._connection_callback_ids = []
        worker, self._worker = self._worker, None
        if worker is not None:
            worker.stop()
            worker.join(1.0)
        self._subject = None
        self._ready = False
        self._set_connected(False, status)

    def _clear_role(self):
        self.disconnect(status="未设置角色")
        self._remove_callbacks(self._role_callback_ids)
        self._role_callback_ids = []
        self._role = None
        self._set_text(self._root_text, "角色根骨骼：—")
        self._set_text(self._outfit_text, "当前衣服：—")
        self._set_text(self._bone_text, "骨骼数：0")
        self._set_text(self._curve_text, "BlendShape 数：0")
        self._set_duplicate_paths([])

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
