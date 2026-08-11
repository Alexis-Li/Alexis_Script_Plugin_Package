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

__version__ = "0.1.0"
PROTOCOL_VERSION = 1
HOST = "127.0.0.1"
PORT = 54321
SUBJECT_NAME = "MtoU_Character"
CURVE_VALUE_TOLERANCE = 1.0e-6

try:
    import maya.api.OpenMaya as om
    import maya.cmds as cmds
except ImportError:
    om = None
    cmds = None


def normalize_name(path):
    return path.rsplit("|", 1)[-1].rsplit(":", 1)[-1]


def build_hierarchy(root, children):
    records = []
    stack = [(root, -1)]
    while stack:
        path, parent = stack.pop()
        index = len(records)
        records.append({"path": path, "name": normalize_name(path), "parent": parent})
        child_paths = list(children(path) or [])
        stack.extend((child, index) for child in reversed(child_paths))
    counts = {}
    for record in records:
        counts[record["name"]] = counts.get(record["name"], 0) + 1
    duplicates = sorted(name for name, count in counts.items() if count > 1)
    if duplicates:
        raise ValueError("duplicate normalized bone names: {0}".format(", ".join(duplicates)))
    return records


def centimeters_per_unit(unit):
    factors = {"mm": 0.1, "cm": 1.0, "m": 100.0, "km": 100000.0,
               "in": 2.54, "ft": 30.48, "yd": 91.44, "mi": 160934.4}
    if unit not in factors:
        raise ValueError("unsupported Maya linear unit: {0}".format(unit))
    return factors[unit]


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
    return json.loads(_recv_exact(sock, size).decode("utf-8"))


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


def _discover_curve_plugs(bone_paths):
    skin_clusters = set()
    for path in bone_paths:
        skin_clusters.update(cmds.listConnections(path, type="skinCluster") or [])
    deformers = set()
    for skin_cluster in skin_clusters:
        for geometry in cmds.skinCluster(skin_cluster, query=True, geometry=True) or []:
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


