"""Pure wire contract for the bounded two-subject MtoU prototype.

This module is the single source of truth for the prototype's newline JSON
protocol (newline-delimited compact JSON over TCP, 1 MiB per line). It has no
Maya imports and no socket, so both hosts and every test can share it:

* message builders for ``init``, ``frame`` and ``remove``;
* strict validators for outgoing messages and for the receiver's
  ``ready``/``applied``/``error`` replies, with stable error codes;
* evidence helpers used by the mayapy peer and the host checks.

Contract summary (version 1):

* exactly two subjects, distinct named ids from ``ROLE_IDS``;
* within a subject, bone names and Morph curve names are unique; bone parents
  index earlier bones only (topological order);
* one Maya evaluated time and one strictly increasing serial per atomic pair,
  starting at 1 (the receiver refuses serial 0);
* transforms and bind rows are UE-space 10-number local poses in centimetres
  (``[tx, ty, tz, qx, qy, qz, qw, sx, sy, sz]``), the root row being the
  inclusive world pose of the Maya root joint;
* the receiver answers ``ready`` with a positive session id, then one
  ``applied`` echo per frame; ``remove`` answers one ``applied`` event that
  disables exactly the removed subject.
"""

import json
import math
import re

PROTOCOL_NAME = "MtoUMultiSubject"
PROTOCOL_VERSION = 1
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 54340

# Frozen per-line ceiling: a peer that sends more is refused, so a wedged or
# hostile stream cannot grow without bound.
MAX_LINE_BYTES = 1024 * 1024

#: Named subject ids the bounded prototype drives. The pair is any two of
#: them; a new pair is a new negotiation and needs a new connection.
ROLE_IDS = ("character", "prop", "arms")
MAX_SUBJECTS = 2

STATUS_APPLIED = "applied"
STATUS_DISABLED = "disabled"

TRANSFORM_WIDTH = 10
BIND_WIDTH = TRANSFORM_WIDTH
QUATERNION_TOLERANCE = 1.0e-4
TIME_TOLERANCE = 1.0e-6
QUATERNION_NORM_MIN = 1.0e-6

INIT = "init"
FRAME = "frame"
REMOVE = "remove"
READY = "ready"
APPLIED = "applied"
ERROR = "error"
MESSAGE_TYPES = (INIT, FRAME, REMOVE, READY, APPLIED, ERROR)
REPLY_TYPES = (READY, APPLIED, ERROR)

_ID_PATTERN = re.compile(r"^[A-Za-z][A-Za-z0-9_]{0,31}$")

# Stable rejection codes. Maya-side refusals carry the same codes as the
# receiver's error replies so a failure is comparable across hosts.
CODE_EMPTY_LINE = "EMPTY_LINE"
CODE_LINE_TOO_LONG = "LINE_TOO_LONG"
CODE_INVALID_JSON = "INVALID_JSON"
CODE_NOT_AN_OBJECT = "NOT_AN_OBJECT"
CODE_UNKNOWN_TYPE = "UNKNOWN_TYPE"
CODE_VERSION_UNSUPPORTED = "VERSION_UNSUPPORTED"
CODE_SUBJECT_COUNT = "SUBJECT_COUNT"
CODE_SUBJECT_ID_INVALID = "SUBJECT_ID_INVALID"
CODE_SUBJECT_ID_DUPLICATE = "SUBJECT_ID_DUPLICATE"
CODE_FPS_INVALID = "FPS_INVALID"
CODE_ROOT_INVALID = "ROOT_INVALID"
CODE_BONES_EMPTY = "BONES_EMPTY"
CODE_BONE_NAME_INVALID = "BONE_NAME_INVALID"
CODE_BONE_NAME_DUPLICATE = "BONE_NAME_DUPLICATE"
CODE_BONE_PARENT_INVALID = "BONE_PARENT_INVALID"
CODE_CURVE_NAME_INVALID = "CURVE_NAME_INVALID"
CODE_CURVE_NAME_DUPLICATE = "CURVE_NAME_DUPLICATE"
CODE_BIND_COUNT = "BIND_COUNT"
CODE_BIND_ROW_INVALID = "BIND_ROW_INVALID"
CODE_BIND_QUATERNION_INVALID = "BIND_QUATERNION_INVALID"
CODE_TRANSFORM_COUNT = "TRANSFORM_COUNT"
CODE_TRANSFORM_ROW_INVALID = "TRANSFORM_ROW_INVALID"
CODE_TRANSFORM_QUATERNION_INVALID = "TRANSFORM_QUATERNION_INVALID"
CODE_CURVE_COUNT = "CURVE_COUNT"
CODE_CURVE_VALUE_INVALID = "CURVE_VALUE_INVALID"
CODE_SERIAL_INVALID = "SERIAL_INVALID"
CODE_SERIAL_NOT_INCREASING = "SERIAL_NOT_INCREASING"
CODE_TIME_INVALID = "TIME_INVALID"
CODE_TIME_NOT_INCREASING = "TIME_NOT_INCREASING"
CODE_SESSION_INVALID = "SESSION_INVALID"
CODE_SESSION_MISMATCH = "SESSION_MISMATCH"
CODE_SERIAL_MISMATCH = "SERIAL_MISMATCH"
CODE_TIME_MISMATCH = "TIME_MISMATCH"
CODE_SUBJECT_MISMATCH = "SUBJECT_MISMATCH"
CODE_STATUS_INVALID = "STATUS_INVALID"
CODE_REMOVE_ID_INVALID = "REMOVE_ID_INVALID"
CODE_REMOVE_ID_INACTIVE = "REMOVE_ID_INACTIVE"


