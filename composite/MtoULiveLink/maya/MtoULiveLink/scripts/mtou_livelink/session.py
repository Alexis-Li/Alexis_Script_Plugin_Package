"""Real-time streaming session, sender worker and negotiation events."""

import errno
import json
import queue
import select
import socket
import threading
import time

from . import config, mayaapi
from .config import (
    CHARACTER_OBJECT_ID,
    DEFAULT_PLAYBACK_CAP,
    SECONDARY_OBJECT_ID,
    WORKFLOW_ANIMATION,
    WORKFLOWS,
)
from .diagnostics import (
    DIAGNOSTICS,
    blendshape_warning_from_reply,
    filter_bone_driven_differences,
    make_diagnostic,
)
from .mayaapi import cmds, om
from .protocol import (
    encode_message,
    exact_int,
    make_frame_message,
    make_hello_message,
    make_init_message,
    make_remove_object_message,
    recv_message,
    validate_reply,
)
from .scene import CharacterSceneError
from .space import validate_frame_rate
from .timeline import maya_is_playing, normalize_playback_cap, playback_cap_fps


class LatestFrame(object):
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


# Control statements both hosts may send outside a mode the Maya panel is
# currently driving; they are recorded instead of ending the connection.
CONTROL_REPLY_TYPES = (
    "mode_state",
    "reference_offer",
    "reference_result",
    "reference_removed",
    "camera_time",
    "applied",
    "object_removed",
)

# A second object's negotiation differences are listed with this prefix, so
# the warning dialog tells them apart from the primary character's.
SECONDARY_WARNING_PREFIX = "副对象 · "


def merge_object_warnings(warnings):
    """One warning for the whole set; the second object's entries are prefixed."""
    merged = {"missing_in_unreal": [], "missing_in_maya": [],
              "bone_name_remaps": [], "has_warning": False}
    for index, warning in enumerate(warnings):
        prefix = "" if index == 0 else SECONDARY_WARNING_PREFIX
        for key in ("missing_in_unreal", "missing_in_maya", "bone_name_remaps"):
            merged[key].extend(prefix + str(value) for value in warning.get(key) or ())
        merged["has_warning"] = merged["has_warning"] or bool(warning.get("has_warning"))
    return merged


