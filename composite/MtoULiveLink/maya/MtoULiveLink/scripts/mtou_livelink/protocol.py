"""Protocol framing, message builders and reply validation."""

import json
import math
import struct

from .config import (
    CHARACTER_OBJECT_ID,
    CONTROL_MAYA,
    CONTROL_UNREAL,
    LINKAGE_MODE,
    MAX_CACHE_FRAME_COUNT,
    MAX_CACHE_PAYLOAD_BYTES,
    MAX_MESSAGE_BYTES,
    MAX_SET_OBJECTS,
    MODES,
    PROTOCOL_VERSION,
    SECONDARY_OBJECT_ID,
    WORKFLOW_ANIMATION,
    WORKFLOWS,
)


def finite(values):
    return all(math.isfinite(float(value)) for value in values)


def validate_set_objects(set_objects):
    """The ordered object ids of one set: the primary first, no repeats."""
    objects = [str(value) for value in (set_objects or ())]
    if not objects or objects[0] != CHARACTER_OBJECT_ID:
        raise ValueError("an object set starts with the '{0}' object".format(
            CHARACTER_OBJECT_ID))
    if len(set(objects)) != len(objects):
        raise ValueError("an object set must not repeat a duplicate object id")
    if len(objects) > MAX_SET_OBJECTS:
        raise ValueError(
            "an object set holds at most {0} objects (max_set_objects)".format(
                MAX_SET_OBJECTS))
    if any(value not in (CHARACTER_OBJECT_ID, SECONDARY_OBJECT_ID) for value in objects):
        raise ValueError("unknown object id in the object set")
    return objects


def make_init_message(bones, curves, revision, workflow=WORKFLOW_ANIMATION,
                      blendshapes_enabled=True, object_id=CHARACTER_OBJECT_ID,
                      set_revision=1, space_revision=1, set_objects=None):
    if workflow not in WORKFLOWS:
        raise ValueError("workflow must be 'animation' or 'model'")
    if not object_id:
        raise ValueError("object id must not be empty")
    objects = validate_set_objects(set_objects or [CHARACTER_OBJECT_ID])
    if str(object_id) not in objects:
        raise ValueError("object id {0!r} is not part of set_objects".format(object_id))
    if workflow != WORKFLOW_ANIMATION and len(objects) > 1:
        raise ValueError("the Model workflow keeps the single-character object set")
    return {
        "type": "init",
        "version": PROTOCOL_VERSION,
        "revision": int(revision),
        "object_id": str(object_id),
        "set_revision": _positive_int(set_revision, "set revision"),
        "space_revision": _positive_int(space_revision, "space revision"),
        "workflow": workflow,
        "blendshapes_enabled": bool(blendshapes_enabled),
        "bones": bones,
        "curves": curves,
        "set_objects": objects,
    }


def make_frame_message(transforms, curves, objects=None):
    """One realtime sample of the whole set at one Maya time.

    The primary keeps the v9 top-level fields; ``objects`` carries every other
    object of a two-object set, so both reach Unreal in one message.
    """
    flat = [value for transform in transforms for value in transform] + list(curves)
    if not finite(flat):
        raise ValueError("frame values must be finite")
    message = {"type": "frame", "transforms": transforms, "curves": curves}
    if objects:
        message["objects"] = _object_rows(objects, "frame", primary_allowed=False)
    return message


def _object_rows(objects, label, primary_allowed=True):
    rows = []
    seen = set()
    for entry in objects:
        if not isinstance(entry, dict):
            raise ValueError("{0} object must be an object".format(label))
        for field in ("object_id", "transforms", "curves"):
            if field not in entry:
                raise ValueError(
                    "{0} object requires field '{1}'".format(label, field))
        object_id = str(entry["object_id"])
        if object_id in seen:
            raise ValueError(
                "{0} names a duplicate object id {1!r}".format(label, object_id))
        if not primary_allowed and object_id == CHARACTER_OBJECT_ID:
            raise ValueError(
                "{0} object rows carry only the objects beside the primary".format(label))
        seen.add(object_id)
        flat = [value for transform in entry["transforms"] for value in transform]
        flat.extend(entry["curves"])
        if not finite(flat):
            raise ValueError("{0} values must be finite".format(label))
        rows.append({
            "object_id": object_id,
            "transforms": entry["transforms"],
            "curves": entry["curves"],
        })
    return rows


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


