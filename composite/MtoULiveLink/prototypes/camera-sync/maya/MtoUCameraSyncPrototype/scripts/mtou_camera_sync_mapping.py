"""Pure Unreal -> Maya camera sync mapping.

This module implements the mapping frozen in
``composite/MtoULiveLink/prototypes/camera-sync/protocol.md`` and imports
nothing outside the Python standard library, so every rule can be checked under
plain CPython without Maya. ``MtoUCameraSyncPrototype.py`` applies the values
this module produces to real Maya nodes.

Conventions
-----------
Unreal is left-handed, centimetres, ``+X`` forward, ``+Y`` right, ``+Z`` up.
Maya is right-handed, centimetres, ``+X`` right, ``+Y`` up, ``-Z`` forward.
The axis conversion used everywhere here is ``ue (x, y, z) -> maya (y, z, -x)``.

Film fit rule
-------------
Maya derives the projected field of view from the film aperture that the film
fit selects, and the resolution (device) aspect supplies the other axis:

* ``Horizontal`` (1) keeps the horizontal aperture exactly and derives the
  vertical extent as ``horizontal_aperture / gate_aspect``.
* ``Vertical`` (2) keeps the vertical aperture exactly and derives the
  horizontal extent as ``vertical_aperture * gate_aspect``.
* ``Fill`` (0) inscribes the resolution gate in the film gate: the rendered
  image is contained by the film back and touches it on one axis. When the gate
  is wider than the sensor the horizontal aperture is kept exactly and the
  derived height is smaller than the vertical aperture; otherwise the vertical
  aperture is kept and the image is narrower than the horizontal aperture.

The rule was confirmed against rendered images (see
``tests/maya_render_marker_check.py``): with a 2.39:1 sensor and a 16:9 gate,
Maya's ``Fill`` keeps the vertical aperture, which is what the formula below
produces.

Unreal fixes the field of view on the constraint axis and derives the other
axis from the output aspect, which is the same behaviour, so the mapping is:
``MaintainXFOV`` -> ``Horizontal``, ``MaintainYFOV`` -> ``Vertical``,
``MaintainMajorAxisFOV`` -> ``Horizontal`` for a landscape rendered image and
``Vertical`` for a portrait one. A payload that declares no constraint uses
``Fill``.

Projection formula
------------------
Maya's camera projection is a pinhole at the film plane. With the world point
``p`` expressed in camera space as ``(lx, ly, lz)`` (``+X`` right, ``+Y`` up,
``-Z`` forward, obtained from the camera world matrix rows), the focal length
``f`` and the film-plane coordinates in inches are

``u = f * lx / -lz`` and ``v = f * ly / -lz``.

The film gate centre is displaced by the film offsets ``(ox, oy)`` in inches,
and the film fit selects the image extent ``(ex, ey)`` on the film plane, so

``ndc_x = (u - ox) / (ex / 2)`` and ``ndc_y = (v - oy) / (ey / 2)``.

``ndc_y`` points up, matching Unreal's marker NDC. A lens squeeze ratio widens
the horizontal aperture before the extent is chosen, because the squeezed image
is desqueezed into the rendered pixels the NDC is measured on.

Time model
----------
``maya_time = maya_start_frame + (display_frame - playback_range.start) *
scene_fps / display_rate``. A non-integral result is reported as
``quantized=True`` and applied at the nearest integer frame.
"""

import json
import math

PROTOCOL_NAME = "MtoUCameraSync"
PROTOCOL_VERSION = 1

MM_PER_INCH = 25.4
CENTIMETRES_PER_INCH = 2.54

FILM_FIT_FILL = 0
FILM_FIT_HORIZONTAL = 1
FILM_FIT_VERTICAL = 2

FILM_FIT_NAMES = {
    FILM_FIT_FILL: "Fill",
    FILM_FIT_HORIZONTAL: "Horizontal",
    FILM_FIT_VERTICAL: "Vertical",
}

#: Maya needs a finite far clip plane where Unreal has none. Used when the
#: payload declares no fallback; the substitution is always reported.
DEFAULT_FAR_CLIP_CM = 100000.0