class ProtocolError(ValueError):
    """A message this contract refuses. ``code`` is stable and machine usable."""

    def __init__(self, code, message="", details=""):
        self.code = code
        self.message = message or code
        self.details = details or self.message
        super(ProtocolError, self).__init__(
            "{0}: {1}".format(self.code, self.details))


class RemoteError(RuntimeError):
    """The receiver answered ``{"type": "error"}`` instead of a reply."""

    def __init__(self, code, details="", reply=None):
        self.code = code or "REMOTE_ERROR"
        self.details = details
        self.reply = reply
        super(RemoteError, self).__init__(
            "{0}: {1}".format(self.code, self.details))


# --------------------------------------------------------------- primitives

def _is_number(value):
    return not isinstance(value, bool) and isinstance(value, (int, float))


def _number(value, code, field, index=None):
    if not _is_number(value):
        raise ProtocolError(
            code, "expected a JSON number",
            "{0}{1} is {2!r}".format(field, "" if index is None else "[{0}]".format(index),
                                     value))
    number = float(value)
    if not math.isfinite(number):
        raise ProtocolError(
            code, "expected a finite number",
            "{0} is {1!r}".format(field, value))
    return number


def _positive_int(value, code, field):
    if isinstance(value, bool) or not isinstance(value, int) or value < 1:
        raise ProtocolError(
            code, "expected a positive JSON integer",
            "{0} is {1!r}".format(field, value))
    return int(value)


def valid_subject_id(value):
    return isinstance(value, str) and _ID_PATTERN.match(value) is not None


def encode_line(message):
    """One compact UTF-8 JSON line, newline terminated."""
    try:
        payload = json.dumps(message, ensure_ascii=False, allow_nan=False,
                             separators=(",", ":"))
    except (TypeError, ValueError) as error:
        raise ProtocolError(CODE_NOT_AN_OBJECT, "message is not JSON encodable",
                            str(error))
    data = payload.encode("utf-8") + b"\n"
    if len(data) > MAX_LINE_BYTES:
        raise ProtocolError(CODE_LINE_TOO_LONG, "encoded line exceeds the 1 MiB bound",
                            "{0} bytes".format(len(data)))
    return data


def parse_message(line):
    """Parse one line into a JSON object with a known ``type``."""
    if isinstance(line, bytes):
        try:
            text = line.decode("utf-8")
        except UnicodeDecodeError as error:
            raise ProtocolError(CODE_INVALID_JSON, "line is not UTF-8", str(error))
    else:
        text = line
    if not isinstance(text, str) or not text.strip():
        raise ProtocolError(CODE_EMPTY_LINE, "line is empty")
    try:
        message = json.loads(text)
    except ValueError as error:
        raise ProtocolError(CODE_INVALID_JSON, "line is not JSON", str(error))
    if not isinstance(message, dict):
        raise ProtocolError(CODE_NOT_AN_OBJECT, "message must be a JSON object",
                            "got {0}".format(type(message).__name__))
    message_type = message.get("type")
    if message_type not in MESSAGE_TYPES:
        raise ProtocolError(CODE_UNKNOWN_TYPE, "unknown message type",
                            "type is {0!r}".format(message_type))
    return message