DEFAULT_CAPABILITIES = {
    "linkage": True,
    "reference": True,
    "cache": True,
    "model": True,
}


def make_hello_message(maya_version, linear_unit, unit_scale, fps,
                       capabilities=None):
    """First message of a connection: version, scene units and capabilities."""
    scale = finite_float(unit_scale)
    if scale is None or scale <= 0.0:
        raise ValueError("scene unit scale must be a positive finite number")
    rate = finite_float(fps)
    if rate is None or rate <= 0.0:
        raise ValueError("scene frame rate must be a positive finite number")
    declared = dict(DEFAULT_CAPABILITIES)
    if capabilities:
        for name, value in capabilities.items():
            declared[str(name)] = bool(value)
    return {
        "type": "hello",
        "version": PROTOCOL_VERSION,
        "host": "maya",
        "maya_version": str(maya_version),
        "scene": {
            "linear_unit": str(linear_unit),
            "unit_scale_cm": scale,
            "fps": rate,
        },
        "capabilities": declared,
    }


def make_mode_message(session_id, mode_revision, mode, control, transition,
                      detail=""):
    """Maya's authority statement for one time-control state."""
    if mode not in MODES:
        raise ValueError("mode must be one of {0}".format(", ".join(MODES)))
    if control not in (CONTROL_MAYA, CONTROL_UNREAL):
        raise ValueError("control must be 'maya' or 'unreal'")
    if transition not in ("ready", "failed"):
        raise ValueError("mode transition must be 'ready' or 'failed'")
    return {
        "type": "mode",
        "session_id": _positive_int(session_id, "session id"),
        "mode_revision": _positive_int(mode_revision, "mode revision"),
        "mode": mode,
        "control": control,
        "transition": transition,
        "detail": str(detail),
    }


def make_leave_linkage_message(session_id, mode_revision, target, detail=""):
    if target not in ("realtime", "end"):
        raise ValueError("linkage exit target must be 'realtime' or 'end'")
    return {
        "type": "leave_linkage",
        "session_id": _positive_int(session_id, "session id"),
        "mode_revision": _positive_int(mode_revision, "mode revision"),
        "target": target,
        "detail": str(detail),
    }


def make_group_message(session_id, set_revision, mode_revision, space_revision,
                       eval_id, transport_serial, sample_serial, objects,
                       camera, time):
    """One complete pose sample answering one linkage target."""
    if not isinstance(objects, list) or not objects:
        raise ValueError("linkage group must carry at least one object")
    rows = _object_rows(objects, "linkage group")
    return {
        "type": "group",
        "session_id": _positive_int(session_id, "session id"),
        "set_revision": _positive_int(set_revision, "set revision"),
        "mode_revision": _positive_int(mode_revision, "mode revision"),
        "space_revision": _positive_int(space_revision, "space revision"),
        "eval_id": str(eval_id),
        "transport_serial": _positive_int(transport_serial, "transport serial"),
        "sample_serial": _positive_int(sample_serial, "sample serial"),
        "objects": rows,
        "camera": camera,
        "time": time,
    }


def group_wire_size(message):
    """Encoded UTF-8 byte length of one group message.

    The linkage budget meters the complete group, not one object at a time, so
    the caller compares this against the frozen per-message ceiling.
    """
    payload = json.dumps(message, ensure_ascii=False, allow_nan=False,
                         separators=(",", ":")).encode("utf-8")
    return len(payload)


