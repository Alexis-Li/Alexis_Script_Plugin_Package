"""Maya side of the camera sync prototype: follower and entry point.

The Unreal publisher owns the camera, the time and the output resolution. This
module connects to it, applies each frame to one disposable Maya camera, moves
Maya's current time to the reported position and answers with the values it
actually holds, plus Maya's own marker NDC for comparison.

Safety contract (see the prototype README):

* the scene is never saved; no keys, no playback range and no frame rate change;
* every camera edit and the time move happen inside one undo chunk, and a failed
  frame is undone, so a frame is never partially applied;
* ``stop()`` restores the Maya current time captured at session start, removes
  the camera this session created and detaches any scriptJob it added.

Maya's API is reached through ``maya.cmds`` only, imported lazily so the module
can be inspected without a running Maya.
"""

import argparse
import importlib.util
import json
import math
import os
import select
import socket
import sys
import time
import traceback
from pathlib import Path

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 54330
DEFAULT_CAMERA_NAME = "MtoU_UE_Camera"

STATE_CONNECTING = "connecting"
STATE_FOLLOWING = "following"
STATE_STOPPED = "stopped"
STATE_FAILED = "failed"

STATUS_APPLIED = "applied"
STATUS_REJECTED = "rejected"

CATEGORY_UNSUPPORTED_SCENE_UNIT = "UNSUPPORTED_SCENE_LINEAR_UNIT"
CATEGORY_UNSUPPORTED_TIME_UNIT = "UNSUPPORTED_SCENE_TIME_UNIT"
CATEGORY_CAMERA_NAME_CONFLICT = "CAMERA_NAME_CONFLICT"
CATEGORY_TRANSPORT = "TRANSPORT_ERROR"
CATEGORY_HANDSHAKE = "HANDSHAKE_FAILED"
CATEGORY_CAMERA_INPUT_DRIVEN = "CAMERA_INPUT_DRIVEN"
CATEGORY_LOCKED_ATTRIBUTE = "LOCKED_SYNC_ATTRIBUTE"
CATEGORY_HOST_STATE_MISMATCH = "HOST_STATE_MISMATCH"

RESOLUTION_PLUGS = ("defaultResolution.width", "defaultResolution.height",
                    "defaultResolution.pixelAspect", "defaultResolution.deviceAspectRatio")
TRANSFORM_WRITE_ATTRIBUTES = ("translateX", "translateY", "translateZ",
                              "rotateX", "rotateY", "rotateZ",
                              "scaleX", "scaleY", "scaleZ",
                              "shearXY", "shearXZ", "shearYZ")

# Maya time units that are frame rates, and the frames per second they mean.
TIME_UNIT_FPS = {
    "game": 30.0,
    "film": 24.0,
    "pal": 25.0,
    "ntsc": 30.0,
    "show": 48.0,
    "palf": 50.0,
    "ntscf": 60.0,
}

# Attributes this prototype owns on the synced camera shape.
MANAGED_ATTRIBUTES = (
    "focalLength",
    "horizontalFilmAperture",
    "verticalFilmAperture",
    "horizontalFilmOffset",
    "verticalFilmOffset",
    "filmFit",
    "lensSqueezeRatio",
    "fStop",
    "focusDistance",
    "depthOfField",
    "nearClipPlane",
    "farClipPlane",
    "displayResolution",
    "displayGateMask",
)

_cmds_module = None
_mapping_module = None


def cmds():
    """``maya.cmds``, imported on first use."""
    global _cmds_module
    if _cmds_module is None:
        import maya.cmds as maya_cmds
        _cmds_module = maya_cmds
    return _cmds_module


def mapping():
    """The pure mapping module, loaded from this script's directory."""
    global _mapping_module
    if _mapping_module is None:
        path = Path(__file__).resolve().parent / "mtou_camera_sync_mapping.py"
        spec = importlib.util.spec_from_file_location(
            "mtou_camera_sync_mapping_loaded", str(path))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _mapping_module = module
    return _mapping_module


class SyncRefused(RuntimeError):
    """A configuration the prototype refuses instead of guessing."""

    def __init__(self, category, detail):
        RuntimeError.__init__(self, "{0}: {1}".format(category, detail))
        self.category = category
        self.detail = detail


def time_unit_for_fps(fps):
    """Maya time unit name for a frames per second value."""
    fps = float(fps)
    for unit, unit_fps in TIME_UNIT_FPS.items():
        if abs(unit_fps - fps) < 1e-9:
            return unit
    return "{0}fps".format(fps)


def scene_time_unit():
    """(unit name, frames per second) of the current Maya scene.

    Refuses units that are not frame rates (seconds, minutes, hours) so the
    prototype never has to guess what a Maya frame means.
    """
    unit = str(cmds().currentUnit(query=True, time=True))
    key = unit.strip().lower()
    if key in TIME_UNIT_FPS:
        return (unit, TIME_UNIT_FPS[key])
    if key.endswith("fps"):
        try:
            fps = float(key[:-3])
        except ValueError:
            fps = 0.0
        if fps > 0.0:
            return (unit, fps)
    raise SyncRefused(
        CATEGORY_UNSUPPORTED_TIME_UNIT,
        "scene time unit {0!r} is not a frame rate; set a frame rate (film, "
        "pal, ntsc, game, show, palf, ntscf or <n>fps) before following".format(unit))