# Unreal EAspectRatioAxisConstraint values, by integer and by name.
ASPECT_AXIS_CONSTRAINTS = {
    0: "maintain_xfov",
    1: "maintain_yfov",
    2: "maintain_major_axis_fov",
}
_ASPECT_AXIS_ALIASES = {
    "maintain_xfov": "maintain_xfov",
    "maintainyfov": "maintain_yfov",
    "maintain_yfov": "maintain_yfov",
    "maintainxfov": "maintain_xfov",
    "maintain_major_axis_fov": "maintain_major_axis_fov",
    "maintainmajoraxisfov": "maintain_major_axis_fov",
    "major_axis_fov": "maintain_major_axis_fov",
}

# Stable payload error categories.
ERR_UNKNOWN_TYPE = "UNKNOWN_MESSAGE_TYPE"
ERR_MISSING_FIELD = "MISSING_FIELD"
ERR_INVALID_FIELD = "INVALID_FIELD"
ERR_UNSUPPORTED_PROTOCOL = "UNSUPPORTED_PROTOCOL"
ERR_UNSUPPORTED_AUTHORITY = "UNSUPPORTED_TIME_AUTHORITY"


class PayloadError(ValueError):
    """A rejected wire message. ``category`` is stable for tests and reports."""

    def __init__(self, category, detail):
        ValueError.__init__(self, "{0}: {1}".format(category, detail))
        self.category = category
        self.detail = detail


# --------------------------------------------------------------------------
# Axis conversion
# --------------------------------------------------------------------------

def ue_to_maya_point(x, y, z):
    """Map an Unreal vector/point to Maya coordinates: (y, z, -x)."""
    return (float(y), float(z), -float(x))


def maya_to_ue_point(x, y, z):
    """Inverse of :func:`ue_to_maya_point`, used by tests and diagnostics."""
    return (-float(z), float(x), float(y))


def ue_basis_vectors(rotation_deg):
    """Unreal world rotation (roll, pitch, yaw degrees) -> (right, up, forward).

    Uses Unreal's own ``FRotationMatrix`` element formulas with the basis
    vectors as rows, so a payload that carries only a rotation can still be
    applied without any Unreal code running here.
    """
    roll, pitch, yaw = (math.radians(float(value)) for value in rotation_deg)
    sp, cp = math.sin(pitch), math.cos(pitch)
    sy, cy = math.sin(yaw), math.cos(yaw)
    sr, cr = math.sin(roll), math.cos(roll)

    forward = (cp * cy, cp * sy, sp)
    right = (sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp)
    up = (-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp)
    return (right, up, forward)


def ue_basis_to_maya_matrix(right, up, forward, location):
    """Build a Maya world matrix from an Unreal basis and location.

    Returns a row-major tuple of 16 floats (``cmds.xform(matrix=...)`` order)
    where row 0 is the Maya X axis, row 1 the Y axis, row 2 the Z axis and row 3
    the translation. The basis is mapped as ``maya_x = right``, ``maya_y = up``,
    ``maya_z = -forward``, each through :func:`ue_to_maya_point`, because Maya
    cameras look down their local ``-Z``.
    """
    maya_x = ue_to_maya_point(*right)
    maya_y = ue_to_maya_point(*up)
    negated_forward = (-float(forward[0]), -float(forward[1]), -float(forward[2]))
    maya_z = ue_to_maya_point(*negated_forward)
    maya_t = ue_to_maya_point(*location)
    return (
        maya_x[0], maya_x[1], maya_x[2], 0.0,
        maya_y[0], maya_y[1], maya_y[2], 0.0,
        maya_z[0], maya_z[1], maya_z[2], 0.0,
        maya_t[0], maya_t[1], maya_t[2], 1.0,
    )


def camera_space_point(point_maya, camera_matrix):
    """World point -> camera space using the Maya world matrix rows as axes."""
    px = float(point_maya[0]) - float(camera_matrix[12])
    py = float(point_maya[1]) - float(camera_matrix[13])
    pz = float(point_maya[2]) - float(camera_matrix[14])
    return (
        px * camera_matrix[0] + py * camera_matrix[1] + pz * camera_matrix[2],
        px * camera_matrix[4] + py * camera_matrix[5] + pz * camera_matrix[6],
        px * camera_matrix[8] + py * camera_matrix[9] + pz * camera_matrix[10],
    )


# --------------------------------------------------------------------------
# Film back
# --------------------------------------------------------------------------

def film_aperture_inches(sensor_width_mm, sensor_height_mm):
    """Unreal filmback sensor size (mm) -> Maya film aperture (inches)."""
    return (float(sensor_width_mm) / MM_PER_INCH,
            float(sensor_height_mm) / MM_PER_INCH)