class LineFramer(object):
    """Bounded newline framing for one connection.

    ``feed`` buffers bytes and ``take_lines`` returns every complete line. The
    buffer never grows past ``max_line_bytes``: a line that does is refused
    immediately instead of being accumulated.
    """

    def __init__(self, max_line_bytes=MAX_LINE_BYTES):
        self.max_line_bytes = int(max_line_bytes)
        self._buffer = b""
        self._refused = None

    @property
    def pending_bytes(self):
        return len(self._buffer)

    def feed(self, data):
        if self._refused is not None:
            raise self._refused
        if not data:
            return
        self._buffer += bytes(data)
        newline = self._buffer.find(b"\n")
        if newline < 0 and len(self._buffer) > self.max_line_bytes:
            self._refused = ProtocolError(
                CODE_LINE_TOO_LONG, "line exceeds the {0} byte bound".format(
                    self.max_line_bytes), "no newline within {0} bytes".format(
                    len(self._buffer)))
            self._buffer = b""
            raise self._refused

    def take_lines(self):
        lines = []
        while True:
            newline = self._buffer.find(b"\n")
            if newline < 0:
                break
            raw = self._buffer[:newline]
            self._buffer = self._buffer[newline + 1:]
            if len(raw) > self.max_line_bytes:
                raise ProtocolError(
                    CODE_LINE_TOO_LONG, "line exceeds the {0} byte bound".format(
                        self.max_line_bytes), "{0} bytes".format(len(raw)))
            if raw.strip():
                lines.append(raw.decode("utf-8"))
        if len(self._buffer) > self.max_line_bytes:
            raise ProtocolError(
                CODE_LINE_TOO_LONG, "line exceeds the {0} byte bound".format(
                    self.max_line_bytes), "no newline within {0} bytes".format(
                    len(self._buffer)))
        return lines


# ------------------------------------------------------------ message builders

def make_init(subjects, fps, version=PROTOCOL_VERSION):
    """Build and validate one ``init`` message.

    ``subjects`` rows are ``{"id","root","bones","curves","bind"}`` where
    ``bones`` is ``[{"name","parent"}]``, ``curves`` is a list of names and
    ``bind`` is one 10-number UE-space row per bone.
    """
    message = {
        "type": INIT,
        "version": int(version),
        "fps": _number(fps, CODE_FPS_INVALID, "fps"),
        "subjects": [
            {
                "id": subject.get("id"),
                "root": subject.get("root"),
                "bones": list(subject.get("bones") or []),
                "curves": list(subject.get("curves") or []),
                "bind": [list(row) for row in (subject.get("bind") or [])],
            }
            for subject in subjects
        ],
    }
    validate_init(message)
    return message


def make_frame(serial, time, subjects):
    """Build and validate one ``frame`` message for the atomic subject pair."""
    message = {
        "type": FRAME,
        "serial": serial,
        "time": time,
        "subjects": [
            {
                "id": subject.get("id"),
                "transforms": [list(row) for row in (subject.get("transforms") or [])],
                "curves": [_number(value, CODE_CURVE_VALUE_INVALID, "curves")
                           for value in (subject.get("curves") or [])],
            }
            for subject in subjects
        ],
    }
    if isinstance(message["serial"], bool) or not isinstance(message["serial"], int) \
            or message["serial"] < 1:
        raise ProtocolError(CODE_SERIAL_INVALID, "serial must be a positive integer",
                            "serial is {0!r}".format(message["serial"]))
    message["time"] = _number(message["time"], CODE_TIME_INVALID, "time")
    return message


def make_remove(subject_id):
    if not valid_subject_id(subject_id):
        raise ProtocolError(CODE_REMOVE_ID_INVALID, "remove id is not a subject id",
                            "id is {0!r}".format(subject_id))
    return {"type": REMOVE, "id": subject_id}


# ------------------------------------------------------------------ validation