def scene_linear_unit():
    """Maya scene linear unit, refused unless it is centimetres."""
    unit = str(cmds().currentUnit(query=True, linear=True))
    if unit.strip().lower() != "cm":
        raise SyncRefused(
            CATEGORY_UNSUPPORTED_SCENE_UNIT,
            "scene linear unit is {0!r}; Unreal sends centimetres and the "
            "prototype does not rescale".format(unit))
    return unit


def _require_writable(plug):
    """Refuse protected plugs, including compound locks, without unlocking them."""
    node, attribute = plug.rsplit(".", 1)
    while attribute:
        if cmds().getAttr(node + "." + attribute, lock=True):
            raise SyncRefused(CATEGORY_LOCKED_ATTRIBUTE,
                              "{0}.{1} is locked; use a disposable unlocked camera/gate".format(
                                  node, attribute))
        parents = cmds().attributeQuery(attribute, node=node, listParent=True) or []
        attribute = parents[0] if parents else None
    if cmds().lockNode(node, query=True, lock=True)[0]:
        raise SyncRefused(CATEGORY_LOCKED_ATTRIBUTE, "{0} is locked".format(node))
    if not cmds().getAttr(plug, settable=True):
        raise SyncRefused(CATEGORY_CAMERA_INPUT_DRIVEN,
                          "{0} has an input driver; use a disposable camera/gate".format(plug))