def film_offset_inches(horizontal_offset_mm, vertical_offset_mm):
    """Unreal sensor offsets (mm) -> Maya film offsets (inches)."""
    return (float(horizontal_offset_mm) / MM_PER_INCH,
            float(vertical_offset_mm) / MM_PER_INCH)


def normalize_aspect_constraint(value):
    """Unreal ``AspectRatioAxisConstraint`` -> canonical lowercase name.

    Accepts the integer enum value or any spelling of the three names. ``None``
    (absent field) returns ``None`` so the caller can choose Maya ``Fill``.
    """
    if value is None:
        return None
    if isinstance(value, bool):
        raise PayloadError(ERR_INVALID_FIELD, "aspect_axis_constraint is a bool")
    if isinstance(value, int):
        if value in ASPECT_AXIS_CONSTRAINTS:
            return ASPECT_AXIS_CONSTRAINTS[value]
        raise PayloadError(
            ERR_INVALID_FIELD, "unknown AspectRatioAxisConstraint {0}".format(value))
    if isinstance(value, str):
        key = value.strip().lower()
        if key in _ASPECT_AXIS_ALIASES:
            return _ASPECT_AXIS_ALIASES[key]
        raise PayloadError(
            ERR_INVALID_FIELD, "unknown AspectRatioAxisConstraint {0!r}".format(value))
    raise PayloadError(
        ERR_INVALID_FIELD,
        "aspect_axis_constraint must be a string or integer, got {0}".format(
            type(value).__name__))


def film_fit(aspect_axis_constraint, constrain_aspect_ratio,
             sensor_aspect_ratio, output_aspect_ratio):
    """Chooses Maya's film fit (``Fill`` 0, ``Horizontal`` 1, ``Vertical`` 2).

    ``MaintainXFOV`` keeps the horizontal field of view -> ``Horizontal``;
    ``MaintainYFOV`` keeps the vertical -> ``Vertical``. Unreal's
    ``MaintainMajorAxisFOV`` keeps the axis that is larger in the aspect ratio
    Unreal actually renders: the sensor aspect when the payload constrains the
    aspect ratio (the filmback aspect then overrides the output), otherwise the
    output aspect. A missing constraint has no axis to keep, so the whole
    sensor is used -> ``Fill``.
    """
    constraint = normalize_aspect_constraint(aspect_axis_constraint)
    if constraint == "maintain_xfov":
        return FILM_FIT_HORIZONTAL
    if constraint == "maintain_yfov":
        return FILM_FIT_VERTICAL
    if constraint == "maintain_major_axis_fov":
        aspect = (float(sensor_aspect_ratio) if constrain_aspect_ratio
                  else float(output_aspect_ratio))
        return FILM_FIT_HORIZONTAL if aspect >= 1.0 else FILM_FIT_VERTICAL
    return FILM_FIT_FILL


def film_fit_reason(aspect_axis_constraint, constrain_aspect_ratio,
                    sensor_aspect_ratio, output_aspect_ratio):
    """Human readable justification of the film fit choice, for reports."""
    fit = film_fit(aspect_axis_constraint, constrain_aspect_ratio,
                   sensor_aspect_ratio, output_aspect_ratio)
    name = FILM_FIT_NAMES[fit]
    constraint = normalize_aspect_constraint(aspect_axis_constraint)
    if constraint is None:
        return ("{0} (no axis constraint declared: the whole sensor aperture is "
                "used; the rendered aspect {1:.4f} must equal the sensor aspect "
                "{2:.4f} for an exact match)".format(
                    name, float(output_aspect_ratio), float(sensor_aspect_ratio)))
    if constraint == "maintain_xfov":
        return "{0} (MaintainXFOV keeps the horizontal aperture's field of view)".format(name)
    if constraint == "maintain_yfov":
        return "{0} (MaintainYFOV keeps the vertical aperture's field of view)".format(name)
    aspect = (float(sensor_aspect_ratio) if constrain_aspect_ratio
              else float(output_aspect_ratio))
    source = "constrained sensor aspect" if constrain_aspect_ratio else "output aspect"
    return ("{0} (MaintainMajorAxisFOV: the {1} {2:.4f} is {3})".format(
        name, source, aspect, "landscape" if aspect >= 1.0 else "portrait"))


