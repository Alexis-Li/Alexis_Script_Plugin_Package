"""Protocol framing, message builders and reply validation."""

import json
import math
import struct

from .config import MAX_CACHE_FRAME_COUNT, MAX_CACHE_PAYLOAD_BYTES, MAX_MESSAGE_BYTES, PROTOCOL_VERSION, WORKFLOWS, WORKFLOW_ANIMATION


def finite(values):
    return all(math.isfinite(float(value)) for value in values)


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
    if not finite(flat):
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
    }
    # ``source_frame`` is present only once the attempt has applied a pose; a
    # pause or resume before the first frame omits it instead of inventing a
    # frame the character never showed.
    optional = {
        "cache_paused": ("source_frame",),
        "cache_resumed": ("source_frame",),
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
    for name in optional.get(reply_type, ()):
        if name in reply and not isinstance(reply[name], (int, float)):
            raise ValueError("protocol reply field '{0}' has the wrong JSON type".format(name))
    for name in ("revision", "target_morph_count", "accepted_morph_count",
                 "upload_id", "play_id", "clear_id", "applied",
                 "applied_frame_count", "applied_frames", "round",
                 "start_frame", "end_frame"):
        if name not in reply:
            continue
        value = reply[name]
        if isinstance(value, bool) or not float(value).is_integer():
            raise ValueError(
                "protocol reply field '{0}' must be an integer count".format(name))
    if "scope" in reply and reply["scope"] not in ("cache", "segment"):
        raise ValueError(
            "protocol reply field 'scope' must be 'cache' or 'segment'")
    if "clear_id" in reply and not 1 <= reply["clear_id"] <= 2147483647:
        raise ValueError("protocol reply field 'clear_id' must be a positive int32")
    if "elapsed_seconds" in reply and finite_float(reply["elapsed_seconds"]) is None:
        raise ValueError(
            "protocol reply field 'elapsed_seconds' must be a finite number")
    return reply