def make_remove_object_message(session_id, object_id, set_revision, detail=""):
    """Maya removes the second object; the revision names the new set."""
    if str(object_id) != SECONDARY_OBJECT_ID:
        raise ValueError(
            "only the '{0}' object can be removed inside a connection; removing "
            "the 'character' object ends the connection".format(SECONDARY_OBJECT_ID))
    return {
        "type": "remove_object",
        "session_id": _positive_int(session_id, "session id"),
        "object_id": str(object_id),
        "set_revision": _positive_int(set_revision, "set revision"),
        "detail": str(detail),
    }


def make_reference_result_message(session_id, transfer_id, accepted, detail,
                                  counts=None, container=None, code=None):
    counts = counts or {}
    message = {
        "type": "reference_result",
        "session_id": _positive_int(session_id, "session id"),
        "transfer_id": str(transfer_id),
        "accepted": bool(accepted),
        "detail": str(detail),
        "counts": {
            "objects": _positive_int(counts.get("objects", 0), "object count", allow_zero=True),
            "triangles": _positive_int(
                counts.get("triangles", 0), "triangle count", allow_zero=True),
            "bytes": _positive_int(counts.get("bytes", 0), "byte count", allow_zero=True),
        },
    }
    if container:
        message["container"] = str(container)
    if code:
        # The stable refusal code, so the other host reports the actual reason
        # instead of guessing one.
        message["code"] = str(code)
    return message


def make_reference_removed_message(session_id, detail=""):
    return {
        "type": "reference_removed",
        "session_id": _positive_int(session_id, "session id"),
        "detail": str(detail),
    }


def make_reference_query_message(session_id, transfer_id, scope):
    if not isinstance(scope, dict) or "level" not in scope:
        raise ValueError("reference scope requires a level package name")
    sublevels = scope.get("sublevels", [])
    if not isinstance(sublevels, list) or any(
            not isinstance(value, str) for value in sublevels):
        raise ValueError("reference scope sublevels must be strings")
    return {
        "type": "reference_query",
        "session_id": _positive_int(session_id, "session id"),
        "transfer_id": str(transfer_id),
        "scope": {
            "level": str(scope["level"]),
            "sublevels": [str(value) for value in sublevels],
        },
    }


def _positive_int(value, label, allow_zero=False):
    number = exact_int(value)
    minimum = 0 if allow_zero else 1
    if number is None or number < minimum or number > 2147483647:
        raise ValueError("{0} must be a positive int32".format(label))
    return number


def make_cache_frame_message(index, transforms, curves):
    index = int(index)
    if index < 0:
        raise ValueError("cached frame index must not be negative")
    flat = [value for transform in transforms for value in transform] + list(curves)
    if not finite(flat):
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


def make_cache_clear_message(clear_id):
    if (exact_int(clear_id) is None or clear_id < 1 or clear_id > 2147483647):
        raise ValueError("cache clear id must be a positive int32")
    return {"type": "cache_clear", "clear_id": clear_id}


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


def recv_exact(sock, size):
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
    size = struct.unpack(">Q", recv_exact(sock, 8))[0]
    if size > MAX_MESSAGE_BYTES:
        raise ValueError("protocol message length exceeds the supported message size")
    return json.loads(recv_exact(sock, size).decode("utf-8"))


def exact_int(value):
    """Return value when it is a non-bool integer JSON number, else None."""
    if isinstance(value, bool) or not isinstance(value, int):
        return None
    return value


def exact_bool(value):
    """Return value when it is a JSON boolean, else None.

    ``bool`` is the only accepted type: JSON ``1``/``0`` are numbers, and
    ``isinstance(1, int)`` style leniency must never turn a count into a
    selection.
    """
    return value if isinstance(value, bool) else None