def film_gate_extent_inches(aperture_inches, film_fit_value, resolution,
                            lens_squeeze_ratio=1.0):
    """Image extent on the film plane for the film fit and the gate aspect.

    ``aperture_inches`` is ``(horizontal, vertical)`` in inches (sensor size,
    before the squeeze). ``resolution`` is the rendered pixel extent, which is
    Maya's device aspect once the applier keeps ``defaultResolution`` in step.
    Returns the ``(width, height)`` in inches of the image window that the
    resolution gate covers.
    """
    aperture_x = float(aperture_inches[0]) * float(lens_squeeze_ratio)
    aperture_y = float(aperture_inches[1])
    width = float(resolution[0])
    height = float(resolution[1])
    if width <= 0.0 or height <= 0.0:
        raise ValueError("resolution must be positive, got {0!r}".format(resolution))
    gate_aspect = width / height
    if int(film_fit_value) == FILM_FIT_HORIZONTAL:
        return (aperture_x, aperture_x / gate_aspect)
    if int(film_fit_value) == FILM_FIT_VERTICAL:
        return (aperture_y * gate_aspect, aperture_y)
    if int(film_fit_value) == FILM_FIT_FILL:
        if gate_aspect >= aperture_x / aperture_y:
            return (aperture_x, aperture_x / gate_aspect)
        return (aperture_y * gate_aspect, aperture_y)
    raise ValueError("unknown film fit {0!r}".format(film_fit_value))


# --------------------------------------------------------------------------
# Time
# --------------------------------------------------------------------------

def display_rate_value(display_rate):
    """Rational ``{numerator, denominator}`` (or a number) -> frames per second."""
    if isinstance(display_rate, dict):
        numerator = float(display_rate["numerator"])
        denominator = float(display_rate["denominator"])
        if denominator == 0.0:
            raise PayloadError(ERR_INVALID_FIELD, "display_rate denominator is zero")
        return numerator / denominator
    value = float(display_rate)
    if value <= 0.0:
        raise PayloadError(ERR_INVALID_FIELD, "display_rate must be positive")
    return value


def maya_time_for_frame(display_frame, playback_start, display_rate,
                        scene_fps, maya_origin_frame):
    """Map two explicit range origins, retaining the fractional Maya frame.

    The second return value says whether the mapped time has a subframe. Maya
    2024 evaluates that time directly, including animated scene nodes.
    """
    rate = display_rate_value(display_rate)
    if rate <= 0.0:
        raise ValueError("display_rate must be positive")
    if float(scene_fps) <= 0.0:
        raise ValueError("scene_fps must be positive")
    time_value = (float(maya_origin_frame)
                  + (float(display_frame) - float(playback_start))
                  * float(scene_fps) / rate)
    return (time_value, not float(time_value).is_integer())


def applied_maya_time(maya_time, subframe):
    """The frame Maya is actually moved to; subframes are not rounded."""
    return float(maya_time)


# --------------------------------------------------------------------------
# Projection
# --------------------------------------------------------------------------

def project_point_ndc(point_maya, camera_matrix, focal_length_mm,
                      film_aperture_inches, film_fit, resolution,
                      film_offset=None, lens_squeeze_ratio=1.0):
    """Project a Maya world point with Maya's own film-aperture projection.

    Returns ``(ndc_x, ndc_y)`` in ``-1..1`` with ``+Y`` up. See the module
    docstring for the formula. ``film_offset`` is ``(horizontal, vertical)`` in
    inches and defaults to no offset. Raises ``ValueError`` for a point on or
    behind the film plane, where the pinhole projection is undefined.
    """
    local_x, local_y, local_z = camera_space_point(point_maya, camera_matrix)
    if local_z >= 0.0:
        raise ValueError("point is on or behind the film plane (camera z={0:.6f})".format(
            local_z))
    focal_inches = float(focal_length_mm) / MM_PER_INCH
    extent_x, extent_y = film_gate_extent_inches(
        film_aperture_inches, film_fit, resolution, lens_squeeze_ratio)
    offset_x, offset_y = (0.0, 0.0) if film_offset is None else (
        float(film_offset[0]), float(film_offset[1]))
    distance = -local_z
    u = focal_inches * local_x / distance
    v = focal_inches * local_y / distance
    return ((u - offset_x) / (extent_x / 2.0), (v - offset_y) / (extent_y / 2.0))


# --------------------------------------------------------------------------
# Payload validation and normalization
# --------------------------------------------------------------------------