class CameraSyncFollower(object):
    """Applies Unreal camera frames to one disposable Maya camera."""

    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT,
                 camera_name=DEFAULT_CAMERA_NAME, connect_timeout=10.0,
                 maya_origin_frame=None, pose_node=None):
        self.host = host
        self.port = int(port)
        self.camera_name = camera_name
        self.connect_timeout = float(connect_timeout)
        self.maya_origin_frame = (None if maya_origin_frame is None
                                  else float(maya_origin_frame))
        self.pose_node = pose_node
        self.state = STATE_STOPPED
        self.detail = ""
        self.error = None
        self.session = None
        self.marker_names = []
        self.frames_applied = 0
        self.frames_rejected = 0
        self.frames_subframe = 0
        self.frames_unchanged = 0
        self.last_report = None
        self.last_frame_serial = None
        self.reports = []
        self.max_reports = 64
        self.max_marker_delta = None
        self.far_clip_substitutions = 0
        self._last_signature = None
        self._socket = None
        self._buffer = b""
        self._peer_closed = False
        self._pending = []
        self._start_time = None
        self._scene_fps = None
        self._time_unit = None
        self._camera_transform = None
        self._camera_shape = None
        self._created_camera = False
        self._script_job = None
        self._scene_resolution_before = None
        self._reused_camera_before = None
        self.idle_pump_note = ""
        self.server_errors = []
        self._undo_state = None
        self._idle_errors = []

    # ---------------------------------------------------------------- session

    @property
    def connected(self):
        return self._socket is not None

    @property
    def camera_nodes(self):
        """The session camera ``(transform, shape)``, or ``(None, None)``."""
        return (self._camera_transform, self._camera_shape)

    def hello_message(self):
        return {
            "type": "hello",
            "protocol": mapping().PROTOCOL_NAME,
            "version": mapping().PROTOCOL_VERSION,
            "host": self.host,
            "scene_fps": self._scene_fps,
            "time_unit": self._time_unit,
            "maya_origin_frame": self.maya_origin_frame,
        }

    def connect(self):
        """Connect with rollback even when setup or the handshake raises."""
        try:
            return self._connect()
        except BaseException as error:
            if self.state != STATE_FAILED:
                self._fail(str(error))
            self.stop()
            raise

    def _connect(self):
        """Check the scene, connect, send ``hello`` and read the session."""
        if self.connected:
            return self.session
        self.state = STATE_CONNECTING
        self.detail = ""
        self.error = None
        unit = scene_linear_unit()
        time_unit, fps = scene_time_unit()
        self._time_unit = time_unit
        self._scene_fps = fps
        self._start_time = float(cmds().currentTime(query=True))
        self._undo_state = bool(cmds().undoInfo(query=True, state=True))
        if not self._undo_state:
            # The frame transaction needs undo; the previous state is restored
            # by stop().
            cmds().undoInfo(stateWithoutFlush=True)
        try:
            self._socket = socket.create_connection(
                (self.host, self.port), timeout=self.connect_timeout)
            self._socket.setblocking(False)
        except OSError as error:
            self._socket = None
            self._fail("{0}: {1}".format(CATEGORY_TRANSPORT, error))
            raise SyncRefused(CATEGORY_TRANSPORT,
                              "cannot connect to {0}:{1}: {2}".format(
                                  self.host, self.port, error))
        self._buffer = b""
        self._peer_closed = False
        try:
            self._send(self.hello_message())
        except OSError as error:
            # The publisher can reset the connection while this client is still
            # introducing itself; that is a transport failure, not a crash.
            detail = "{0}: {1}".format(CATEGORY_TRANSPORT, error)
            self._fail(detail)
            self.stop()
            raise SyncRefused(CATEGORY_TRANSPORT, detail)
        deadline = time.time() + self.connect_timeout
        while time.time() < deadline:
            try:
                messages = self._receive(0.05)
            except mapping().PayloadError as error:
                self._fail("{0}: {1}".format(CATEGORY_HANDSHAKE, error))
                self.stop()
                raise SyncRefused(CATEGORY_HANDSHAKE,
                                  "undecodable server line: {0}".format(error))
            except OSError as error:
                # A publisher that ends a session and closes the socket immediately can
                # reset it while the client is still reading, so a reconnect attempt can
                # fail here as well as in pump(). Either way it is a transport failure,
                # not a crash.
                detail = "{0}: {1}".format(CATEGORY_TRANSPORT, error)
                self._fail(detail)
                self.stop()
                raise SyncRefused(CATEGORY_TRANSPORT, detail)
            if messages:
                # Frames may share a batch with the session; they are applied
                # here rather than dropped.
                self._ingest(messages)
            self._handle_closed_peer()
            if self.state == STATE_FOLLOWING:
                return self.session
            if self.state == STATE_STOPPED:
                # The publisher can end the session inside the handshake (a user stopping
                # follow right after Maya connects). The connection was accepted and then
                # released, so this attempt is over; waiting for a session that will
                # never come would only turn it into a timeout.
                raise SyncRefused(CATEGORY_HANDSHAKE,
                                  "{0}: the publisher ended the session during the "
                                  "handshake ({1})".format(CATEGORY_HANDSHAKE, self.detail))
            if self.state == STATE_FAILED:
                detail = self.error
                self.stop()
                # A failure raised by a report send is a transport failure even though
                # it interrupted the handshake.
                raise SyncRefused(
                    CATEGORY_TRANSPORT if CATEGORY_TRANSPORT in detail
                    else CATEGORY_HANDSHAKE, detail)
        self._fail("{0}: no session message within {1}s".format(
            CATEGORY_HANDSHAKE, self.connect_timeout))
        self.stop()
        raise SyncRefused(CATEGORY_HANDSHAKE,
                          "no session message within {0}s".format(self.connect_timeout))

    def _ingest(self, messages):
        """Handle decoded messages; the session may arrive after a frame."""
        for message in messages:
            if self.state == STATE_CONNECTING:
                if message["type"] == "session":
                    self._accept_session(message)
                    continue
                if message["type"] == "frame":
                    self._pending.append(message)
                    continue
                if message["type"] in ("error", "end"):
                    self._handle(message)
                    if message["type"] == "end":
                        self._fail("the publisher ended the session before it "
                                   "started: {0}".format(message.get("reason")))
                    else:
                        # A publisher that refuses this connection (one client at a
                        # time) will never send a session, so waiting for one until the
                        # timeout would make every reconnect attempt ten seconds slower
                        # than it has to be.
                        self._fail("{0}: the publisher refused this connection: "
                                   "{1}".format(CATEGORY_HANDSHAKE,
                                                message.get("category")))
                    continue
            self._handle(message)
        while self.state == STATE_FOLLOWING and self._pending:
            self._handle(self._pending.pop(0))

    def _accept_session(self, message):
        self.session = message
        if self.maya_origin_frame is None:
            self.maya_origin_frame = float(message["playback_range"]["start"])
        self.marker_names = list(message.get("marker_names") or [])
        self.state = STATE_FOLLOWING
        self.detail = "following on port {0}".format(self.port)

    def _handle_closed_peer(self):
        """A closed stream is only a failure when the publisher did not end the session."""
        if not self._peer_closed:
            return
        self._peer_closed = False
        if self.state != STATE_STOPPED:
            self._fail("{0}: the publisher closed the connection".format(CATEGORY_TRANSPORT))

    def pump(self, max_messages=16):
        """Process pending server messages. Safe to call from an idle event."""
        if not self.connected:
            return 0
        try:
            messages = self._receive(0.0, max_messages)
        except mapping().PayloadError as error:
            self._reject_undecodable(error)
            return 1
        except OSError as error:
            if self.state != STATE_STOPPED:
                self._fail("{0}: {1}".format(CATEGORY_TRANSPORT, error))
            return 0
        if messages:
            self._ingest(messages)
        self._handle_closed_peer()
        return len(messages)

    def _reject_undecodable(self, error):
        """A line the validator refused: report it, keep the session usable."""
        self.frames_rejected += 1
        report = {
            "type": "applied",
            "session": (self.session or {}).get("sequence"),
            "sequence": None,
            "frame_serial": None,
            "eval_serial": None,
            "eval_identity": None,
            "maya_frame": None,
            "camera": {},
            "status": STATUS_REJECTED,
            "detail": ["rejected: {0}".format(error)],
            "markers": [],
        }
        self.last_report = report
        self.reports.append(report)
        if len(self.reports) > self.max_reports:
            del self.reports[0:len(self.reports) - self.max_reports]
        try:
            self._send(report)
        except OSError:
            pass

    def abort(self, reason="client dropped the connection"):
        """Drop the transport without the ``bye`` handshake.

        This is what a client that crashes or loses its network looks like to the
        publisher: a closed socket instead of a protocol goodbye. The follower's
        session state is untouched, so ``stop()`` still releases the scene; only
        the caller decides whether the session is over.
        """
        if self._socket is not None:
            try:
                self._socket.close()
            except OSError:
                pass
        self._socket = None
        self._buffer = b""
        self._peer_closed = False
        self._pending = []
        self.detail = reason
        return reason

    def stop(self, reason="client stopped"):
        """Restore the timeline and remove everything this session created."""
        summary = {"reason": reason}
        if self.connected:
            try:
                self._send({"type": "bye"})
            except OSError:
                pass
            try:
                self._socket.close()
            except OSError:
                pass
        self._socket = None
        self._buffer = b""
        self._peer_closed = False
        self._pending = []
        self.detach_idle_pump()
        if self._start_time is not None:
            try:
                cmds().currentTime(self._start_time)
                summary["restored_time"] = self._start_time
            except RuntimeError as error:
                summary["restore_error"] = str(error)
            self._start_time = None
        if self._created_camera and self._camera_transform:
            if cmds().objExists(self._camera_transform):
                try:
                    cmds().delete(self._camera_transform)
                    summary["camera_removed"] = self._camera_transform
                except RuntimeError as error:
                    summary["camera_remove_error"] = str(error)
        elif self._reused_camera_before is not None:
            self._restore_reused_camera(summary)
        self._camera_transform = None
        self._camera_shape = None
        self._created_camera = False
        if self._scene_resolution_before is not None:
            self._restore_scene_resolution(summary)
        if self._undo_state is False:
            try:
                cmds().undoInfo(stateWithoutFlush=False)
            except RuntimeError:
                pass
        self._undo_state = None
        self.session = None
        self._last_signature = None
        if self.state != STATE_FAILED:
            self.state = STATE_STOPPED
            self.detail = reason
        return summary

    def _fail(self, detail):
        self.state = STATE_FAILED
        self.error = detail
        self.detail = detail

    # ------------------------------------------------------------- transport

    def _receive(self, timeout, max_messages=64):
        """Decode complete lines. Raises OSError on a lost connection.

        When the publisher closes the connection with complete lines already read, those
        lines are returned instead of being thrown away: the closing line is among them,
        and it is what tells this client the session ended on purpose rather than being
        lost. ``_peer_closed`` records that the stream is over.
        """
        messages = []
        if self._socket is None:
            return messages
        deadline = time.time() + timeout
        while len(messages) < max_messages:
            while b"\n" in self._buffer:
                line, self._buffer = self._buffer.split(b"\n", 1)
                if not line.strip():
                    continue
                messages.append(mapping().decode_message(line.decode("utf-8")))
                if len(messages) >= max_messages:
                    return messages
            remaining = max(0.0, deadline - time.time())
            readable, _, _ = select.select([self._socket], [], [], remaining)
            if not readable:
                return messages
            chunk = self._socket.recv(65536)
            if not chunk:
                self._peer_closed = True
                if messages:
                    return messages
                raise OSError("publisher closed the connection")
            self._buffer += chunk
            deadline = time.time() + timeout
        return messages

    def _send(self, message):
        if self._socket is None:
            raise OSError("not connected")
        self._socket.setblocking(True)
        try:
            self._socket.sendall(
                (json.dumps(message, ensure_ascii=True) + "\n").encode("utf-8"))
        finally:
            if self._socket is not None:
                self._socket.setblocking(False)

    # ---------------------------------------------------------------- frames

    def _send_report(self, report):
        """Make one report best-effort: a lost connection is a failure, not a raise.

        A publisher that closed the connection (or reset it) while a frame was being
        applied must be reported as a transport failure; the caller's retry loop is
        what decides whether the session continues.
        """
        try:
            self._send(report)
            return True
        except OSError as error:
            self._fail("{0}: {1}".format(CATEGORY_TRANSPORT, error))
            return False

    def _handle(self, message):
        kind = message["type"]
        if kind == "frame":
            report = self.apply_frame(message)
            self._send_report(report)
        elif kind == "end":
            self.detail = "publisher ended the session: {0}".format(
                message.get("reason"))
            self.stop(self.detail)
            self.state = STATE_STOPPED
        elif kind == "error":
            # protocol.md: the publisher answers a refused client message with an
            # error line and keeps the session; it is a report, not a failure.
            entry = {"category": message.get("category"), "detail": message.get("detail")}
            self.server_errors.append(entry)
            self.detail = "publisher refused a message: {0}: {1}".format(
                entry["category"], entry["detail"])
        elif kind == "session":
            self.session = message
            self.marker_names = list(message.get("marker_names") or [])

    def ensure_camera(self):
        """The session camera transform and shape, created once and reused."""
        if self._camera_transform and cmds().objExists(self._camera_transform):
            self._validate_camera_inputs(self._camera_transform, self._camera_shape)
            return (self._camera_transform, self._camera_shape)
        name = self.camera_name
        if cmds().objExists(name):
            shapes = cmds().listRelatives(name, shapes=True, type="camera") or []
            if not shapes:
                raise SyncRefused(
                    CATEGORY_CAMERA_NAME_CONFLICT,
                    "{0!r} exists and is not a camera; refusing to reuse it".format(name))
            self._validate_camera_inputs(name, shapes[0])
            self._camera_transform = name
            self._camera_shape = shapes[0]
            self._created_camera = False
            self._capture_reused_camera()
        else:
            self._camera_transform = cmds().createNode("transform", name=name)
            self._camera_shape = cmds().createNode(
                "camera", name=name + "Shape", parent=self._camera_transform)
            self._created_camera = True
        return (self._camera_transform, self._camera_shape)

    def _validate_camera_inputs(self, transform, shape):
        """Borrow only static cameras and static parents; never sever a driver."""
        nodes = [shape]
        node = transform
        while node:
            nodes.append(node)
            parents = cmds().listRelatives(node, parent=True, fullPath=True) or []
            node = parents[0] if parents else None
        for node in nodes:
            connections = cmds().listConnections(
                node, source=True, destination=False, plugs=True, connections=True) or []
            for plug in connections[::2]:
                if cmds().getAttr(plug, type=True) != "message":
                    raise SyncRefused(CATEGORY_CAMERA_INPUT_DRIVEN,
                                      "{0} has an input driver; choose another camera name "
                                      "to create a disposable camera".format(plug))
        for attribute in TRANSFORM_WRITE_ATTRIBUTES:
            _require_writable(transform + "." + attribute)
        for attribute in MANAGED_ATTRIBUTES:
            _require_writable(shape + "." + attribute)

    def _capture_reused_camera(self):
        """Remember the state of a camera this session borrows instead of creating.

        The session only ever deletes a camera it created. A node that already
        carries the session name is reused, so its managed attributes and its
        world placement are captured here and written back by ``stop()``: a
        borrowed node has to look the same after the session as it did before.
        """
        if self._reused_camera_before is not None or not self._camera_shape:
            return
        state = {"attributes": {}}
        for attribute in MANAGED_ATTRIBUTES:
            state["attributes"][attribute] = cmds().getAttr(
                "{0}.{1}".format(self._camera_shape, attribute))
        state["world_matrix"] = [float(value) for value in
                                 cmds().xform(self._camera_transform, query=True,
                                              worldSpace=True, matrix=True)]
        self._reused_camera_before = state

    def _restore_reused_camera(self, summary):
        """Write the borrowed camera's attributes and placement back."""
        state = self._reused_camera_before
        self._reused_camera_before = None
        if not state or not self._camera_shape or not cmds().objExists(self._camera_shape):
            summary["reused_camera_restored"] = False
            return
        try:
            self._validate_camera_inputs(self._camera_transform, self._camera_shape)
            for attribute, value in sorted(state["attributes"].items()):
                plug = "{0}.{1}".format(self._camera_shape, attribute)
                cmds().setAttr(plug, value)
            cmds().xform(self._camera_transform, worldSpace=True,
                         matrix=list(state["world_matrix"]))
            summary["reused_camera_restored"] = self._camera_shape
        except (RuntimeError, SyncRefused) as error:
            summary["reused_camera_restore_error"] = str(error)

    def _capture_scene_resolution(self):
        """Remember the scene gate the first frame is about to overwrite."""
        if self._scene_resolution_before is not None:
            return
        state = {}
        for plug in RESOLUTION_PLUGS:
            state[plug] = cmds().getAttr(plug)
        self._scene_resolution_before = state

    def _restore_scene_resolution(self, summary):
        """Put the scene's own resolution gate back."""
        state = self._scene_resolution_before
        self._scene_resolution_before = None
        try:
            for plug in state:
                _require_writable(plug)
            for plug, value in sorted(state.items()):
                cmds().setAttr(plug, value)
            summary["resolution_restored"] = dict(state)
        except (RuntimeError, SyncRefused) as error:
            summary["resolution_restore_error"] = str(error)

    def apply_frame(self, frame, session=None):
        """Apply one frame atomically. Returns the ``applied`` report payload.

        A frame that fails validation or application is reported as
        ``rejected`` and leaves the camera untouched: all Maya writes and the
        time move share one undo chunk that is undone on failure, so a frame is
        never partially applied. A frame whose state is identical to the last
        applied one (the publisher's heartbeat) leaves Maya untouched as well.
        """
        mapping_module = mapping()
        frame_serial = frame.get("frame_serial") if isinstance(frame, dict) else None
        report = {
            "type": "applied",
            "session": frame.get("session") if isinstance(frame, dict) else None,
            "sequence": frame.get("sequence") if isinstance(frame, dict) else None,
            "frame_serial": frame_serial,
            "eval_serial": frame.get("eval_serial") if isinstance(frame, dict) else None,
            "eval_identity": frame.get("eval_identity") if isinstance(frame, dict) else None,
            "maya_frame": None,
            "camera": {},
            "status": STATUS_REJECTED,
            "detail": [],
            "markers": [],
        }
        detail = report["detail"]
        try:
            frame = mapping_module.validate_message(frame, expected_type="frame")
            session = session if session is not None else (self.session or {})
            # Maya has a single gate, while Unreal renders the film aperture inside the
            # output resolution and leaves bars where the two aspects differ. The gate
            # therefore follows the aperture pixels the publisher reports, which is what
            # makes Maya's derived axis match Unreal's rendered extent.
            gate = frame.get("aperture_resolution") or frame["output_resolution"]
            resolution = (float(gate["x"]), float(gate["y"]))
            camera_payload = frame["camera"]
            basis = mapping_module.payload_basis(camera_payload)
            location = mapping_module.payload_location(camera_payload)
            matrix = mapping_module.ue_basis_to_maya_matrix(
                basis[0], basis[1], basis[2], location)
            attributes, notes = mapping_module.camera_parameters(
                camera_payload, session, resolution, frame)
            time_payload = frame["time"]
            display_rate = (time_payload.get("display_rate")
                            or session.get("display_rate"))
            playback_range = session.get("playback_range") or {}
            playback_start = float(playback_range.get("start", 0.0))
            maya_time, subframe = mapping_module.maya_time_for_frame(
                time_payload["display_frame"], playback_start, display_rate,
                self._scene_fps, self.maya_origin_frame if self.maya_origin_frame is not None
                else playback_start)
            applied_time = mapping_module.applied_maya_time(maya_time, subframe)
            if self.pose_node and not cmds().objExists(self.pose_node):
                raise SyncRefused("POSE_NODE_MISSING", self.pose_node)
        except mapping_module.PayloadError as error:
            self.frames_rejected += 1
            detail.append("rejected: {0}: {1}".format(error.category, error.detail))
            return self._report_rejection(report, detail, frame_serial)
        except (ValueError, TypeError, KeyError, SyncRefused) as error:
            self.frames_rejected += 1
            detail.append("rejected: {0}: {1}".format(type(error).__name__, error))
            return self._report_rejection(report, detail, frame_serial)

        report["maya_frame"] = applied_time
        signature = (tuple(matrix), tuple(sorted(attributes.items())),
                     resolution, applied_time)
        heartbeat = signature == self._last_signature
        detail.append(notes["film_fit_reason"])
        if notes["far_clip_substituted"]:
            if not heartbeat:
                self.far_clip_substitutions += 1
            detail.append(
                "far clip plane is not representable in Unreal: Maya far clip set to "
                "{0:.3f} cm from the {1} fallback".format(
                    notes["far_clip_cm"], notes["far_clip_source"]))
        if subframe:
            detail.append(
                "display frame {0} maps to Maya subframe {1}".format(
                    time_payload["display_frame"], maya_time))

        transform = None
        try:
            # Validate the entire write set before adopting/creating a camera,
            # moving time or capturing anything that stop would write back.
            for plug in RESOLUTION_PLUGS:
                _require_writable(plug)
            transform, shape = self.ensure_camera()
        except Exception as error:
            self.frames_rejected += 1
            detail.append("rejected: {0}: {1}".format(type(error).__name__, error))
            return self._report_rejection(report, detail, frame_serial)
        if heartbeat:
            detail.append("heartbeat: identical state, Maya untouched")
        else:
            time_before = float(cmds().currentTime(query=True))
            try:
                cmds().undoInfo(openChunk=True)
                try:
                    cmds().currentTime(applied_time)
                    cmds().xform(transform, worldSpace=True, matrix=list(matrix))
                    for attribute, value in sorted(attributes.items()):
                        plug = "{0}.{1}".format(shape, attribute)
                        cmds().setAttr(plug, value)
                    self._apply_resolution_gate(shape, resolution)
                    self._read_frame_state(report, frame, applied_time, subframe, notes)
                    self._verify_frame_state(report["camera"], matrix, attributes,
                                             resolution, applied_time)
                except Exception:
                    cmds().undoInfo(closeChunk=True)
                    try:
                        cmds().undo()
                    except RuntimeError as undo_error:
                        detail.append("frame rollback failed: {0}".format(undo_error))
                    cmds().currentTime(time_before)
                    raise
                else:
                    cmds().undoInfo(closeChunk=True)
            except Exception as error:
                self.frames_rejected += 1
                detail.append("rejected: {0}: {1}".format(type(error).__name__, error))
                return self._report_rejection(report, detail, frame_serial)
            self._last_signature = signature

        if heartbeat:
            try:
                self._read_frame_state(report, frame, applied_time, subframe, notes)
                self._verify_frame_state(report["camera"], matrix, attributes,
                                         resolution, applied_time)
            except Exception as error:
                self.frames_rejected += 1
                self._last_signature = None  # the next heartbeat must repair the state
                detail.append("rejected: {0}: {1}".format(type(error).__name__, error))
                return self._report_rejection(report, detail, frame_serial)
        report["unreal_display_frame"] = time_payload["display_frame"]
        report["unreal_camera_path"] = camera_payload.get("path")
        report["maya_origin_frame"] = (self.maya_origin_frame if self.maya_origin_frame
                                        is not None else playback_start)
        if self.pose_node:
            report["pose"] = {
                "node": self.pose_node,
                "sampled_maya_frame": float(cmds().currentTime(query=True)),
                "translate_x": float(cmds().getAttr(self.pose_node + ".translateX")),
            }
        for marker in report["markers"]:
            if marker["delta"] is None:
                continue
            largest = max(abs(marker["delta"][0]), abs(marker["delta"][1]))
            if self.max_marker_delta is None or largest > self.max_marker_delta:
                self.max_marker_delta = largest
        report["status"] = STATUS_APPLIED
        if heartbeat:
            self.frames_unchanged += 1
        else:
            self.frames_applied += 1
            if subframe:
                self.frames_subframe += 1
        self.last_frame_serial = frame_serial
        self.last_report = report
        self.reports.append(report)
        if len(self.reports) > self.max_reports:
            del self.reports[0:len(self.reports) - self.max_reports]
        return report

    def _read_frame_state(self, report, frame, applied_time, subframe, notes):
        values = self._read_back(self._camera_transform, self._camera_shape,
                                 applied_time, subframe, notes)
        report["camera"] = values
        projection = {"focal_length_mm": values["focalLength"],
                      "aperture_inches": (values["horizontalFilmAperture"],
                                          values["verticalFilmAperture"]),
                      "film_offset_inches": (values["horizontalFilmOffset"],
                                             values["verticalFilmOffset"]),
                      "film_fit": values["filmFit"],
                      "lens_squeeze_ratio": values["lensSqueezeRatio"]}
        gate = values["defaultResolution"]
        # Maya's projection uses device aspect, including any pixel aspect.
        report["markers"] = mapping().marker_report(
            frame["markers"], values["world_matrix"], projection,
            (gate["deviceAspectRatio"], 1.0))

    def _verify_frame_state(self, values, matrix, attributes, resolution, applied_time):
        expected = dict(attributes)
        expected["maya_time"] = applied_time
        expected["displayResolution"] = True
        expected["displayGateMask"] = True
        differences = [key for key, value in expected.items()
                       if not math.isclose(float(values[key]), float(value),
                                           rel_tol=0.0 if key == "maya_time" else 1e-6,
                                           abs_tol=1e-6)]
        if any(not math.isclose(actual, wanted, rel_tol=1e-6, abs_tol=1e-4)
               for actual, wanted in zip(values["world_matrix"], matrix)):
            differences.append("world_matrix")
        gate = values["defaultResolution"]
        if (gate["width"] != int(round(resolution[0])) or
                gate["height"] != int(round(resolution[1])) or
                not math.isclose(gate["deviceAspectRatio"], resolution[0] / resolution[1],
                                 rel_tol=1e-6) or gate["pixelAspect"] != 1.0):
            differences.append("defaultResolution")
        if differences:
            raise SyncRefused(CATEGORY_HOST_STATE_MISMATCH,
                              "Maya read-back differs from the target: " + ", ".join(differences))

    def _report_rejection(self, report, detail, frame_serial):
        """Publish a rejected frame like any other report.

        A rejected frame has to reach the caller: swallowing it would let a peer
        report a clean session while every frame failed to apply.
        """
        report["detail"] = detail
        self.last_report = report
        self.last_frame_serial = frame_serial
        self.reports.append(report)
        if len(self.reports) > self.max_reports:
            del self.reports[0:len(self.reports) - self.max_reports]
        return report

    def drain_reports(self):
        """Reports produced since the last drain (peers and tests)."""
        reports, self.reports = self.reports, []
        return reports

    def _apply_resolution_gate(self, shape, resolution):
        """Set the rendered pixel extent and the device aspect Maya derives from it.

        Maya's film fit uses ``defaultResolution.deviceAspectRatio``, which does
        not follow ``width``/``height`` when they are set through the API, so
        the prototype writes it as well; otherwise a non-16:9 gate would keep
        framing with the previous aspect. The scene's own gate is captured on
        the first write and restored by ``stop()``.
        """
        self._capture_scene_resolution()
        for plug, value in (("defaultResolution.width", int(round(resolution[0]))),
                            ("defaultResolution.height", int(round(resolution[1]))),
                            ("defaultResolution.pixelAspect", 1.0),
                            ("defaultResolution.deviceAspectRatio",
                             resolution[0] / resolution[1])):
            cmds().setAttr(plug, value)
        for plug in (shape + ".displayResolution", shape + ".displayGateMask"):
            cmds().setAttr(plug, True)

    def _read_back(self, transform, shape, applied_time, subframe, notes):
        values = {}
        for attribute in MANAGED_ATTRIBUTES:
            plug = "{0}.{1}".format(shape, attribute)
            values[attribute] = cmds().getAttr(plug)
        values["maya_time"] = float(cmds().currentTime(query=True))
        values["applied_time"] = applied_time
        values["subframe"] = bool(subframe)
        values["far_clip_substituted"] = bool(notes["far_clip_substituted"])
        values["far_clip_source"] = notes["far_clip_source"]
        values["film_fit"] = values["filmFit"]
        values["lens_squeeze_ratio"] = values["lensSqueezeRatio"]
        values["world_matrix"] = [float(value) for value in
                                  cmds().xform(transform, query=True,
                                               worldSpace=True, matrix=True)]
        values["transform"] = transform
        values["shape"] = shape
        values["defaultResolution"] = {
            "width": int(cmds().getAttr("defaultResolution.width")),
            "height": int(cmds().getAttr("defaultResolution.height")),
            "pixelAspect": float(cmds().getAttr("defaultResolution.pixelAspect")),
            "deviceAspectRatio": float(
                cmds().getAttr("defaultResolution.deviceAspectRatio")),
        }
        return values

    # ------------------------------------------------------------- idle pump

    def attach_idle_pump(self, force=False):
        """Follow from Maya's idle event, when this Maya session has one.

        Batch sessions (mayapy) do not deliver idle events and return no
        scriptJob id, so the note says so and ``pump()`` is the headless path.
        """
        maya_cmds = cmds()
        if self._script_job is not None:
            return self._script_job
        if maya_cmds.about(batch=True) and not force:
            self.idle_pump_note = ("batch session: idle events are not delivered, "
                                   "call pump() instead")
            return None
        job = maya_cmds.scriptJob(idleEvent=self._idle_tick, protected=False)
        if job is None:
            self.idle_pump_note = ("this Maya session returned no scriptJob for "
                                   "idleEvent; the idle pump is unavailable, call "
                                   "pump() instead")
            return None
        self._script_job = job
        self.idle_pump_note = "idle pump attached as scriptJob {0}".format(job)
        return job

    def detach_idle_pump(self):
        if self._script_job is None:
            return
        try:
            if cmds().scriptJob(exists=self._script_job):
                cmds().scriptJob(kill=self._script_job, force=True)
        except (RuntimeError, TypeError):
            pass
        self._script_job = None

    def _idle_tick(self):
        try:
            self.pump()
        except Exception as error:  # never break Maya's idle loop
            detail = "{0}: {1}".format(type(error).__name__, error)
            self._idle_errors.append(detail)
            self._fail(detail)
            self.stop()

    # --------------------------------------------------------------- reports

    def summary(self):
        return {
            "state": self.state,
            "detail": self.detail,
            "error": self.error,
            "host": self.host,
            "port": self.port,
            "camera": self._camera_transform,
            "scene_fps": self._scene_fps,
            "time_unit": self._time_unit,
            "frames_applied": self.frames_applied,
            "frames_rejected": self.frames_rejected,
            "frames_subframe": self.frames_subframe,
            "frames_unchanged": self.frames_unchanged,
            "far_clip_substitutions": self.far_clip_substitutions,
            "last_frame_serial": self.last_frame_serial,
            "max_marker_delta": self.max_marker_delta,
            "idle_pump": self.idle_pump_note,
            "server_errors": list(self.server_errors),
            "idle_errors": list(self._idle_errors),
        }