class SenderWorker(threading.Thread):
    def __init__(self, init_message, hello_message=None):
        threading.Thread.__init__(self, name="MtoULiveLinkSender")
        self.daemon = True
        # One init per object of the set, primary first; a single dict is the
        # one-character set.
        if isinstance(init_message, (list, tuple)):
            self._init_messages = [dict(message) for message in init_message]
        else:
            self._init_messages = [init_message]
        self._init_message = self._init_messages[0]
        self._hello_message = hello_message
        # The connection greeting precedes the object descriptions so a peer
        # with a different protocol version refuses before it reads an object.
        initial = b""
        if hello_message is not None:
            initial += encode_message(hello_message)
        for message in self._init_messages:
            initial += encode_message(message)
        self._initial_packet = initial
        self._hello_reply = None
        self._last_control_reply = None
        self._latest = LatestFrame()
        # Unbounded by design: the cached upload streams hundreds of frames
        # through the same ordered path. Linkage controls keep their own bound
        # in the linkage session instead of constraining this queue.
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

    def submit_control(self, message):
        """Queue one control message in order, whichever delivery mode is on.

        Latest mode never drains the ordered queue, so a control statement
        made while live frames stream (结束预览 after 返回实时, a refused
        entry) travels through the carryover queue that precedes them.
        """
        with self._send_lock:
            with self._lock:
                ordered_mode = self._ordered_mode
            packet = encode_message(message)
            if ordered_mode:
                self._ordered.put(packet)
            else:
                self._carryover.put(packet)

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
        # Read the endpoint at call time so a peer or a test can point the
        # worker at another port without reloading the module.
        result = sock.connect_ex((config.HOST, config.PORT))
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
        packet = memoryview(self._initial_packet)
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

    def _read_handshake_reply(self, sock):
        """Read until ready/error, recording the hello acknowledgement."""
        deadline = time.time() + 5.0
        while not self._stop_event.is_set():
            timeout = deadline - time.time()
            if timeout <= 0.0:
                raise socket.timeout("timed out waiting for the Unreal reply")
            sock.settimeout(timeout)
            reply = validate_reply(recv_message(sock))
            reply_type = reply.get("type")
            if reply_type == "hello_ack":
                with self._lock:
                    self._hello_reply = dict(reply)
                continue
            if reply_type == "mode_state":
                continue
            return reply
        return None

    def _identity_mismatch(self, reply, init_message=None):
        """Describe the first identity field the ready reply fails to echo."""
        init_message = init_message or self._init_message
        expected_object = init_message.get("object_id")
        if expected_object is not None and reply.get("object_id") != expected_object:
            return "object_id: expected {0!r}, ready reported {1!r}".format(
                expected_object, reply.get("object_id"))
        checks = (
            ("session_id", self._hello_reply or {}),
            ("set_revision", init_message),
            ("space_revision", init_message),
        )
        for field, source in checks:
            expected = exact_int(source.get(field))
            if expected is None:
                continue
            actual = exact_int(reply.get(field))
            if actual != expected:
                return "{0}: expected {1!r}, ready reported {2!r}".format(
                    field, expected, reply.get(field))
        return None

    def _negotiate_set(self, sock):
        """Read one ready per object of the set; returns the merged warning.

        Any object Unreal refuses ends the whole set: the error reply becomes
        the session's terminal status and nothing streams.
        """
        warnings = []
        for init_message in self._init_messages:
            reply = self._read_handshake_reply(sock)
            if reply is None:
                return None
            if reply.get("type") == "error":
                diagnostic = make_diagnostic(
                    reply.get("code") or "INTERNAL_ERROR",
                    reply.get("message") or "Unreal rejected the connection",
                    details=reply.get("details") or reply.get("message") or "")
                self._set_status("error", diagnostic["summary"], diagnostic=diagnostic)
                return None
            if reply.get("type") != "ready":
                diagnostic = make_diagnostic(
                    "INVALID_MESSAGE",
                    "expected ready, received {0}".format(reply.get("type")))
                self._set_status("error", diagnostic["summary"], diagnostic=diagnostic)
                return None
            identity_error = self._identity_mismatch(reply, init_message)
            if identity_error is not None:
                diagnostic = make_diagnostic(
                    "INVALID_MESSAGE",
                    "The ready reply does not echo the negotiated identity.",
                    details=identity_error)
                self._set_status("error", diagnostic["summary"], diagnostic=diagnostic)
                return None
            expected_revision = exact_int(init_message.get("revision"))
            if (expected_revision is None
                    or exact_int(reply.get("revision")) != expected_revision):
                diagnostic = make_diagnostic(
                    "INVALID_MESSAGE",
                    "The ready reply does not echo the negotiated revision.",
                    details="init revision {0!r}, ready revision {1!r}".format(
                        init_message.get("revision"),
                        reply.get("revision")))
                self._set_status("error", diagnostic["summary"], diagnostic=diagnostic)
                return None
            warnings.append(blendshape_warning_from_reply(
                filter_bone_driven_differences(init_message, reply)))
        return merge_object_warnings(warnings)

    def hello_reply(self):
        with self._lock:
            return dict(self._hello_reply) if self._hello_reply else None

    def last_control_reply(self):
        """The last control statement received without a registered listener."""
        with self._lock:
            return dict(self._last_control_reply) if self._last_control_reply else None

    def run(self):
        connected = False
        try:
            sock = self._connect()
            if sock is None or self._stop_event.is_set():
                return
            if not self._send_initial(sock):
                return
            sock.settimeout(5.0)
            warning = self._negotiate_set(sock)
            if warning is None:
                return
            connected = True
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
                    if listener is not None:
                        try:
                            # A listener returns False for a reply it leaves
                            # to the session's own handling below.
                            handled = listener(reply) is not False
                        except Exception as exc:
                            handled = True
                            if cmds is not None:
                                try:
                                    cmds.warning(
                                        "MtoU_LiveLink cached reply listener failed: {0}".format(
                                            exc))
                                except Exception:
                                    pass
                        if handled:
                            continue
                    if reply.get("type") == "error":
                        diagnostic = make_diagnostic(
                            reply.get("code") or "STREAM_INTERRUPTED",
                            reply.get("message") or "Unreal closed the connection",
                            details=reply.get("details")
                            or reply.get("message") or "")
                        self._set_status(
                            "error", diagnostic["summary"], diagnostic=diagnostic)
                        return
                    if reply.get("type") in CONTROL_REPLY_TYPES:
                        # A control statement can cross a mode boundary: the
                        # leave answer, an unsolicited reference offer or a
                        # late camera/time target must never end the session
                        # that the user is still using.
                        with self._lock:
                            self._last_control_reply = dict(reply)
                        continue
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