_SESSION_FIELDS = ("protocol", "version", "port", "time_authority", "sequence",
                   "display_rate", "tick_resolution", "playback_range",
                   "output_resolution", "camera_cut")
_FRAME_FIELDS = ("session", "sequence", "frame_serial", "time", "camera_cut",
                 "output_resolution", "camera", "view", "projection", "markers")
_ERROR_FIELDS = ("category", "detail")
_END_FIELDS = ("reason",)

MESSAGE_FIELDS = {
    "session": _SESSION_FIELDS,
    "frame": _FRAME_FIELDS,
    "error": _ERROR_FIELDS,
    "end": _END_FIELDS,
}

_REQUIRED_CAMERA_FIELDS = ("focal_length_mm", "sensor_width_mm", "sensor_height_mm")

# Tolerated alternative spellings, canonical name first.
_CAMERA_ALIASES = {
    "location": ("location", "world_location"),
    "focal_length_mm": ("focal_length_mm", "current_focal_length_mm"),
    "focus_distance_cm": ("focus_distance_cm", "manual_focus_distance_cm"),
    "squeeze_factor": ("squeeze_factor", "lens_squeeze_ratio"),
    "far_clip_cm": ("far_clip_cm", "far_clip_fallback_cm"),
    "constrain_aspect_ratio": ("constrain_aspect_ratio", "b_constrain_aspect_ratio"),
    "aspect_axis_constraint": ("aspect_axis_constraint",),
    "sensor_width_mm": ("sensor_width_mm",),
    "sensor_height_mm": ("sensor_height_mm",),
    "sensor_horizontal_offset_mm": ("sensor_horizontal_offset_mm",),
    "sensor_vertical_offset_mm": ("sensor_vertical_offset_mm",),
    "f_stop": ("f_stop", "current_aperture"),
    "depth_of_field": ("depth_of_field",),
    "near_clip_cm": ("near_clip_cm",),
    "right": ("right",),
    "up": ("up",),
    "forward": ("forward",),
    "rotation": ("rotation",),
}


def get_field(mapping, *names):
    """First present key of ``names``; raises ``PayloadError`` when none is."""
    for name in names:
        if name in mapping:
            return mapping[name]
    raise PayloadError(ERR_MISSING_FIELD, "missing field {0!r}".format(names[0]))


def optional_field(mapping, *names):
    for name in names:
        if name in mapping:
            return mapping[name]
    return None


def camera_field(camera, canonical):
    """Canonical or alias spelling of one camera field, or ``None``."""
    return optional_field(camera, *_CAMERA_ALIASES[canonical])


def require_mapping(value, label):
    if not isinstance(value, dict):
        raise PayloadError(ERR_INVALID_FIELD,
                           "{0} must be an object, got {1}".format(
                               label, type(value).__name__))
    return value