def _capture_subject():
    _require_maya()
    root = _selected_root()
    bones = build_hierarchy(root, _maya_children)
    for bone in bones:
        bone["dag_path"] = _dag_path(bone["path"])
    return {"root": root, "bones": bones,
            "curves": _discover_curve_plugs([bone["path"] for bone in bones]),
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
        self._missing_curves = []

    def submit(self, frame_message):
        self._latest.put(encode_message(frame_message))

    def status(self):
        with self._lock:
            return self._state, self._detail, list(self._missing_curves)

    def _set_status(self, state, detail, missing_curves=None):
        with self._lock:
            self._state = state
            self._detail = detail
            self._missing_curves = list(missing_curves or [])

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
        try:
            sock = self._connect()
            if sock is None or self._stop_event.is_set():
                return
            if not self._send_initial(sock):
                return
            sock.settimeout(5.0)
            reply = recv_message(sock)
            if reply.get("type") == "error":
                raise RuntimeError(reply.get("message") or "Unreal rejected the connection")
            if reply.get("type") != "ready":
                raise RuntimeError("expected ready, received {0}".format(reply.get("type")))
            self._set_status("ready", "Connected", reply.get("missing_curves") or [])
            sock.settimeout(0.25)
            while not self._stop_event.is_set():
                packet = self._latest.take()
                if packet is not None:
                    sock.sendall(packet)
                readable, _, _ = select.select([sock], [], [], 0.01)
                if readable and not sock.recv(1):
                    raise EOFError("Unreal closed the connection")
        except Exception as exc:
            if not self._stop_event.is_set():
                self._set_status("error", str(exc))
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
        self._callback_ids = []
        self._timer_id = None
        self._ready = False
        self._root_text = None
        self._status_text = None
        self._error_text = None

    def build_ui(self):
        cmds.window(WINDOW_NAME, title="MtoU Live Link", closeCommand=self.close)
        cmds.columnLayout(adjustableColumn=True)
        self._root_text = cmds.text(label="Root: None", align="left")
        cmds.button(label="Connect", command=lambda *_: self.connect())
        cmds.button(label="Disconnect", command=lambda *_: self.disconnect())
        self._status_text = cmds.text(label="Disconnected", align="left")
        self._error_text = cmds.text(label="", align="left")
        cmds.showWindow(WINDOW_NAME)

    def _set_text(self, control, text):
        if control and cmds.control(control, exists=True):
            cmds.text(control, edit=True, label=text)

    def _set_status(self, text):
        self._set_text(self._status_text, text)

    def _set_error(self, text):
        self._set_text(self._error_text, text)

    def connect(self):
        _require_maya()
        if self._worker is not None:
            return
        try:
            subject = _capture_subject()
        except ValueError as exc:
            self._set_status("Disconnected")
            self._set_error(str(exc))
            return
        init_message = make_init_message(
            [[bone["name"], bone["parent"]] for bone in subject["bones"]],
            [curve["name"] for curve in subject["curves"]],
        )
        self._subject = subject
        self._worker = _SenderWorker(init_message)
        self._ready = False
        self._set_text(self._root_text, "Root: {0}".format(subject["root"]))
        self._set_error("")
        self._set_status("Connecting to Unreal...")
        self._worker.start()
        try:
            self._callback_ids.append(
                om.MSceneMessage.addCallback(om.MSceneMessage.kBeforeNew, self._on_scene_change))
            self._callback_ids.append(
                om.MSceneMessage.addCallback(om.MSceneMessage.kBeforeOpen, self._on_scene_change))
            self._callback_ids.append(
                om.MSceneMessage.addCallback(om.MSceneMessage.kMayaExiting, self._on_scene_change))
            self._callback_ids.append(
                om.MNodeMessage.addNodeDestroyedCallback(subject["bones"][0]["dag_path"].node(),
                                                         self._on_root_destroyed))
            self._timer_id = om.MTimerMessage.addTimerCallback(1.0 / 30.0, self._on_timer)
        except Exception as exc:
            self._set_error(str(exc))
            self.disconnect(keep_error=True)

    def _on_timer(self, elapsed, last_time, client_data):
        worker = self._worker
        if worker is None:
            return
        state, detail, missing_curves = worker.status()
        if state == "error":
            self._set_error(detail)
            self.disconnect(keep_error=True)
            return
        if state != "ready":
            self._set_status(detail)
            return
        if not self._ready:
            self._ready = True
            if missing_curves:
                detail = "Connected (missing Morph Targets: {0})".format(
                    ", ".join(missing_curves))
            self._set_status(detail)
        try:
            transforms, curves = _sample_pose(self._subject)
            worker.submit(make_frame_message(transforms, curves))
        except (RuntimeError, ValueError) as exc:
            self._set_error(str(exc))
            self.disconnect(keep_error=True)

    def _on_scene_change(self, *unused):
        self.disconnect()

    def _on_root_destroyed(self, *unused):
        self.disconnect()

    def disconnect(self, keep_error=False):
        timer_id, self._timer_id = self._timer_id, None
        if timer_id is not None:
            try:
                om.MMessage.removeCallback(timer_id)
            except RuntimeError:
                pass
        callback_ids, self._callback_ids = self._callback_ids, []
        for callback_id in callback_ids:
            try:
                om.MMessage.removeCallback(callback_id)
            except RuntimeError:
                pass
        worker, self._worker = self._worker, None
        if worker is not None:
            worker.stop()
            worker.join(1.0)
        self._subject = None
        self._ready = False
        self._set_text(self._root_text, "Root: None")
        self._set_status("Disconnected")
        if not keep_error:
            self._set_error("")

    def close(self, *unused):
        self.disconnect()


_CONTROLLER = None
WINDOW_NAME = "MtoULiveLinkWindow"


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