def _validated_subject_ids(subjects, expected_count=MAX_SUBJECTS):
    if not isinstance(subjects, list) or len(subjects) != expected_count:
        raise ProtocolError(
            CODE_SUBJECT_COUNT,
            "exactly {0} subjects are required".format(expected_count),
            "got {0}".format(len(subjects) if isinstance(subjects, list) else subjects))
    ids = []
    for subject in subjects:
        if not isinstance(subject, dict):
            raise ProtocolError(CODE_SUBJECT_ID_INVALID, "subject must be an object",
                                repr(subject))
        subject_id = subject.get("id")
        if not valid_subject_id(subject_id):
            raise ProtocolError(CODE_SUBJECT_ID_INVALID, "subject id is invalid",
                                "id is {0!r}".format(subject_id))
        if subject_id in ids:
            raise ProtocolError(CODE_SUBJECT_ID_DUPLICATE, "subject ids must be unique",
                                "duplicate id {0!r}".format(subject_id))
        ids.append(subject_id)
    return ids


def _validate_bones(subject_id, bones):
    if not isinstance(bones, list) or not bones:
        raise ProtocolError(CODE_BONES_EMPTY, "a subject needs at least one bone",
                            "subject {0} declared {1!r}".format(subject_id, bones))
    names = []
    parents = []
    for index, bone in enumerate(bones):
        if not isinstance(bone, dict):
            raise ProtocolError(CODE_BONE_NAME_INVALID, "bone must be an object",
                                "{0}[{1}] is {2!r}".format(subject_id, index, bone))
        name = bone.get("name")
        if not isinstance(name, str) or not name:
            raise ProtocolError(CODE_BONE_NAME_INVALID, "bone name must be a string",
                                "{0}[{1}] name is {2!r}".format(subject_id, index, name))
        if name in names:
            raise ProtocolError(
                CODE_BONE_NAME_DUPLICATE,
                "bone names must be unique inside a subject",
                "subject {0} repeats {1!r}".format(subject_id, name))
        parent = bone.get("parent")
        if isinstance(parent, bool) or not isinstance(parent, int) \
                or parent < -1 or parent >= index:
            raise ProtocolError(
                CODE_BONE_PARENT_INVALID,
                "bone parent must index an earlier bone (-1 for the root)",
                "{0}[{1}].parent is {2!r}".format(subject_id, index, parent))
        names.append(name)
        parents.append(int(parent))
    return names, parents


def _validate_names(subject_id, field, values, invalid_code, duplicate_code):
    if not isinstance(values, list):
        raise ProtocolError(invalid_code, "{0} must be a list".format(field),
                            "{0} is {1!r}".format(subject_id, values))
    seen = []
    for value in values:
        if not isinstance(value, str) or not value:
            raise ProtocolError(invalid_code,
                                "{0} entries must be non-empty strings".format(field),
                                "{0} has {1!r}".format(subject_id, value))
        if value in seen:
            raise ProtocolError(duplicate_code,
                                "{0} must be unique inside a subject".format(field),
                                "{0} repeats {1!r}".format(subject_id, value))
        seen.append(value)
    return seen


def _validate_rows(subject_id, field, rows, width, expected_count, count_code,
                   row_code, quaternion_code=None):
    if not isinstance(rows, list) or len(rows) != expected_count:
        raise ProtocolError(
            count_code,
            "{0} needs one {1} row per declared bone".format(subject_id, field),
            "{0} rows={1} bones={2}".format(subject_id, len(rows), expected_count))
    validated = []
    for index, row in enumerate(rows):
        if not isinstance(row, (list, tuple)) or len(row) != width:
            raise ProtocolError(
                row_code, "{0} rows must have {1} numbers".format(field, width),
                "{0}[{1}] is {2!r}".format(subject_id, index, row))
        values = [_number(value, row_code, "{0}[{1}]".format(subject_id, index), i)
                  for i, value in enumerate(row)]
        if quaternion_code is not None:
            qx, qy, qz, qw = values[3:7]
            norm = math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw)
            if not norm or abs(norm - 1.0) > QUATERNION_TOLERANCE:
                raise ProtocolError(
                    quaternion_code, "quaternion must be unit length",
                    "{0}[{1}] norm is {2}".format(subject_id, index, norm))
        validated.append(values)
    return validated