def require_number(value, label):
    """Numeric field check; rejects bools, strings and non-finite values."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise PayloadError(ERR_INVALID_FIELD,
                           "{0} must be a number, got {1!r}".format(label, value))
    value = float(value)
    if not math.isfinite(value):
        raise PayloadError(ERR_INVALID_FIELD,
                           "{0} must be finite, got {1!r}".format(label, value))
    return value


def require_sequence(value, label):
    if not isinstance(value, (list, tuple)):
        raise PayloadError(ERR_INVALID_FIELD,
                           "{0} must be a list, got {1}".format(
                               label, type(value).__name__))
    return value


def _check_required_fields(message, message_type):
    for field in MESSAGE_FIELDS[message_type]:
        if field not in message:
            raise PayloadError(
                ERR_MISSING_FIELD,
                "{0} message is missing field {1!r}".format(message_type, field))
    return message


def _validate_session(message):
    protocol = message["protocol"]
    if protocol != PROTOCOL_NAME:
        raise PayloadError(ERR_UNSUPPORTED_PROTOCOL,
                           "protocol {0!r}, expected {1!r}".format(protocol, PROTOCOL_NAME))
    version = message["version"]
    if version != PROTOCOL_VERSION:
        raise PayloadError(ERR_UNSUPPORTED_PROTOCOL,
                           "version {0!r}, expected {1}".format(version, PROTOCOL_VERSION))
    authority = message["time_authority"]
    if authority != "unreal":
        raise PayloadError(ERR_UNSUPPORTED_AUTHORITY,
                           "time_authority {0!r}, expected 'unreal'".format(authority))
    playback_range = require_mapping(message["playback_range"], "playback_range")
    get_field(playback_range, "start")
    get_field(playback_range, "end")
    resolution = require_mapping(message["output_resolution"], "output_resolution")
    _resolution_pixels(resolution)
    display_rate_value(message["display_rate"])
    return message


def _resolution_pixels(resolution):
    x = require_number(get_field(resolution, "x"), "output_resolution.x")
    y = require_number(get_field(resolution, "y"), "output_resolution.y")
    if x <= 0.0 or y <= 0.0:
        raise PayloadError(ERR_INVALID_FIELD,
                           "output_resolution must be positive, got {0}x{1}".format(x, y))
    return (x, y)


def _validate_markers(markers):
    require_sequence(markers, "markers")
    for index, marker in enumerate(markers):
        require_mapping(marker, "markers[{0}]".format(index))
        get_field(marker, "name")
        location = require_mapping(
            get_field(marker, "location"), "markers[{0}].location".format(index))
        for axis in ("x", "y", "z"):
            require_number(get_field(location, axis),
                           "markers[{0}].location.{1}".format(index, axis))
        ndc = require_mapping(
            get_field(marker, "ndc"), "markers[{0}].ndc".format(index))
        for axis in ("x", "y"):
            require_number(get_field(ndc, axis), "markers[{0}].ndc.{1}".format(index, axis))
    return markers


_NUMERIC_CAMERA_FIELDS = ("sensor_horizontal_offset_mm", "sensor_vertical_offset_mm",
                          "f_stop", "focus_distance_cm", "squeeze_factor",
                          "near_clip_cm")


def _validate_camera(camera):
    require_mapping(camera, "camera")
    for canonical in _REQUIRED_CAMERA_FIELDS:
        if camera_field(camera, canonical) is None:
            raise PayloadError(
                ERR_MISSING_FIELD,
                "camera is missing field {0!r}".format(_CAMERA_ALIASES[canonical][0]))
        require_number(camera_field(camera, canonical),
                       "camera.{0}".format(_CAMERA_ALIASES[canonical][0]))
    for canonical in _NUMERIC_CAMERA_FIELDS:
        value = camera_field(camera, canonical)
        if value is not None:
            require_number(value, "camera.{0}".format(_CAMERA_ALIASES[canonical][0]))
    for canonical in ("constrain_aspect_ratio", "depth_of_field"):
        value = camera_field(camera, canonical)
        if value is not None and not isinstance(value, bool):
            raise PayloadError(ERR_INVALID_FIELD,
                               "camera.{0} must be a bool".format(canonical))
    location = camera_field(camera, "location")
    rotation = camera_field(camera, "rotation")
    basis = [camera_field(camera, name) for name in ("right", "up", "forward")]
    if location is None:
        raise PayloadError(ERR_MISSING_FIELD, "camera is missing field 'location'")
    location = require_mapping(location, "camera.location")
    for axis in ("x", "y", "z"):
        require_number(get_field(location, axis), "camera.location." + axis)
    if any(vector is not None for vector in basis):
        for name, vector in zip(("right", "up", "forward"), basis):
            if vector is None:
                raise PayloadError(
                    ERR_MISSING_FIELD,
                    "camera supplies a basis but not {0!r}".format(name))
            vector = require_mapping(vector, "camera." + name)
            for axis in ("x", "y", "z"):
                require_number(get_field(vector, axis), "camera.{0}.{1}".format(name, axis))
    elif rotation is None:
        raise PayloadError(
            ERR_MISSING_FIELD,
            "camera needs either right/up/forward or rotation")
    else:
        rotation = require_mapping(rotation, "camera.rotation")
        for axis in ("roll", "pitch", "yaw"):
            require_number(get_field(rotation, axis), "camera.rotation." + axis)
    normalize_aspect_constraint(camera_field(camera, "aspect_axis_constraint"))
    return camera


def _validate_frame(message):
    require_number(get_field(message, "frame_serial"), "frame_serial")
    time_payload = require_mapping(message["time"], "time")
    require_number(get_field(time_payload, "display_frame"), "time.display_frame")
    display_rate = optional_field(time_payload, "display_rate")
    if display_rate is not None:
        display_rate_value(display_rate)
    resolution = require_mapping(message["output_resolution"], "output_resolution")
    _resolution_pixels(resolution)
    _validate_camera(message["camera"])
    _validate_markers(message["markers"])
    return message


def validate_message(message, expected_type=None):
    """Validate one decoded wire message; returns it unchanged when valid.

    Rejects unknown ``type`` values and missing fields with a stable
    ``PayloadError.category``. Nothing is applied unless the whole message
    validates, so a rejected frame never leaves a partially updated camera.
    """
    require_mapping(message, "message")
    message_type = get_field(message, "type")
    if message_type not in MESSAGE_FIELDS:
        raise PayloadError(ERR_UNKNOWN_TYPE,
                           "unknown message type {0!r}".format(message_type))
    if expected_type is not None and message_type != expected_type:
        raise PayloadError(ERR_INVALID_FIELD,
                           "expected {0!r}, got {1!r}".format(expected_type, message_type))
    _check_required_fields(message, message_type)
    if message_type == "session":
        _validate_session(message)
    elif message_type == "frame":
        _validate_frame(message)
    return message


def decode_message(line):
    """Decode and validate one UTF-8 JSON line from the wire."""
    try:
        message = json.loads(line)
    except ValueError as error:
        raise PayloadError(ERR_INVALID_FIELD, "invalid JSON: {0}".format(error))
    return validate_message(message)


# --------------------------------------------------------------------------
# Normalized application values
# --------------------------------------------------------------------------

def payload_basis(camera):
    """(right, up, forward) Unreal vectors from a camera payload."""
    if camera_field(camera, "right") is not None:
        def triple(name):
            vector = camera_field(camera, name)
            return tuple(require_number(get_field(vector, axis),
                                        "camera.{0}.{1}".format(name, axis))
                         for axis in ("x", "y", "z"))
        return (triple("right"), triple("up"), triple("forward"))
    rotation = camera_field(camera, "rotation")
    return ue_basis_vectors(tuple(
        require_number(get_field(rotation, axis), "camera.rotation." + axis)
        for axis in ("roll", "pitch", "yaw")))


def payload_location(camera):
    location = camera_field(camera, "location")
    return tuple(require_number(get_field(location, axis), "camera.location." + axis)
                 for axis in ("x", "y", "z"))


def resolve_far_clip(camera, session, frame=None):
    """Maya far clip for one frame. Returns ``(centimetres, substituted, source)``.

    Unreal has no far clip plane, so the payload's declared fallback is used
    whenever the camera does not carry a usable finite value, and the
    substitution is reported to Unreal.
    """
    candidates = (
        ("camera", camera_field(camera, "far_clip_cm")),
        ("frame", None if frame is None else optional_field(frame, "far_clip_cm")),
        ("session_fallback",
         None if session is None else optional_field(session, "far_clip_fallback_cm")),
    )
    for source, candidate in candidates:
        if candidate is None or isinstance(candidate, bool):
            continue
        try:
            value = float(candidate)
        except (TypeError, ValueError):
            continue
        if math.isfinite(value) and value > 0.0:
            return (value, source != "camera", source)
    return (DEFAULT_FAR_CLIP_CM, True, "prototype_default")


def camera_parameters(camera, session, resolution, frame=None):
    """Normalized Maya attribute values for one frame payload.

    Returns ``(attributes, notes)`` where ``attributes`` maps Maya attribute
    names to the values to write and ``notes`` records what the prototype had
    to adapt (film fit justification, far clip substitution, and the raw Unreal
    values used to derive the projection).
    """
    require_mapping(camera, "camera")
    sensor_width_mm = require_number(camera_field(camera, "sensor_width_mm"),
                                     "camera.sensor_width_mm")
    sensor_height_mm = require_number(camera_field(camera, "sensor_height_mm"),
                                      "camera.sensor_height_mm")
    if sensor_width_mm <= 0.0 or sensor_height_mm <= 0.0:
        raise PayloadError(ERR_INVALID_FIELD, "sensor size must be positive")
    focal_length_mm = require_number(camera_field(camera, "focal_length_mm"),
                                     "camera.focal_length_mm")
    if focal_length_mm <= 0.0:
        raise PayloadError(ERR_INVALID_FIELD, "focal_length_mm must be positive")

    aperture = film_aperture_inches(sensor_width_mm, sensor_height_mm)
    offset_mm = camera_field(camera, "sensor_horizontal_offset_mm")
    offset_mm_v = camera_field(camera, "sensor_vertical_offset_mm")
    offsets = film_offset_inches(
        0.0 if offset_mm is None else require_number(offset_mm, "camera.sensor_horizontal_offset_mm"),
        0.0 if offset_mm_v is None else require_number(offset_mm_v, "camera.sensor_vertical_offset_mm"))
    squeeze = camera_field(camera, "squeeze_factor")
    squeeze = 1.0 if squeeze is None else require_number(squeeze, "camera.squeeze_factor")
    if squeeze <= 0.0:
        raise PayloadError(ERR_INVALID_FIELD, "squeeze_factor must be positive")

    sensor_aspect = sensor_width_mm / sensor_height_mm
    output_aspect = float(resolution[0]) / float(resolution[1])
    constrain = bool(camera_field(camera, "constrain_aspect_ratio"))
    constraint = camera_field(camera, "aspect_axis_constraint")
    fit = film_fit(constraint, constrain, sensor_aspect, output_aspect)

    far_clip, far_clip_substituted, far_clip_source = resolve_far_clip(camera, session, frame)

    near_clip = camera_field(camera, "near_clip_cm")
    attributes = {
        "focalLength": focal_length_mm,
        "horizontalFilmAperture": aperture[0],
        "verticalFilmAperture": aperture[1],
        "horizontalFilmOffset": offsets[0],
        "verticalFilmOffset": offsets[1],
        "filmFit": fit,
        "lensSqueezeRatio": squeeze,
        "nearClipPlane": 1.0 if near_clip is None else require_number(
            near_clip, "camera.near_clip_cm"),
        "farClipPlane": far_clip,
    }
    f_stop = camera_field(camera, "f_stop")
    if f_stop is not None:
        f_stop = require_number(f_stop, "camera.f_stop")
        if f_stop <= 0.0:
            raise PayloadError(ERR_INVALID_FIELD, "f_stop must be positive")
        attributes["fStop"] = f_stop
    focus_distance = camera_field(camera, "focus_distance_cm")
    if focus_distance is not None:
        focus_distance = require_number(focus_distance, "camera.focus_distance_cm")
        if focus_distance <= 0.0:
            raise PayloadError(ERR_INVALID_FIELD, "focus_distance_cm must be positive")
        attributes["focusDistance"] = focus_distance
    depth_of_field = camera_field(camera, "depth_of_field")
    if depth_of_field is not None:
        attributes["depthOfField"] = bool(depth_of_field)

    notes = {
        "film_fit": fit,
        "film_fit_name": FILM_FIT_NAMES[fit],
        "film_fit_reason": film_fit_reason(constraint, constrain, sensor_aspect,
                                           output_aspect),
        "far_clip_substituted": far_clip_substituted,
        "far_clip_source": far_clip_source,
        "far_clip_cm": far_clip,
        "requested_far_clip_cm": camera_field(camera, "far_clip_cm"),
        "sensor_aspect": sensor_aspect,
        "output_aspect": output_aspect,
        "aperture_inches": aperture,
        "film_offset_inches": offsets,
        "lens_squeeze_ratio": squeeze,
        "focal_length_mm": focal_length_mm,
    }
    return (attributes, notes)


def marker_report(markers, camera_matrix, notes, resolution):
    """Maya marker NDC and the delta against Unreal's NDC for each marker."""
    report = []
    for marker in markers:
        location = marker["location"]
        entry = {
            "name": marker["name"],
            "unreal_ndc": [float(marker["ndc"]["x"]), float(marker["ndc"]["y"])],
            "maya_ndc": None,
            "delta": None,
        }
        try:
            maya_ndc = project_point_ndc(
                ue_to_maya_point(location["x"], location["y"], location["z"]),
                camera_matrix, notes["focal_length_mm"], notes["aperture_inches"],
                notes["film_fit"], resolution, notes["film_offset_inches"],
                notes["lens_squeeze_ratio"])
            entry["maya_ndc"] = [maya_ndc[0], maya_ndc[1]]
            entry["delta"] = [maya_ndc[0] - entry["unreal_ndc"][0],
                              maya_ndc[1] - entry["unreal_ndc"][1]]
        except ValueError as error:
            entry["error"] = "UNPROJECTABLE: {0}".format(error)
        report.append(entry)
    return report


def ndc_to_pixel(ndc, resolution):
    """NDC (``-1..1``, ``+Y`` up) -> pixel centre coordinates, row 0 at the top."""
    width = float(resolution[0])
    height = float(resolution[1])
    return ((float(ndc[0]) + 1.0) / 2.0 * width - 0.5,
            (1.0 - float(ndc[1])) / 2.0 * height - 0.5)
