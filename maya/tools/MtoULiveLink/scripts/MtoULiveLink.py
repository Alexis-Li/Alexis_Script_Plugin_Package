"""MtoULiveLink Maya entry point."""

import json
import math
import socket
import struct


__version__ = "0.1.0"
PROTOCOL_VERSION = 1
HOST = "127.0.0.1"
PORT = 54321
SUBJECT_NAME = "MtoU_Character"

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


def main():
    """Launch the tool."""
    raise NotImplementedError("Implement MtoULiveLink.main()")


if __name__ == "__main__":
    main()