def validate_init(message, protocol_version=PROTOCOL_VERSION):
    """Validate one ``init`` and return its summary."""
    if not isinstance(message, dict) or message.get("type") != INIT:
        raise ProtocolError(CODE_UNKNOWN_TYPE, "message is not an init",
                            "type is {0!r}".format(
                                message.get("type") if isinstance(message, dict) else message))
    version = message.get("version")
    if version != protocol_version:
        raise ProtocolError(
            CODE_VERSION_UNSUPPORTED, "unsupported protocol version",
            "declared {0!r}, expected {1!r}".format(version, protocol_version))
    fps = _number(message.get("fps"), CODE_FPS_INVALID, "fps")
    if fps <= 0.0:
        raise ProtocolError(CODE_FPS_INVALID, "fps must be positive",
                            "fps is {0}".format(fps))
    subjects = message.get("subjects")
    ids = _validated_subject_ids(subjects)
    summary = {"ids": ids, "fps": fps, "bones": {}, "parents": {}, "curves": {},
               "rows": {}, "roots": {}, "active": list(ids)}
    for subject in subjects:
        subject_id = subject["id"]
        root = subject.get("root")
        if not isinstance(root, str) or not root:
            raise ProtocolError(CODE_ROOT_INVALID, "subject root must be a node path",
                                "{0} root is {1!r}".format(subject_id, root))
        names, parents = _validate_bones(subject_id, subject.get("bones"))
        curves = _validate_names(subject_id, "curve names", subject.get("curves"),
                                 CODE_CURVE_NAME_INVALID, CODE_CURVE_NAME_DUPLICATE)
        rows = _validate_rows(subject_id, "bind", subject.get("bind"), BIND_WIDTH,
                              len(names), CODE_BIND_COUNT, CODE_BIND_ROW_INVALID,
                              CODE_BIND_QUATERNION_INVALID)
        summary["bones"][subject_id] = names
        summary["parents"][subject_id] = parents
        summary["curves"][subject_id] = curves
        summary["rows"][subject_id] = len(rows)
        summary["roots"][subject_id] = root
    return summary


def active_subject_ids(summary):
    """Ids a frame must still carry, in negotiated order."""
    return [subject_id for subject_id in summary["ids"]
            if subject_id in summary["active"]]


def disable_subject(summary, subject_id):
    """Drop one negotiated subject from the active set (after ``remove``)."""
    if subject_id not in summary["active"]:
        raise ProtocolError(
            CODE_REMOVE_ID_INACTIVE, "subject is not active",
            "{0} active are {1}".format(subject_id, summary["active"]))
    if len(summary["active"]) <= 1:
        raise ProtocolError(
            CODE_REMOVE_ID_INACTIVE, "the last active subject cannot be removed",
            "{0} active are {1}".format(subject_id, summary["active"]))
    summary["active"] = [value for value in summary["active"]
                         if value != subject_id]
    return list(summary["active"])