def finite_float(value):
    """Return value as float when it is a non-bool finite JSON number."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    number = float(value)
    return number if math.isfinite(number) else None


def validate_anchor(anchor):
    """A linkage ``mode_state`` names the target's placement.

    The shape is the handoff manifest's ``world.anchor``: ``location_cm[3]``
    and ``rotation_deg{roll, pitch, yaw}``, all finite.
    """
    if not isinstance(anchor, dict):
        raise ValueError("a linkage mode_state must name its anchor")
    location = anchor.get("location_cm")
    rotation = anchor.get("rotation_deg")
    if not isinstance(location, list) or len(location) != 3             or any(finite_float(value) is None for value in location):
        raise ValueError("anchor field 'location_cm' must be three finite numbers")
    if not isinstance(rotation, dict) or any(
            finite_float(rotation.get(name)) is None
            for name in ("roll", "pitch", "yaw")):
        raise ValueError(
            "anchor field 'rotation_deg' must name finite roll, pitch and yaw")


# List fields whose entries are objects rather than names.
STRING_LIST_EXCEPTIONS = ("components", "targets")


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
        "cache_playing": {
            "upload_id": (int, float), "play_id": (int, float), "loop": bool},
        "cache_progress": {"play_id": (int, float), "applied": (int, float)},
        "cache_complete": {
            "play_id": (int, float),
            "applied_frame_count": (int, float),
            "elapsed_seconds": (int, float),
            "scope": str,
            "start_frame": (int, float),
            "end_frame": (int, float),
        },
        "cache_paused": {"play_id": (int, float), "applied_frames": (int, float)},
        "cache_resumed": {"play_id": (int, float), "applied_frames": (int, float)},
        "cache_seeked": {
            "play_id": (int, float),
            "source_frame": (int, float),
            "applied_frames": (int, float),
        },
        "cache_looped": {
            "play_id": (int, float),
            "round": (int, float),
            "source_frame": (int, float),
        },
        "cache_loop_changed": {"play_id": (int, float), "loop": bool},
        "cache_stopped": {"play_id": (int, float)},
        "cache_cleared": {
            "upload_id": (int, float), "play_id": (int, float), "clear_id": int},
        "hello_ack": {
            "version": (int, float),
            "session_id": (int, float),
            "capabilities": dict,
            "targets": list,
            "space": dict,
        },
        "object_removed": {
            "session_id": (int, float),
            "object_id": str,
            "set_revision": (int, float),
            "restored": bool,
            "detail": str,
        },
        "mode_state": {
            "session_id": (int, float),
            "mode_revision": (int, float),
            "mode": str,
            "control": str,
            "detail": str,
        },
        "camera_time": {
            "session_id": (int, float),
            "set_revision": (int, float),
            "mode_revision": (int, float),
            "space_revision": (int, float),
            "eval_id": str,
            "eval_serial": (int, float),
            "transport_serial": (int, float),
            "frame_serial": (int, float),
            "time": dict,
            "camera": dict,
            "gate": dict,
        },
        "applied": {
            "session_id": (int, float),
            "mode_revision": (int, float),
            "eval_id": str,
            "sample_serial": (int, float),
            "applied": bool,
            "applied_display_frame": (int, float),
            "lag_seconds": (int, float),
            "components": list,
            "reason": str,
        },
        "reference_offer": {
            "session_id": (int, float),
            "transfer_id": str,
            "directory": str,
            "manifest_digest": str,
            "scope": dict,
            "space_revision": (int, float),
            "counts": dict,
        },
        "reference_query": {
            "session_id": (int, float),
            "transfer_id": str,
            "scope": dict,
        },
    }
    # ``source_frame`` is present only once the attempt has applied a pose; a
    # pause or resume before the first frame omits it instead of inventing a
    # frame the character never showed.
    optional = {
        "cache_paused": {"source_frame": (int, float)},
        "cache_resumed": {"source_frame": (int, float)},
        "camera_time": {"cut": dict},
        "applied": {"display_frame": (int, float)},
        "hello_ack": {"detail": str},
    }
    fields = required.get(reply_type)
    if reply_type == "hello_ack" and exact_int(reply.get("version")) != PROTOCOL_VERSION:
        # A peer of another version answers with another shape; the version is
        # the reason, whatever else differs.
        raise ValueError(
            "protocol reply field 'version' must equal the current protocol version")
    if fields is None:
        raise ValueError(
            "protocol reply type must be 'ready', 'error', a cache outcome"
            " or a linkage message")
    for name, expected_type in fields.items():
        if name not in reply:
            raise ValueError("protocol reply requires field '{0}'".format(name))
        if not isinstance(reply[name], expected_type):
            raise ValueError("protocol reply field '{0}' has the wrong JSON type".format(name))
        if expected_type is list and name not in STRING_LIST_EXCEPTIONS \
                and any(not isinstance(value, str) for value in reply[name]):
            raise ValueError("protocol reply field '{0}' must contain strings".format(name))
    for name, expected_type in optional.get(reply_type, {}).items():
        if name in reply and not isinstance(reply[name], expected_type):
            raise ValueError("protocol reply field '{0}' has the wrong JSON type".format(name))
    for name in ("revision", "target_morph_count", "accepted_morph_count",
                 "upload_id", "play_id", "clear_id",
                 "applied_frame_count", "applied_frames", "round",
                 "start_frame", "end_frame", "session_id", "set_revision",
                 "mode_revision", "space_revision", "eval_serial",
                 "transport_serial", "frame_serial", "sample_serial",
                 "version"):
        if name not in reply:
            continue
        value = reply[name]
        if isinstance(value, bool) or not float(value).is_integer():
            raise ValueError(
                "protocol reply field '{0}' must be an integer count".format(name))
    if reply_type == "cache_progress":
        applied = reply.get("applied")
        if isinstance(applied, bool) or not float(applied).is_integer():
            raise ValueError(
                "protocol reply field 'applied' must be an integer count")
    if reply_type == "cache_complete" and reply["scope"] not in ("cache", "segment"):
        raise ValueError(
            "protocol reply field 'scope' must be 'cache' or 'segment'")
    if reply_type == "hello_ack":
        for target in reply["targets"]:
            if not isinstance(target, dict):
                raise ValueError("hello_ack targets must be objects")
            for name in ("object_id", "kind", "binding_path", "generation"):
                if not isinstance(target.get(name), str):
                    raise ValueError(
                        "hello_ack target field '{0}' must be a string".format(name))
        space = reply["space"]
        if not isinstance(space.get("anchor"), dict):
            raise ValueError("hello_ack space field 'anchor' must be an object")
    if reply_type in ("mode_state",) and reply["mode"] not in MODES:
        raise ValueError(
            "protocol reply field 'mode' must be one of {0}".format(", ".join(MODES)))
    if reply_type in ("mode_state",) and reply["control"] not in (CONTROL_MAYA, CONTROL_UNREAL):
        raise ValueError("protocol reply field 'control' must be 'maya' or 'unreal'")
    if reply_type == "mode_state" and reply["mode"] == LINKAGE_MODE:
        validate_anchor(reply.get("anchor"))
    if reply_type == "applied" and not reply["applied"]:
        if not reply["reason"]:
            raise ValueError(
                "a failed linkage application must name its reason")
    if reply_type in ("camera_time", "applied") and not reply["eval_id"]:
        raise ValueError("protocol reply field 'eval_id' must not be empty")
    if "clear_id" in reply and not 1 <= reply["clear_id"] <= 2147483647:
        raise ValueError("protocol reply field 'clear_id' must be a positive int32")
    if "elapsed_seconds" in reply and finite_float(reply["elapsed_seconds"]) is None:
        raise ValueError(
            "protocol reply field 'elapsed_seconds' must be a finite number")
    if "lag_seconds" in reply and finite_float(reply["lag_seconds"]) is None:
        raise ValueError("protocol reply field 'lag_seconds' must be a finite number")
    return reply