class StreamingSessionError(RuntimeError):
    def __init__(self, code, message="", details="", recapture_scene=False):
        self._code = code
        self._message = message or code
        self._details = details or self._message
        self._recapture_scene = bool(recapture_scene)
        super(StreamingSessionError, self).__init__(self._message)

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


def copy_session_payload(value):
    if isinstance(value, dict):
        return {key: copy_session_payload(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return tuple(copy_session_payload(item) for item in value)
    return value


class StreamingSessionEvent(object):
    def __init__(self, kind, warning=None, diagnostic=None, recapture_scene=False):
        self._kind = kind
        self._warning = copy_session_payload(warning) if warning else None
        self._diagnostic = copy_session_payload(diagnostic) if diagnostic else None
        self._recapture_scene = bool(recapture_scene)

    @property
    def kind(self):
        return self._kind

    @property
    def warning(self):
        return copy_session_payload(self._warning) if self._warning else None

    @property
    def diagnostic(self):
        return copy_session_payload(self._diagnostic) if self._diagnostic else None

    @property
    def recapture_scene(self):
        return self._recapture_scene


class StreamingSession(object):
    """Owns one Maya-to-Unreal streaming connection lifecycle."""

    def __init__(self, scene, fps, on_event, playback_cap=DEFAULT_PLAYBACK_CAP,
                 playback_state=None, workflow=WORKFLOW_ANIMATION,
                 blendshapes_enabled=True, object_id=CHARACTER_OBJECT_ID,
                 set_revision=1, space_revision=1, scene_units=None,
                 maya_version=None, secondary_scene=None):
        if workflow not in WORKFLOWS:
            raise StreamingSessionError(
                "INVALID_MESSAGE",
                "workflow must be 'animation' or 'model'.",
                "requested workflow: {0}".format(workflow))
        if secondary_scene is not None and workflow != WORKFLOW_ANIMATION:
            raise StreamingSessionError(
                "OBJECT_SET_UNSUPPORTED",
                "模型工作流只支持单个角色。",
                "a second object streams only in the Animation workflow")
        self._scene = scene
        # The independent object beside the primary character; None for the
        # one-character set. Removal inside the connection clears it.
        self._secondary_scene = secondary_scene
        self._secondary_revision = None
        # The new set revision a requested removal waits for; frames pause
        # until Unreal confirms it.
        self._removal_pending = None
        self._scene_fps = fps
        self._playback_cap = normalize_playback_cap(playback_cap)
        self._playback_state = playback_state or maya_is_playing
        self._is_playing = self._read_playback_state()
        self._on_event = on_event
        self._workflow = workflow
        self._blendshapes_enabled = bool(blendshapes_enabled)
        self._object_id = str(object_id)
        self._set_revision = int(set_revision)
        self._space_revision = int(space_revision)
        self._scene_units = tuple(scene_units or ("cm", 1.0))
        self._maya_version = maya_version
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
        self._sampling_paused = False

    @property
    def object_id(self):
        return self._object_id

    @property
    def set_objects(self):
        """The ordered object ids of the current set, primary first."""
        if self._secondary_scene is not None:
            return (self._object_id, SECONDARY_OBJECT_ID)
        return (self._object_id,)

    @property
    def secondary_scene(self):
        return self._secondary_scene

    @property
    def removal_pending(self):
        return self._removal_pending is not None

    def object_scenes(self):
        """Map of object id to its scene for the current set."""
        scenes = {self._object_id: self._scene}
        if self._secondary_scene is not None:
            scenes[SECONDARY_OBJECT_ID] = self._secondary_scene
        return scenes

    @property
    def set_revision(self):
        return self._set_revision

    @property
    def space_revision(self):
        return self._space_revision

    @property
    def hello_reply(self):
        if self._worker is None:
            return None
        return self._worker.hello_reply()

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
              blendshapes_enabled=True, object_id=CHARACTER_OBJECT_ID,
              set_revision=1, space_revision=1, scene_units=None,
              maya_version=None, secondary_scene=None):
        try:
            fps = validate_frame_rate(fps)
            snapshot = scene.snapshot()
        except CharacterSceneError as error:
            raise StreamingSessionError(
                error.code, error.message, error.details, recapture_scene=True)
        except (RuntimeError, ValueError) as exc:
            raise StreamingSessionError(
                "INVALID_FRAME_RATE", str(exc), details=str(exc))
        secondary_snapshot = None
        if secondary_scene is not None:
            try:
                secondary_snapshot = secondary_scene.snapshot()
            except CharacterSceneError as error:
                raise StreamingSessionError(
                    error.code, "副对象：" + error.message, error.details)
        session = cls(scene, fps, on_event, playback_cap, playback_state,
                      workflow, blendshapes_enabled, object_id, set_revision,
                      space_revision, scene_units, maya_version, secondary_scene)
        try:
            session._start(snapshot, secondary_snapshot)
        except Exception as exc:
            session._cleanup()
            if isinstance(exc, StreamingSessionError):
                raise
            raise StreamingSessionError(
                "INTERNAL_ERROR", str(exc), details=str(exc))
        return session

    def _object_init(self, snapshot, object_id):
        if len(snapshot.bind_local_transforms) != len(snapshot.bones):
            raise StreamingSessionError(
                "BIND_POSE_INVALID",
                "The captured bind pose does not match the captured skeleton.")
        bones = [
            [name, parent, list(snapshot.bind_local_transforms[index])]
            for index, (name, parent) in enumerate(snapshot.bones)
        ]
        return make_init_message(
            bones, list(snapshot.curve_names), snapshot.revision,
            workflow=self._workflow,
            blendshapes_enabled=self._blendshapes_enabled,
            object_id=object_id,
            set_revision=self._set_revision,
            space_revision=self._space_revision,
            set_objects=list(self.set_objects))

    def _start(self, snapshot, secondary_snapshot=None):
        init_messages = [self._object_init(snapshot, self._object_id)]
        if secondary_snapshot is not None:
            init_messages.append(
                self._object_init(secondary_snapshot, SECONDARY_OBJECT_ID))
            self._secondary_revision = secondary_snapshot.revision
        hello_message = make_hello_message(
            self._maya_version or "unknown",
            self._scene_units[0], self._scene_units[1], self._scene_fps)
        self._worker = SenderWorker(init_messages, hello_message)
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
        """Suspend live sampling for Cached Playback."""
        self._pause_sampling(
            reply_listener, "CACHED_PLAYBACK_NOT_READY",
            "Cached Playback requires a ready streaming session.")

    def begin_linkage(self, reply_listener=None):
        """Suspend live sampling so the linkage pump owns the pose instead."""
        self._pause_sampling(
            reply_listener, "STREAM_INTERRUPTED",
            "镜头联动需要已连接的 Unreal 会话。")

    def _pause_sampling(self, reply_listener=None, code="CACHED_PLAYBACK_NOT_READY",
                        message="Cached Playback requires a ready streaming session."):
        """Suspend live sampling, switch to ordered delivery, and route
        Unreal's post-ready replies to ``reply_listener`` atomically, so no
        submission can precede listener installation."""
        if self._outcome is not None or self._worker is None or not self.is_ready:
            raise StreamingSessionError(code, message)
        if reply_listener is not None:
            self._worker.set_reply_listener(reply_listener)
        if self._sampling_paused:
            return
        self._sampling_paused = True
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
            raise StreamingSessionError(
                "CACHED_PLAYBACK_NOT_READY",
                "The streaming session is not ready for cached messages.")
        self._worker.submit_ordered(frame_message)

    def submit_linkage(self, message):
        """Queue one ordered linkage message; False when the worker is gone."""
        if not self.is_ready or self._worker is None:
            raise StreamingSessionError(
                "STREAM_INTERRUPTED",
                "镜头联动需要已连接的 Unreal 会话。")
        self._worker.submit_control(message)
        return True

    def ordered_pending(self):
        if self._worker is None:
            return 0
        return self._worker.ordered_pending()

    def request_secondary_removal(self, session_id, detail=""):
        """Ask Unreal to remove the second object; returns the new set revision.

        Frames of the old set stop here: realtime sampling pauses until Unreal
        confirms the new set with ``object_removed``.
        """
        if not self.is_ready or self._worker is None:
            raise StreamingSessionError(
                "STREAM_INTERRUPTED", "移除副对象需要已连接的 Unreal 会话。")
        if self._secondary_scene is None:
            raise StreamingSessionError(
                "INVALID_MESSAGE", "当前对象集合中没有副对象。")
        if self._removal_pending is not None:
            return self._removal_pending
        revision = self._set_revision + 1
        self._worker.submit_control(make_remove_object_message(
            session_id, SECONDARY_OBJECT_ID, revision, detail))
        self._removal_pending = revision
        return revision

    def adopt_removal(self, message):
        """Adopt Unreal's ``object_removed``: the set holds the primary only.

        Accepts Maya's own removal and one the Unreal user started. Returns the
        removed scene, which the caller owns, or None for a confirmation of a
        set this session already has.
        """
        revision = exact_int(message.get("set_revision"))
        if message.get("object_id") != SECONDARY_OBJECT_ID or revision is None:
            raise StreamingSessionError(
                "INVALID_MESSAGE", "object_removed names no removable object.",
                "object_removed {0!r} at set revision {1!r}".format(
                    message.get("object_id"), message.get("set_revision")))
        if revision <= self._set_revision:
            return None
        removed, self._secondary_scene = self._secondary_scene, None
        self._secondary_revision = None
        self._set_revision = revision
        self._removal_pending = None
        if not self._sampling_paused:
            self._sample_and_submit(force=True)
        return removed

    def end_cached_replay(self, discard_pending=False):
        if self._worker is None:
            return
        self._worker.end_ordered(discard_pending=discard_pending)

    def resume_from_cached(self):
        self._resume_sampling()

    def resume_after_linkage(self):
        """Return the live sampling path after linkage leaves the mode."""
        self._resume_sampling()

    def _resume_sampling(self):
        if not self._sampling_paused:
            return
        self.end_cached_replay()
        self._sampling_paused = False
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
        if not self._sampling_paused:
            self._replace_timer(self._effective_rate())

    def change_cap(self, playback_cap):
        if self._outcome is not None:
            return
        self._playback_cap = normalize_playback_cap(playback_cap)
        if self._sampling_paused:
            return
        if not self._replace_timer(self._effective_rate()):
            return
        self._sample_and_submit(force=True)

    def stop(self):
        if self._outcome is None:
            self._outcome = "stopped"
            self._terminal_event = StreamingSessionEvent("stopped")
            self._phase = "terminal"
        elif self._cleaned:
            return
        self._finish_terminal()

    def _on_scene_change(self, *unused):
        self.stop()

    def _on_timer(self, generation, *unused):
        if generation != self._timer_generation or self._sampling_paused:
            return
        self._update_playback_state()
        self._sample_and_submit()

    def _on_time_changed(self, *unused):
        del unused
        if self._sampling_paused:
            return
        self._update_playback_state()
        self._sample_and_submit()

    def _on_playback_condition_changed(self, *args):
        if self._sampling_paused:
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

    def poll_connection(self):
        """Notice a worker that failed or disconnected while sampling is paused.

        Live sampling notices a dead connection on its own timer, but linkage
        pauses that timer; its pump calls this instead, so a connection Unreal
        ended still reaches the terminal event that restores Maya. Returns True
        while the connection is usable.
        """
        if self._outcome is not None or self._worker is None:
            return False
        state, detail, unused_warning, diagnostic = self._worker.status()
        return not self._fail_on_worker_state(state, detail, diagnostic)

    def _fail_on_worker_state(self, state, detail, diagnostic):
        """Request the terminal failure for a failed worker; True when it did."""
        if state == "error":
            self._request_failure(
                diagnostic or make_diagnostic("INTERNAL_ERROR", "Streaming failed."),
                recapture_scene=False)
            return True
        if state == "disconnected":
            self._request_failure(
                make_diagnostic(
                    "STREAM_INTERRUPTED", detail or "Live Link connection ended.",
                    details=detail or "The streaming worker disconnected unexpectedly."),
                recapture_scene=False)
            return True
        return False

    def _sample_and_submit(self, force=False):
        if self._outcome is not None or self._worker is None:
            return
        state, detail, warning, diagnostic = self._worker.status()
        if self._fail_on_worker_state(state, detail, diagnostic):
            return
        if state != "ready":
            return
        if self._phase == "connecting":
            self._phase = "ready"
            self._emit(StreamingSessionEvent("ready", warning=warning))
        if self._removal_pending is not None:
            # The old set is closed; the new one starts with Unreal's answer.
            return
        now = time.time()
        sample_interval = 1.0 / self._effective_rate()
        if (not force and self._last_sample_time is not None
                and now >= self._last_sample_time
                and now - self._last_sample_time < sample_interval):
            return
        self._last_sample_time = now
        try:
            frame = self._scene.sample()
        except CharacterSceneError as error:
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
        rows = None
        if self._secondary_scene is not None:
            # Both objects come from the same evaluation: the set is one
            # message, so Unreal never shows them at two Maya times.
            try:
                secondary = self._secondary_scene.sample()
            except CharacterSceneError as error:
                self._request_failure(
                    make_diagnostic(
                        error.code if error.code in DIAGNOSTICS else "SAMPLING_FAILED",
                        "副对象：" + error.message, details=error.details),
                    recapture_scene=False)
                return
            if secondary.revision != self._secondary_revision:
                self._request_failure(
                    make_diagnostic(
                        "SAMPLING_FAILED",
                        "副对象的描述在本次连接中发生了变化。",
                        details="secondary scene revision changed from {0} to {1}".format(
                            self._secondary_revision, secondary.revision)),
                    recapture_scene=False)
                return
            rows = [{"object_id": SECONDARY_OBJECT_ID,
                     "transforms": list(secondary.transforms),
                     "curves": list(secondary.curves)}]
        self._worker.submit(make_frame_message(
            list(frame.transforms), list(frame.curves), rows))

    def _request_failure(self, diagnostic, recapture_scene):
        if self._outcome is not None:
            return
        self._outcome = "failed"
        self._phase = "terminal"
        self._terminal_event = StreamingSessionEvent(
            "failed", diagnostic=diagnostic, recapture_scene=recapture_scene)
        if not self._cleanup_pending:
            self._cleanup_pending = True
            mayaapi.run_deferred(self._finish_terminal, cmds.evalDeferred)

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
        self._sampling_paused = False
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


def bind_conflict_status(snapshot):
    if not snapshot.bind_conflict_count:
        return ""
    return "（已按绑定姿势解析 {0} 处蒙皮绑定矩阵冲突）".format(
        snapshot.bind_conflict_count)