def validate_frame(message, summary, previous_serial=None, previous_time=None):
    """Validate one ``frame`` against the negotiated init summary."""
    if not isinstance(message, dict) or message.get("type") != FRAME:
        raise ProtocolError(CODE_UNKNOWN_TYPE, "message is not a frame",
                            "type is {0!r}".format(
                                message.get("type") if isinstance(message, dict) else message))
    serial = message.get("serial")
    if isinstance(serial, bool) or not isinstance(serial, int) or serial < 1:
        raise ProtocolError(CODE_SERIAL_INVALID, "serial must be a positive integer",
                            "serial is {0!r}".format(serial))
    if previous_serial is not None and serial <= int(previous_serial):
        raise ProtocolError(
            CODE_SERIAL_NOT_INCREASING, "serial must increase per connection",
            "{0} after {1}".format(serial, previous_serial))
    time = _number(message.get("time"), CODE_TIME_INVALID, "time")
    if previous_time is not None and time <= float(previous_time):
        raise ProtocolError(
            CODE_TIME_NOT_INCREASING, "evaluated time must increase per connection",
            "{0} after {1}".format(time, previous_time))
    subjects = message.get("subjects")
    if not isinstance(subjects, list):
        raise ProtocolError(CODE_SUBJECT_MISMATCH, "subjects must be a list",
                            repr(subjects))
    ids = [subject.get("id") if isinstance(subject, dict) else None
           for subject in subjects]
    expected = active_subject_ids(summary)
    if ids != expected:
        raise ProtocolError(
            CODE_SUBJECT_MISMATCH,
            "frame subjects must repeat the negotiated ids in order",
            "expected {0}, got {1}".format(expected, ids))
    for subject in subjects:
        subject_id = subject["id"]
        bone_count = len(summary["bones"][subject_id])
        _validate_rows(subject_id, "transforms", subject.get("transforms"),
                       TRANSFORM_WIDTH, bone_count, CODE_TRANSFORM_COUNT,
                       CODE_TRANSFORM_ROW_INVALID, CODE_TRANSFORM_QUATERNION_INVALID)
        curves = subject.get("curves")
        if not isinstance(curves, list):
            raise ProtocolError(CODE_CURVE_COUNT, "curves must be a list",
                                "{0} curves is {1!r}".format(subject_id, curves))
        if len(curves) != len(summary["curves"][subject_id]):
            raise ProtocolError(
                CODE_CURVE_COUNT, "curve count must match the negotiated init",
                "{0}: expected {1}, got {2}".format(
                    subject_id, len(summary["curves"][subject_id]), len(curves)))
        for index, value in enumerate(curves):
            _number(value, CODE_CURVE_VALUE_INVALID,
                    "{0}.curves[{1}]".format(subject_id, index))
    return {"serial": int(serial), "time": time, "ids": ids}


def validate_reply_type(reply, expected):
    if not isinstance(reply, dict) or reply.get("type") not in REPLY_TYPES:
        raise ProtocolError(CODE_UNKNOWN_TYPE, "not a receiver reply",
                            repr(reply))
    if reply["type"] == ERROR:
        raise RemoteError(reply.get("code"), str(reply.get("details") or ""), reply)
    if reply["type"] != expected:
        raise ProtocolError(
            CODE_UNKNOWN_TYPE, "unexpected reply type",
            "expected {0}, got {1}".format(expected, reply["type"]))
    return reply


def validate_ready(reply, previous_session=None):
    """Validate one ``ready`` reply and return the negotiated session id.

    A connection may negotiate more than once: after a semantic error reply, or
    after a previous session, a new ``init`` is a new negotiation. The session
    id therefore has to increase, so a stale or replayed ``ready`` is refused.
    """
    validate_reply_type(reply, READY)
    session = _positive_int(reply.get("session"), CODE_SESSION_INVALID, "session")
    if previous_session is not None and session <= int(previous_session):
        raise ProtocolError(
            CODE_SESSION_INVALID, "a new negotiation needs a new session id",
            "{0} after {1}".format(session, previous_session))
    return session


def _statuses(reply, session, ids, allowed_status):
    if reply.get("session") != session:
        raise ProtocolError(
            CODE_SESSION_MISMATCH, "reply session does not match the negotiation",
            "expected {0}, got {1!r}".format(session, reply.get("session")))
    subjects = reply.get("subjects")
    if not isinstance(subjects, list) or len(subjects) != len(ids):
        raise ProtocolError(CODE_SUBJECT_MISMATCH, "reply subjects do not match",
                            "expected {0}, got {1!r}".format(ids, subjects))
    statuses = {}
    for subject, expected_id in zip(subjects, ids):
        if not isinstance(subject, dict) or subject.get("id") != expected_id:
            raise ProtocolError(CODE_SUBJECT_MISMATCH, "reply subject id mismatch",
                                "expected {0}, got {1!r}".format(expected_id, subject))
        status = subject.get("status")
        if status not in allowed_status:
            raise ProtocolError(
                CODE_STATUS_INVALID, "unexpected subject status",
                "{0} status is {1!r}, allowed {2}".format(
                    expected_id, status, sorted(allowed_status)))
        statuses[expected_id] = status
    return statuses