def run(host=DEFAULT_HOST, port=DEFAULT_PORT, duration=None, camera_name=DEFAULT_CAMERA_NAME,
        idle_pump=False, poll=0.005, on_frame=None, maya_origin_frame=None,
        pose_node=None):
    """Connect, follow until the publisher stops or ``duration`` elapses, stop.

    With ``idle_pump`` the follower is driven by Maya's idle event; otherwise
    this loop pumps explicitly, which is what headless mayapy needs.
    """
    follower = CameraSyncFollower(host=host, port=port, camera_name=camera_name,
                                  maya_origin_frame=maya_origin_frame,
                                  pose_node=pose_node)
    try:
        follower.connect()
        if idle_pump:
            follower.attach_idle_pump()
        deadline = None if duration is None else time.time() + float(duration)
        while follower.state == STATE_FOLLOWING:
            follower.pump()
            if on_frame is not None and follower.last_report is not None:
                on_frame(follower.last_report)
            if deadline is not None and time.time() >= deadline:
                follower.stop("duration elapsed")
                break
            if not idle_pump:
                time.sleep(poll)
    except KeyboardInterrupt:
        follower.stop("interrupted")
    finally:
        follower.stop()
    return follower


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--duration", type=float, default=None,
                        help="seconds to follow; default is until the publisher ends")
    parser.add_argument("--camera-name", default=DEFAULT_CAMERA_NAME)
    parser.add_argument("--maya-origin-frame", type=float, default=None,
                        help="Maya frame mapped to the Unreal playback start; defaults to that start")
    parser.add_argument("--pose-node", default=None,
                        help="optional keyed Maya joint/transform whose translateX is returned as a witness")
    parser.add_argument("--idle-pump", action="store_true",
                        help="drive the session from Maya's idle event")
    parser.add_argument("--result", default=None,
                        help="optional path for the session summary JSON")
    args = parser.parse_args(argv)
    if _cmds_module is None:
        try:
            import maya.cmds  # noqa: F401
        except ImportError:
            import maya.standalone
            maya.standalone.initialize(name="python")
    follower = run(args.host, args.port, args.duration, args.camera_name,
                   args.idle_pump, maya_origin_frame=args.maya_origin_frame,
                   pose_node=args.pose_node)
    summary = follower.summary()
    print(json.dumps(summary, indent=2, ensure_ascii=True))
    if args.result:
        Path(args.result).write_text(
            json.dumps(summary, indent=2, ensure_ascii=True), encoding="utf-8")
    return 0 if summary["state"] in (STATE_STOPPED,) and not summary["error"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
