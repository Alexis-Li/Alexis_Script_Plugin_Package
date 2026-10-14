"""Centimetre space, frame-rate and unit conversion helpers."""

import math
import re

from .config import MAX_FRAME_RATE, MIN_FRAME_RATE, TIME_UNIT_FPS
from .protocol import finite


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


def convert_transform(translation, quaternion, scale, unit_scale):
    qx, qy, qz, qw = (float(value) for value in quaternion)
    length = math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw)
    if not length or not finite(translation) or not finite(quaternion) or not finite(scale):
        raise ValueError("transform values must be finite and quaternion length must be non-zero")
    qx, qy, qz, qw = qx / length, qy / length, qz / length, qw / length
    tx, ty, tz = (float(value) * unit_scale for value in translation)
    sx, sy, sz = (float(value) for value in scale)
    return [tx, tz, ty, -qx, -qz, -qy, qw, sx, sz, sy]