def validate_applied(reply, session, serial, time, active_ids):
    """Validate one frame ``applied`` echo and return per-subject statuses."""
    validate_reply_type(reply, APPLIED)
    echo_serial = reply.get("serial")
    if isinstance(echo_serial, bool) or not isinstance(echo_serial, int) \
            or echo_serial != int(serial):
        raise ProtocolError(
            CODE_SERIAL_MISMATCH, "applied echo must repeat the frame serial",
            "sent {0}, got {1!r}".format(serial, echo_serial))
    echo_time = _number(reply.get("time"), CODE_TIME_MISMATCH, "time")
    if abs(echo_time - float(time)) > TIME_TOLERANCE:
        raise ProtocolError(
            CODE_TIME_MISMATCH, "applied echo must repeat the evaluated time",
            "sent {0}, got {1}".format(time, echo_time))
    return _statuses(reply, session, list(active_ids), (STATUS_APPLIED,))


def validate_remove_reply(reply, session, removed_id, remaining_ids, last_serial=None,
                          last_time=None):
    """Validate the ``applied`` state event answering ``remove``.

    The event reports the whole subject set of the connection: the removed
    subject as ``disabled`` and every remaining subject as ``applied`` (order is
    free, the set is not). Optional ``serial``/``time`` echo the last applied
    frame (``0``/``0.0`` when none was applied) and must not rewind the
    connection; they are a state echo, not a frame acknowledgement.
    """
    validate_reply_type(reply, APPLIED)
    if reply.get("serial") is not None:
        serial = reply.get("serial")
        if isinstance(serial, bool) or not isinstance(serial, int) or serial < 0:
            raise ProtocolError(CODE_SERIAL_INVALID, "remove echo serial is invalid",
                                repr(serial))
        if last_serial is not None and serial < int(last_serial):
            raise ProtocolError(
                CODE_SERIAL_NOT_INCREASING, "remove echo serial went backwards",
                "{0} after {1}".format(serial, last_serial))
    if reply.get("time") is not None:
        time = _number(reply.get("time"), CODE_TIME_INVALID, "time")
        if last_time is not None and time < float(last_time) - TIME_TOLERANCE:
            raise ProtocolError(
                CODE_TIME_NOT_INCREASING, "remove echo time went backwards",
                "{0} after {1}".format(time, last_time))
    if reply.get("session") != session:
        raise ProtocolError(
            CODE_SESSION_MISMATCH, "reply session does not match the negotiation",
            "expected {0}, got {1!r}".format(session, reply.get("session")))
    subjects = reply.get("subjects")
    expected = {subject_id: STATUS_APPLIED for subject_id in remaining_ids
                if subject_id != removed_id}
    expected[removed_id] = STATUS_DISABLED
    if not isinstance(subjects, list) or len(subjects) != len(expected):
        raise ProtocolError(
            CODE_SUBJECT_MISMATCH, "remove echo must report the whole subject set",
            "expected {0}, got {1!r}".format(sorted(expected), subjects))
    statuses = {}
    for subject in subjects:
        subject_id = subject.get("id") if isinstance(subject, dict) else None
        if subject_id not in expected or subject_id in statuses:
            raise ProtocolError(
                CODE_SUBJECT_MISMATCH, "remove echo reported an unexpected subject",
                repr(subject))
        status = subject.get("status")
        if status != expected[subject_id]:
            raise ProtocolError(
                CODE_STATUS_INVALID, "unexpected subject status in remove echo",
                "{0} status is {1!r}, expected {2!r}".format(
                    subject_id, status, expected[subject_id]))
        statuses[subject_id] = status
    return statuses


# ------------------------------------------------------------------- evidence

def new_evidence(tool, host=None, port=None):
    return {
        "ok": False,
        "tool": tool,
        "protocol": {"name": PROTOCOL_NAME, "version": PROTOCOL_VERSION},
        "host": host,
        "port": port,
        "checks": [],
        "errors": [],
        "sessions": [],
    }


def add_check(evidence, label, ok, detail=""):
    entry = {"check": label, "ok": bool(ok), "detail": detail}
    evidence.setdefault("checks", []).append(entry)
    return entry


def add_error(evidence, code, detail=""):
    entry = {"code": code, "detail": detail}
    evidence.setdefault("errors", []).append(entry)
    return entry


def finalise_evidence(evidence):
    evidence["ok"] = (not evidence.get("errors")
                      and all(entry.get("ok") for entry in evidence.get("checks") or []))
    return evidence


def write_json(path, value):
    """Write evidence JSON; the caller owns the path."""
    with open(path, "w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, ensure_ascii=True, sort_keys=False,
                  allow_nan=False)
        stream.write("\n")
    return path
