"""Unreal -> Maya camera application for the v10 linkage contract.

Two layers live here. The upper one is pure Python: the mapping frozen in the
camera-sync prototype (``composite/MtoULiveLink/prototypes/camera-sync``) ported
into the product package so nothing imports the prototype at runtime, and the
payload validation one v10 ``camera_time`` message has to pass. The lower one is
:class:`LinkCamera`, the Maya host adapter that writes one target to one
tool-owned camera plus the resolution gate, reads the values Maya actually
holds back for the ``applied`` evidence, and restores the scene it borrowed.

Conventions (identical to the prototype)
----------------------------------------
Unreal is left-handed, centimetres, ``+X`` forward, ``+Y`` right, ``+Z`` up.
Maya is right-handed, centimetres, ``+X`` right, ``+Y`` up, ``-Z`` forward.
The axis conversion used everywhere here is ``ue (x, y, z) -> maya (y, z, -x)``.

Sensor millimetres become Maya film-aperture inches (``/ 25.4``, signs
preserved). Unreal fixes the field of view on its constraint axis, which Maya
expresses as ``filmFit``: ``MaintainXFOV -> Horizontal`` (1),
``MaintainYFOV -> Vertical`` (2), ``MaintainMajorAxisFOV -> Horizontal`` for a
landscape rendered image and ``Vertical`` for a portrait one, and no declared
constraint -> ``Fill`` (0). Maya derives the fitted axis from the resolution
gate's device aspect, so the gate's pixel extent and device aspect are written
with the camera attributes.

Scene safety
------------
Nothing is written before every refusal check has passed: the scene linear
unit must be centimetres, every target attribute must be writable and static,
every model panel camera and every gate plug must be writable, and the camera
name must not be taken by a foreign node. The camera edits and the gate share
one undo chunk that is undone on failure, so a refused or failed target leaves
the scene exactly as it was. :meth:`LinkCamera.capture_state` and
:meth:`LinkCamera.restore_state` put back the look-through camera of each model
panel, the resolution-gate attributes, the undo state, and delete only the
camera this adapter created.
"""

import math

from .config import LINK_CAMERA_NAME
from .mayaapi import cmds, om

#: Millimetres per inch; Unreal reports the filmback in millimetres and Maya
#: the film aperture in inches.
MM_PER_INCH = 25.4

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
ERR_MISSING_FIELD = "MISSING_FIELD"
ERR_INVALID_FIELD = "INVALID_FIELD"

# Stable scene-refusal categories, mirroring the camera-sync prototype.
CATEGORY_UNSUPPORTED_SCENE_UNIT = "UNSUPPORTED_SCENE_LINEAR_UNIT"
CATEGORY_LOCKED_ATTRIBUTE = "LOCKED_SYNC_ATTRIBUTE"
CATEGORY_CAMERA_INPUT_DRIVEN = "CAMERA_INPUT_DRIVEN"
CATEGORY_CAMERA_NAME_CONFLICT = "CAMERA_NAME_CONFLICT"
CATEGORY_HOST_STATE_MISMATCH = "HOST_STATE_MISMATCH"
CATEGORY_NO_CAMERA_YET = "NO_CAMERA_YET"

# Attributes this adapter owns on the tool-owned camera shape.
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

# Transform plugs the world matrix is written through.
TRANSFORM_WRITE_ATTRIBUTES = ("translateX", "translateY", "translateZ",
                              "rotateX", "rotateY", "rotateZ",
                              "scaleX", "scaleY", "scaleZ",
                              "shearXY", "shearXZ", "shearYZ")

# The Maya resolution gate this adapter writes and restores.
RESOLUTION_PLUGS = ("defaultResolution.width", "defaultResolution.height",
                    "defaultResolution.pixelAspect", "defaultResolution.deviceAspectRatio")


class PayloadError(ValueError):
    """A rejected camera payload. ``category`` is stable for tests and reports."""

    def __init__(self, category, detail):
        ValueError.__init__(self, "{0}: {1}".format(category, detail))
        self.category = category
        self.detail = detail


class CameraSyncRefused(RuntimeError):
    """A scene configuration this adapter refuses instead of guessing."""

    def __init__(self, category, detail):
        RuntimeError.__init__(self, "{0}: {1}".format(category, detail))
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
    return float(display_rate)


def maya_time_for_frame(display_frame, playback_start, display_rate,
                        maya_fps, maya_origin):
    """Map one Unreal display frame to Maya time, retaining fractional frames.

    ``maya_time = maya_origin + (display_frame - playback_start) * maya_fps /
    display_rate`` with ``display_rate`` the exact rational
    ``{numerator, denominator}`` object from the message: 23.976 is 24000/1001,
    never a rounded 24. The origin is explicit, so the result never depends on
    Maya's current frame when linkage started. A non-integral result is a
    fractional frame Maya 2024 evaluates directly; it is never rounded.
    """
    rate = display_rate_value(display_rate)
    if rate <= 0.0:
        raise ValueError("display_rate must be positive")
    if float(maya_fps) <= 0.0:
        raise ValueError("maya_fps must be positive")
    return (float(maya_origin)
            + (float(display_frame) - float(playback_start)) * float(maya_fps) / rate)


def applied_maya_time(maya_time, subframe=None):
    """The frame Maya is actually moved to; subframes are not rounded.

    ``subframe`` is accepted for call-site symmetry with
    :func:`maya_time_for_frame`'s report and ignored: ``maya_time`` is already
    the frame Maya evaluates.
    """
    return float(maya_time)


# --------------------------------------------------------------------------
# Projection
# --------------------------------------------------------------------------

def project_point_ndc(point_maya, camera_matrix, focal_length_mm,
                      film_aperture_inches, film_fit, resolution,
                      film_offset=None, lens_squeeze_ratio=1.0):
    """Project a Maya world point with Maya's own film-aperture projection.

    Returns ``(ndc_x, ndc_y)`` in ``-1..1`` with ``+Y`` up. With the world point
    ``p`` expressed in camera space as ``(lx, ly, lz)`` and the focal length
    ``f`` in inches, the film-plane coordinates are ``u = f * lx / -lz`` and
    ``v = f * ly / -lz``; the film gate centre is displaced by the film offsets
    and the film fit selects the image extent, so

    ``ndc_x = (u - ox) / (ex / 2)`` and ``ndc_y = (v - oy) / (ey / 2)``.

    Raises ``ValueError`` for a point on or behind the film plane, where the
    pinhole projection is undefined.
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
# Payload accessors and validators
# --------------------------------------------------------------------------

_REQUIRED_CAMERA_FIELDS = ("focal_length_mm", "sensor_width_mm", "sensor_height_mm")

# Tolerated alternative spellings, canonical name first.
_CAMERA_ALIASES = {
    "location": ("location", "world_location", "component_location"),
    "focal_length_mm": ("focal_length_mm", "current_focal_length_mm"),
    "focus_distance_cm": ("focus_distance_cm", "manual_focus_distance_cm"),
    "squeeze_factor": ("squeeze_factor", "lens_squeeze_ratio"),
    "far_clip_cm": ("far_clip_cm",),
    "constrain_aspect_ratio": ("constrain_aspect_ratio", "b_constrain_aspect_ratio"),
    "aspect_axis_constraint": ("aspect_axis_constraint",),
    "sensor_width_mm": ("sensor_width_mm",),
    "sensor_height_mm": ("sensor_height_mm",),
    "sensor_aspect_ratio": ("sensor_aspect_ratio", "sensor_aspect", "aspect_ratio"),
    "rendered_aspect_ratio": ("rendered_aspect_ratio", "output_aspect_ratio"),
    "sensor_horizontal_offset_mm": ("sensor_horizontal_offset_mm",),
    "sensor_vertical_offset_mm": ("sensor_vertical_offset_mm",),
    "f_stop": ("f_stop", "current_aperture", "aperture"),
    "focus_method": ("focus_method",),
    "depth_of_field": ("depth_of_field",),
    "near_clip_cm": ("near_clip_cm",),
    "right": ("right",),
    "up": ("up",),
    "forward": ("forward",),
    "rotation": ("rotation",),
}

# Tolerated alternative spellings of the resolution gate, canonical name first.
_GATE_ALIASES = {
    "output_resolution": ("output_resolution", "resolution"),
    "output_width": ("output_width",),
    "output_height": ("output_height",),
    "aperture_resolution": ("aperture_resolution",),
    "aperture_width": ("aperture_width",),
    "aperture_height": ("aperture_height",),
    "width": ("width",),
    "height": ("height",),
    "pixel_aspect_ratio": ("pixel_aspect_ratio", "pixel_aspect"),
    "resolution_source": ("resolution_source",),
    "overscan": ("overscan",),
    "overscan_resolution_fraction": ("overscan_resolution_fraction",),
    "aspect_ratio": ("aspect_ratio",),
}

# ``focus_method`` values that mean Maya's depth of field stays off.
_DISABLED_FOCUS_METHODS = ("disable", "disabled", "none", "off")

# Required fields of one v10 ``camera_time`` payload, with their categories.
_CAMERA_TIME_FIELDS = ("session_id", "set_revision", "mode_revision", "space_revision",
                       "eval_id", "eval_serial", "transport_serial", "frame_serial",
                       "time", "camera", "gate")
_CAMERA_TIME_NUMBERS = ("session_id", "set_revision", "mode_revision", "space_revision",
                        "eval_serial", "transport_serial", "frame_serial")
_TIME_NUMBERS = ("display_frame", "source_frame", "seconds", "tick")
_TIME_RATIONALS = ("display_rate", "tick_resolution")


def optional_field(mapping, *names):
    for name in names:
        if name in mapping:
            return mapping[name]
    return None


def get_field(mapping, *names):
    """First present key of ``names``; raises ``PayloadError`` when none is."""
    for name in names:
        if name in mapping:
            return mapping[name]
    raise PayloadError(ERR_MISSING_FIELD, "missing field {0!r}".format(names[0]))


def camera_field(camera, canonical):
    """Canonical or alias spelling of one camera field, or ``None``."""
    return optional_field(camera, *_CAMERA_ALIASES[canonical])


def gate_field(gate, canonical):
    """Canonical or alias spelling of one resolution-gate field, or ``None``."""
    return optional_field(gate, *_GATE_ALIASES[canonical])


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


def require_vector(value, label):
    """A ``{x, y, z}`` object or a three number sequence, as a 3-tuple."""
    if isinstance(value, dict):
        missing = [axis for axis in ("x", "y", "z") if axis not in value]
        if missing:
            raise PayloadError(ERR_MISSING_FIELD,
                               "{0} is missing component {1!r}".format(label, missing[0]))
        return tuple(require_number(value[axis], "{0}.{1}".format(label, axis))
                     for axis in ("x", "y", "z"))
    require_sequence(value, label)
    if len(value) != 3:
        raise PayloadError(ERR_INVALID_FIELD,
                           "{0} must have three components, got {1}".format(
                               label, len(value)))
    return tuple(require_number(component, "{0}[{1}]".format(label, index))
                 for index, component in enumerate(value))


def _gate_pair(gate, ladder, label):
    """The first ``(width, height)`` extent in ``ladder`` the gate declares.

    Each ladder rung is ``(nested object, flat width key, flat height key)``;
    the nested object may be ``None`` to accept the flat pair alone. Half a
    pair is a missing field, never a zero.
    """
    for nested, flat_width, flat_height in ladder:
        value = gate_field(gate, nested) if nested else None
        if value is not None:
            if isinstance(value, dict):
                missing = [axis for axis in ("x", "y") if axis not in value]
                if missing:
                    raise PayloadError(ERR_MISSING_FIELD, "gate.{0} is missing {1!r}".format(
                        nested, missing[0]))
                return (require_number(value["x"], "gate.{0}.x".format(nested)),
                        require_number(value["y"], "gate.{0}.y".format(nested)))
            require_sequence(value, "gate.{0}".format(nested))
            if len(value) != 2:
                raise PayloadError(ERR_INVALID_FIELD,
                                   "gate.{0} must have two components".format(nested))
            return (require_number(value[0], "gate.{0}[0]".format(nested)),
                    require_number(value[1], "gate.{0}[1]".format(nested)))
        width = gate_field(gate, flat_width)
        height = gate_field(gate, flat_height)
        if width is None and height is None:
            continue
        if width is None or height is None:
            raise PayloadError(ERR_MISSING_FIELD,
                               "gate declares only one of {0!r}/{1!r}".format(
                                   flat_width, flat_height))
        return (require_number(width, "gate.{0}".format(flat_width)),
                require_number(height, "gate.{0}".format(flat_height)))
    return None


def gate_extent_pixels(gate):
    """The rendered pixel extent the gate declares: ``(width, height)``.

    ``output_resolution{x, y}`` is the v10 spelling; ``output_width``/
    ``output_height`` and the ``width``/``height`` mirror the Unreal side also
    writes for the restore shape are accepted, so one payload serves both.
    """
    require_mapping(gate, "gate")
    return _gate_pair(gate, (("output_resolution", "output_width", "output_height"),
                             (None, "width", "height")), "gate output extent")


def gate_framing_pixels(gate):
    """The film-aperture pixel extent Unreal rendered inside the output.

    Maya derives the fitted axis from the resolution gate's device aspect, and
    Unreal renders the sensor aperture inside the output gate and leaves bars
    where the two aspects differ, so the film fit follows the aperture the
    publisher reports and falls back to the output extent.
    """
    require_mapping(gate, "gate")
    pair = _gate_pair(gate, (("aperture_resolution", "aperture_width",
                              "aperture_height"),), "gate aperture extent")
    return gate_extent_pixels(gate) if pair is None else pair


def gate_pixel_aspect(gate):
    """The pixel aspect the gate declares; ``1.0`` when it declares none."""
    value = gate_field(gate, "pixel_aspect_ratio")
    if value is None:
        return 1.0
    aspect = require_number(value, "gate.pixel_aspect_ratio")
    if aspect <= 0.0:
        raise PayloadError(ERR_INVALID_FIELD, "gate.pixel_aspect_ratio must be positive")
    return aspect


def gate_device_pixels(gate):
    """The resolution gate's device pixels: pixel extent times pixel aspect.

    Maya derives the fitted axis from ``defaultResolution.deviceAspectRatio``,
    which is the pixel extent times the pixel aspect, so the film fit, the
    projection and the gate that is written must all follow this value.
    """
    framing = gate_framing_pixels(gate)
    return (framing[0] * gate_pixel_aspect(gate), framing[1])


def _validate_camera(camera):
    require_mapping(camera, "camera")
    for canonical in _REQUIRED_CAMERA_FIELDS:
        if camera_field(camera, canonical) is None:
            raise PayloadError(
                ERR_MISSING_FIELD,
                "camera is missing field {0!r}".format(_CAMERA_ALIASES[canonical][0]))
        require_number(camera_field(camera, canonical),
                       "camera.{0}".format(_CAMERA_ALIASES[canonical][0]))
    for canonical in ("sensor_horizontal_offset_mm", "sensor_vertical_offset_mm",
                      "sensor_aspect_ratio", "rendered_aspect_ratio", "f_stop",
                      "focus_distance_cm", "squeeze_factor", "near_clip_cm",
                      "far_clip_cm"):
        value = camera_field(camera, canonical)
        if value is not None:
            require_number(value, "camera.{0}".format(_CAMERA_ALIASES[canonical][0]))
    for canonical in ("constrain_aspect_ratio", "depth_of_field"):
        value = camera_field(camera, canonical)
        if value is not None and not isinstance(value, bool):
            raise PayloadError(ERR_INVALID_FIELD,
                               "camera.{0} must be a bool".format(canonical))
    method = camera_field(camera, "focus_method")
    if method is not None and (not isinstance(method, str) or not method.strip()):
        raise PayloadError(ERR_INVALID_FIELD,
                           "camera.focus_method must be a non-empty string")
    location = camera_field(camera, "location")
    if location is None:
        raise PayloadError(ERR_MISSING_FIELD, "camera is missing field 'location'")
    require_vector(location, "camera.location")
    basis = [camera_field(camera, name) for name in ("right", "up", "forward")]
    rotation = camera_field(camera, "rotation")
    if any(vector is not None for vector in basis):
        for name, vector in zip(("right", "up", "forward"), basis):
            if vector is None:
                raise PayloadError(
                    ERR_MISSING_FIELD,
                    "camera supplies a basis but not {0!r}".format(name))
            require_vector(vector, "camera." + name)
    elif rotation is None:
        raise PayloadError(
            ERR_MISSING_FIELD,
            "camera needs either right/up/forward or rotation")
    elif isinstance(rotation, dict):
        for axis in ("roll", "pitch", "yaw"):
            if axis not in rotation:
                raise PayloadError(
                    ERR_MISSING_FIELD, "camera.rotation is missing field {0!r}".format(axis))
            require_number(rotation[axis], "camera.rotation." + axis)
    else:
        require_vector(rotation, "camera.rotation")
    normalize_aspect_constraint(camera_field(camera, "aspect_axis_constraint"))
    return camera


def _validate_gate(gate):
    require_mapping(gate, "gate")
    output = gate_extent_pixels(gate)
    if output is None:
        raise PayloadError(ERR_MISSING_FIELD,
                           "gate is missing field 'output_resolution' or 'width'")
    if output[0] <= 0.0 or output[1] <= 0.0:
        raise PayloadError(ERR_INVALID_FIELD,
                           "gate output extent must be positive, got {0}x{1}".format(
                               output[0], output[1]))
    aperture = _gate_pair(gate, (("aperture_resolution", "aperture_width",
                                 "aperture_height"),), "gate aperture extent")
    if aperture is not None and (aperture[0] <= 0.0 or aperture[1] <= 0.0):
        raise PayloadError(ERR_INVALID_FIELD,
                           "gate aperture extent must be positive, got {0}x{1}".format(
                               aperture[0], aperture[1]))
    gate_pixel_aspect(gate)
    source = gate_field(gate, "resolution_source")
    if source is not None and not isinstance(source, str):
        raise PayloadError(ERR_INVALID_FIELD,
                           "gate.resolution_source must be a string")
    fraction = gate_field(gate, "overscan_resolution_fraction")
    if fraction is not None:
        require_number(fraction, "gate.overscan_resolution_fraction")
    return gate


def _validate_time(time_payload):
    require_mapping(time_payload, "camera_time.time")
    for field in _TIME_NUMBERS:
        if field in time_payload:
            require_number(time_payload[field], "camera_time.time." + field)
    if "display_frame" not in time_payload:
        raise PayloadError(ERR_MISSING_FIELD,
                           "camera_time.time is missing field 'display_frame'")
    require_number(time_payload["display_frame"], "camera_time.time.display_frame")
    for field in _TIME_RATIONALS:
        value = time_payload.get(field)
        if value is not None:
            display_rate_value(value)
    if "playback_range" in time_payload:
        playback_range = require_mapping(time_payload["playback_range"],
                                         "camera_time.time.playback_range")
        for bound in ("start", "end"):
            if bound in playback_range:
                require_number(playback_range[bound],
                               "camera_time.time.playback_range." + bound)
    return time_payload


def validate_camera_time(payload):
    """Validate one v10 ``camera_time`` payload; returns it unchanged when valid.

    Rejects unknown message types, non-finite numbers, wrong shapes and missing
    fields with a stable ``PayloadError.category`` and actionable detail text.
    Unknown extra keys are tolerated, because the Unreal side adds fields as its
    camera evaluation grows. Nothing is applied unless the whole payload
    validates, so a rejected target never leaves a partially updated camera.
    """
    require_mapping(payload, "camera_time")
    message_type = payload.get("type")
    if message_type is not None and message_type != "camera_time":
        raise PayloadError(ERR_INVALID_FIELD,
                           "expected 'camera_time', got {0!r}".format(message_type))
    for field in _CAMERA_TIME_FIELDS:
        if field not in payload:
            raise PayloadError(ERR_MISSING_FIELD,
                               "camera_time payload is missing field {0!r}".format(field))
    for field in _CAMERA_TIME_NUMBERS:
        require_number(payload[field], "camera_time." + field)
    eval_id = payload["eval_id"]
    if not isinstance(eval_id, str) or not eval_id:
        raise PayloadError(ERR_INVALID_FIELD,
                           "camera_time.eval_id must be a non-empty string")
    _validate_time(payload["time"])
    _validate_camera(payload["camera"])
    _validate_gate(payload["gate"])
    cut = payload.get("cut")
    if cut is not None:
        require_mapping(cut, "camera_time.cut")
        for field in ("stage", "name"):
            if field in cut and not isinstance(cut[field], str):
                raise PayloadError(ERR_INVALID_FIELD,
                                   "camera_time.cut.{0} must be a string".format(field))
    return payload


# --------------------------------------------------------------------------
# Normalized application values
# --------------------------------------------------------------------------

def payload_basis(camera):
    """(right, up, forward) Unreal vectors from a camera payload."""
    if camera_field(camera, "right") is not None:
        return tuple(require_vector(camera_field(camera, name), "camera." + name)
                     for name in ("right", "up", "forward"))
    rotation = camera_field(camera, "rotation")
    if isinstance(rotation, dict):
        degrees = tuple(require_number(rotation[axis], "camera.rotation." + axis)
                        for axis in ("roll", "pitch", "yaw"))
    else:
        degrees = require_vector(rotation, "camera.rotation")
    return ue_basis_vectors(degrees)


def payload_location(camera):
    """The Unreal world location of a camera payload, as a 3-tuple."""
    return require_vector(camera_field(camera, "location"), "camera.location")


def resolve_far_clip(camera):
    """Maya far clip for one camera payload.

    Returns ``(centimetres, substituted, source)``. Unreal has no far clip
    plane, so a finite positive ``far_clip_cm`` is used directly and anything
    else falls back to the documented module default; the substitution is
    always reported so the caller can tell a real value from a stand-in.
    """
    value = camera_field(camera, "far_clip_cm")
    if value is not None:
        centimetres = require_number(value, "camera.far_clip_cm")
        if centimetres > 0.0:
            return (centimetres, False, "camera")
    return (DEFAULT_FAR_CLIP_CM, True, "module_default")


def camera_attributes(camera, gate):
    """Normalized Maya values for one camera payload and its resolution gate.

    Returns ``(attributes, notes)`` where ``attributes`` maps Maya camera
    attribute names to the values to write and ``notes`` records what the
    mapping adapted (film fit justification, far clip substitution, and the
    ``intended`` values :meth:`LinkCamera.readback` reports, so a caller can
    compare what Maya holds with what Unreal asked for).
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
    horizontal = camera_field(camera, "sensor_horizontal_offset_mm")
    vertical = camera_field(camera, "sensor_vertical_offset_mm")
    offsets = film_offset_inches(
        0.0 if horizontal is None else require_number(
            horizontal, "camera.sensor_horizontal_offset_mm"),
        0.0 if vertical is None else require_number(
            vertical, "camera.sensor_vertical_offset_mm"))
    squeeze = camera_field(camera, "squeeze_factor")
    squeeze = 1.0 if squeeze is None else require_number(squeeze, "camera.squeeze_factor")
    if squeeze <= 0.0:
        raise PayloadError(ERR_INVALID_FIELD, "squeeze_factor must be positive")

    sensor_aspect = (require_number(camera_field(camera, "sensor_aspect_ratio"),
                                    "camera.sensor_aspect_ratio")
                     if camera_field(camera, "sensor_aspect_ratio") is not None
                     else sensor_width_mm / sensor_height_mm)
    output = gate_device_pixels(gate)
    output_aspect = (require_number(camera_field(camera, "rendered_aspect_ratio"),
                                    "camera.rendered_aspect_ratio")
                     if camera_field(camera, "rendered_aspect_ratio") is not None
                     else output[0] / output[1])
    constrain = bool(camera_field(camera, "constrain_aspect_ratio"))
    constraint = camera_field(camera, "aspect_axis_constraint")
    fit = film_fit(constraint, constrain, sensor_aspect, output_aspect)

    far_clip, far_clip_substituted, far_clip_source = resolve_far_clip(camera)

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
    depth_of_field = _depth_of_field(camera)
    if depth_of_field is not None:
        attributes["depthOfField"] = depth_of_field

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
        "framing_pixels": gate_framing_pixels(gate),
        "gate_device_pixels": output,
        "intended": {
            "focal_length_mm": focal_length_mm,
            "horizontal_film_aperture_in": aperture[0],
            "vertical_film_aperture_in": aperture[1],
            "horizontal_film_offset_in": offsets[0],
            "vertical_film_offset_in": offsets[1],
            "film_fit": fit,
            "lens_squeeze_ratio": squeeze,
            "f_stop": attributes.get("fStop"),
            "focus_distance_cm": attributes.get("focusDistance"),
            "depth_of_field": attributes.get("depthOfField"),
            "near_clip_cm": 1.0 if near_clip is None else require_number(
                near_clip, "camera.near_clip_cm"),
            "far_clip_cm": far_clip,
            "far_clip_substituted": far_clip_substituted,
            "far_clip_source": far_clip_source,
        },
    }
    return (attributes, notes)


def _depth_of_field(camera):
    """Maya's depth-of-field flag for one camera payload, or ``None``.

    An explicit ``depth_of_field`` wins; otherwise Unreal's ``focus_method``
    decides, because ``Disable`` means the camera renders without depth of
    field even though it carries an aperture and a focus distance.
    """
    explicit = camera_field(camera, "depth_of_field")
    if explicit is not None:
        return bool(explicit)
    method = camera_field(camera, "focus_method")
    if method is None:
        return None
    return method.strip().lower() not in _DISABLED_FOCUS_METHODS


# --------------------------------------------------------------------------
# Maya host adapter
# --------------------------------------------------------------------------

def require_writable(plug):
    """Refuse a protected plug, including compound locks, without unlocking it.

    Checks every ancestor of the attribute (``translateX`` is written through
    ``translate``) and the node itself, then the plug's own driver, so a locked
    or driven attribute is refused before anything is written rather than
    silently ignored by ``setAttr``.
    """
    node, attribute = plug.rsplit(".", 1)
    while attribute:
        if cmds.getAttr(node + "." + attribute, lock=True):
            raise CameraSyncRefused(
                CATEGORY_LOCKED_ATTRIBUTE,
                "{0} is locked; the link camera never unlocks scene attributes "
                "and writes no target of a locked plug".format(plug))
        parents = cmds.attributeQuery(attribute, node=node, listParent=True) or []
        attribute = parents[0] if parents else None
    drivers = cmds.listConnections(plug, source=True, destination=False) or []
    if drivers:
        raise CameraSyncRefused(
            CATEGORY_CAMERA_INPUT_DRIVEN,
            "{0} has an input driver {1}; the link camera only writes static "
            "values, so remove the connection or use a different camera "
            "name".format(plug, drivers[0]))
    if cmds.lockNode(node, query=True, lock=True)[0]:
        raise CameraSyncRefused(
            CATEGORY_LOCKED_ATTRIBUTE,
            "{0} is locked; the link camera never unlocks scene nodes".format(node))


def require_scene_unit():
    """Refuse a scene that is not in centimetres, instead of rescaling."""
    unit = str(cmds.currentUnit(query=True, linear=True))
    if unit.strip().lower() != "cm":
        raise CameraSyncRefused(
            CATEGORY_UNSUPPORTED_SCENE_UNIT,
            "scene linear unit is {0!r}; Unreal sends centimetres and the link "
            "camera does not rescale - set Preferences > Settings > Linear to "
            "centimetres".format(unit))
    return unit


def require_maya():
    """Refuse a host call when this module runs outside Maya."""
    if cmds is None or om is None:
        raise RuntimeError("MtoU_LinkLink camera must run inside Maya")


def model_panels():
    """Every model panel Maya currently has; empty in a batch session."""
    panels = []
    for panel_type in ("modelPanel", "modelEditor"):
        for panel in (cmds.getPanel(type=panel_type) or []):
            if panel not in panels:
                panels.append(panel)
    return panels


def _panel_command(panel):
    return cmds.modelPanel if cmds.nodeType(panel) == "modelPanel" else cmds.modelEditor


def panel_camera(panel):
    """The look-through camera a model panel currently shows."""
    return _panel_command(panel)(panel, query=True, camera=True)


def set_panel_camera(panel, camera):
    """Make a model panel look through ``camera``."""
    _panel_command(panel)(panel, edit=True, camera=camera)


def _world_matrix(transform):
    """The world matrix of a transform, read through ``om`` so tests patch it."""
    selection = om.MSelectionList()
    selection.add(transform)
    return [float(value) for value in selection.getDagPath(0).inclusiveMatrix()]


def _optional_number(plug):
    """A possibly absent numeric attribute: a camera without an fStop."""
    if not cmds.objExists(plug):
        return None
    if hasattr(cmds.getAttr(plug), "__len__"):
        return None
    value = cmds.getAttr(plug)
    if value is None:
        return None
    return float(value)


def _optional_bool(plug):
    if not cmds.objExists(plug):
        return None
    value = cmds.getAttr(plug)
    if value is None:
        return None
    return bool(value)


def _read_gate():
    """The Maya resolution gate as it stands right now."""
    return {
        "width": int(cmds.getAttr("defaultResolution.width")),
        "height": int(cmds.getAttr("defaultResolution.height")),
        "pixel_aspect_ratio": float(cmds.getAttr("defaultResolution.pixelAspect")),
        "device_aspect_ratio": float(
            cmds.getAttr("defaultResolution.deviceAspectRatio")),
    }


def _validate_camera_inputs(transform, shape):
    """Borrow only static cameras and static parents; never sever a driver."""
    node = shape
    while node:
        drivers = cmds.listConnections(
            node, source=True, destination=False, plugs=True, connections=True) or []
        for index in range(0, len(drivers), 2):
            if cmds.getAttr(drivers[index + 1], type=True) != "message":
                raise CameraSyncRefused(
                    CATEGORY_CAMERA_INPUT_DRIVEN,
                    "{0} is driven by {1}; the link camera borrows only static "
                    "cameras - use a different camera name".format(
                        drivers[index], drivers[index + 1]))
        parents = cmds.listRelatives(node, parent=True, fullPath=True) or []
        node = parents[0] if parents else None
    for attribute in TRANSFORM_WRITE_ATTRIBUTES:
        require_writable(transform + "." + attribute)
    for attribute in MANAGED_ATTRIBUTES:
        require_writable(shape + "." + attribute)


def _closed(actual, wanted, tolerance=1e-6):
    """Numeric comparison for values that survive a float round trip."""
    if isinstance(actual, bool) or isinstance(wanted, bool):
        return bool(actual) == bool(wanted)
    if actual is None or wanted is None:
        return actual == wanted
    return abs(float(actual) - float(wanted)) <= abs(tolerance)


def _resolution_value(plug):
    if plug.endswith("width") or plug.endswith("height"):
        return int(round(float(cmds.getAttr(plug))))
    return float(cmds.getAttr(plug))


def _camera_shapes(name):
    shapes = cmds.listRelatives(name, shapes=True, type="camera") or []
    return [shape for shape in shapes]


class LinkCamera(object):
    """Applies one Unreal ``camera_time`` target to Maya and restores the scene.

    The class is import-safe outside Maya: ``cmds``/``om`` are ``None`` until
    Maya is imported and every host call goes through them, so tests patch the
    module-level names. One instance owns one tool-owned camera, created on
    demand and named :data:`LINK_CAMERA_NAME`, and one resolution gate.

    ``apply(camera_payload, gate_payload)`` validates the payloads, refuses
    anything it cannot write, then writes the mapped camera attributes, the
    world matrix and the resolution gate inside one undo chunk and reads the
    values Maya holds back. ``capture_state()`` and ``restore_state(state)``
    capture and restore everything the adapter may change: the look-through
    camera of each model panel, the resolution-gate attributes, the undo state
    and the borrowed camera, deleting only the camera this adapter created.
    """

    def __init__(self, name=None):
        self.name = LINK_CAMERA_NAME if name is None else name
        self._transform = None
        self._shape = None
        self._created = False
        self._reused = False
        self._state = None
        self.last_report = None
        self.last_notes = None

    # ------------------------------------------------------------------ apply

    def apply(self, camera_payload, gate_payload):
        """Apply one camera/gate payload to the scene and report what Maya holds.

        Returns ``(report, values)``: ``report`` describes what was written,
        ``values`` is the host read-back compared against
        ``notes["intended"]``. Every refusal happens before any Maya call that
        changes the scene.
        """
        camera = _validate_camera(camera_payload)
        gate = _validate_gate(gate_payload)
        require_maya()
        require_scene_unit()

        matrix = ue_basis_to_maya_matrix(*(payload_basis(camera)
                                          + (payload_location(camera),)))
        attributes, notes = camera_attributes(camera, gate)
        # Maya has a single gate, while Unreal renders the film aperture inside
        # the output resolution and leaves bars where the two aspects differ.
        # The gate therefore follows the aperture pixels the publisher reports,
        # which is what makes Maya's derived axis match Unreal's rendered
        # extent.
        pixels = gate_framing_pixels(gate)
        pixel_aspect = gate_pixel_aspect(gate)
        panels = model_panels()

        for panel in panels:
            require_writable(panel + ".camera")
        for plug in RESOLUTION_PLUGS:
            require_writable(plug)

        # The snapshot is taken before the camera exists, so it records the
        # scene as it looked before this adapter touched anything.
        if self._state is None:
            self._state = self.capture_state()

        transform, shape, borrowed = self._ensure_camera()
        created_now = self._created and not self._reused
        self._reused = True
        for attribute in TRANSFORM_WRITE_ATTRIBUTES:
            require_writable(transform + "." + attribute)
        for attribute in MANAGED_ATTRIBUTES:
            require_writable(shape + "." + attribute)
        for panel in panels:
            require_writable(panel + ".camera")

        report = {
            "camera": transform,
            "shape": shape,
            "created": created_now,
            "attributes": dict(attributes),
            "notes": notes,
            "panels": panels,
            "gate": {"output_width": pixels[0], "output_height": pixels[1],
                     "pixel_aspect_ratio": pixel_aspect},
        }
        self.last_notes = notes
        self.last_report = report

        undo_open = bool(cmds.undoInfo(query=True, state=True))
        if not undo_open:
            cmds.undoInfo(stateWithoutFlush=True)
        time_before = float(cmds.currentTime(query=True))
        cmds.undoInfo(openChunk=True)
        try:
            cmds.xform(transform, worldSpace=True, matrix=list(matrix))
            for attribute, value in sorted(attributes.items()):
                cmds.setAttr("{0}.{1}".format(shape, attribute), value)
            self._apply_resolution_gate(pixels, pixel_aspect)
            for panel in panels:
                set_panel_camera(panel, transform)
            for plug in (shape + ".displayResolution", shape + ".displayGateMask"):
                cmds.setAttr(plug, True)
            values = self.readback()
            self._verify(values, attributes, notes, matrix)
        except BaseException:
            cmds.undoInfo(closeChunk=True)
            try:
                cmds.undo()
            except RuntimeError as undo_error:
                report["undo_error"] = str(undo_error)
            try:
                cmds.currentTime(time_before, edit=True)
            except RuntimeError:
                pass
            raise
        else:
            cmds.undoInfo(closeChunk=True)
        finally:
            if not undo_open:
                try:
                    cmds.undoInfo(stateWithoutFlush=False)
                except RuntimeError:
                    pass
        return (report, values)

    def _ensure_camera(self):
        """The tool-owned camera transform and shape, created on demand.

        A camera that already carries the tool name is borrowed after its
        inputs are validated, so the adapter never deletes a node it did not
        create; a foreign node under the same name is refused.
        """
        if self._transform and cmds.objExists(self._transform):
            shapes = _camera_shapes(self._transform)
            if shapes:
                return (self._transform, shapes[0], self._created)
        if cmds.objExists(self.name):
            shapes = _camera_shapes(self.name)
            if not shapes:
                raise CameraSyncRefused(
                    CATEGORY_CAMERA_NAME_CONFLICT,
                    "{0!r} exists and is not a camera; the link camera refuses "
                    "to reuse a foreign node - pick another name".format(self.name))
            self._transform = self.name
            self._shape = shapes[0]
            _validate_camera_inputs(self._transform, self._shape)
            return (self._transform, self._shape, self._created)
        transform = cmds.createNode("transform", name=self.name)
        shape = cmds.createNode("camera", name=self.name + "Shape", parent=transform)
        self._transform = transform
        self._shape = shape
        self._created = True
        return (transform, shape, True)

    def _apply_resolution_gate(self, output, pixel_aspect):
        """Set the rendered pixel extent and the device aspect Maya derives.

        Maya's film fit reads ``defaultResolution.deviceAspectRatio``, which
        does not follow ``width``/``height`` when they are set through the API,
        so it is written as well; otherwise a non-16:9 gate would keep framing
        with the previous aspect. ``pixelAspect`` carries the gate's own pixel
        aspect so a non-square-pixel gate frames exactly as Unreal renders it.
        """
        cmds.setAttr("defaultResolution.width", int(round(float(output[0]))))
        cmds.setAttr("defaultResolution.height", int(round(float(output[1]))))
        cmds.setAttr("defaultResolution.pixelAspect", float(pixel_aspect))
        cmds.setAttr("defaultResolution.deviceAspectRatio",
                     float(output[0]) * float(pixel_aspect) / float(output[1]))

    # --------------------------------------------------------------- readback

    def readback(self):
        """The values Maya actually holds after the last applied target.

        Reports the world matrix through ``om``, every managed camera
        attribute through ``cmds.getAttr`` and the resolution gate, so a
        caller can compare them against :func:`camera_attributes`' ``intended``
        notes. Raises when no camera has been applied yet.
        """
        require_maya()
        if not (self._transform and cmds.objExists(self._transform)):
            raise CameraSyncRefused(
                CATEGORY_NO_CAMERA_YET,
                "no MtoU_LinkCamera camera has been applied; call apply() first")
        shape = self._shape
        values = {
            "camera": self._transform,
            "shape": shape,
            "maya_time": float(cmds.currentTime(query=True)),
            "world_matrix": _world_matrix(self._transform),
            "focal_length_mm": float(cmds.getAttr(shape + ".focalLength")),
            "horizontal_film_aperture_in": float(
                cmds.getAttr(shape + ".horizontalFilmAperture")),
            "vertical_film_aperture_in": float(
                cmds.getAttr(shape + ".verticalFilmAperture")),
            "horizontal_film_offset_in": float(
                cmds.getAttr(shape + ".horizontalFilmOffset")),
            "vertical_film_offset_in": float(
                cmds.getAttr(shape + ".verticalFilmOffset")),
            "film_fit": int(cmds.getAttr(shape + ".filmFit")),
            "lens_squeeze_ratio": float(cmds.getAttr(shape + ".lensSqueezeRatio")),
            "f_stop": _optional_number(shape + ".fStop"),
            "focus_distance_cm": _optional_number(shape + ".focusDistance"),
            "depth_of_field": _optional_bool(shape + ".depthOfField"),
            "near_clip_cm": float(cmds.getAttr(shape + ".nearClipPlane")),
            "far_clip_cm": float(cmds.getAttr(shape + ".farClipPlane")),
        }
        notes = self.last_notes or {}
        intended = notes.get("intended", {})
        values["far_clip_substituted"] = intended.get("far_clip_substituted")
        values["far_clip_source"] = intended.get("far_clip_source")
        values["film_fit_name"] = FILM_FIT_NAMES.get(values["film_fit"],
                                                     str(values["film_fit"]))
        values["gate"] = _read_gate()
        return values

    # ------------------------------------------------------------ capture/restore

    def capture_state(self):
        """An opaque snapshot of everything this adapter may change.

        Records the look-through camera of each model panel it will touch, the
        resolution-gate attributes, the undo state, and whether a tool camera
        already exists together with its borrowed state. Called once when
        linkage starts; the result is passed back to :meth:`restore_state`.
        """
        require_maya()
        state = {
            "name": self.name,
            "panels": dict((panel, panel_camera(panel)) for panel in model_panels()),
            "resolution": dict((plug, _resolution_value(plug))
                               for plug in RESOLUTION_PLUGS),
            "undo": bool(cmds.undoInfo(query=True, state=True)),
            "camera": None,
        }
        if cmds.objExists(self.name):
            state["camera"] = self._borrow_state()
        return state

    def _borrow_state(self):
        """Remember a camera that already carries the tool name.

        The adapter only ever deletes a camera it created, so a node that
        already carries the name is reused and gets its managed attributes and
        placement written back, leaving a borrowed node exactly as it was.
        Ownership is deliberately left untouched: the caller may hold a camera
        this adapter created, and clearing ``_created`` here would strand that
        node in the scene when the adapter later restores.
        """
        shapes = _camera_shapes(self.name) if cmds.objExists(self.name) else []
        if not shapes:
            return None
        self._transform = self._transform or self.name
        self._shape = shapes[0]
        state = {"transform": self._transform, "shape": shapes[0],
                 "attributes": dict((attribute, cmds.getAttr(
                     "{0}.{1}".format(shapes[0], attribute)))
                     for attribute in MANAGED_ATTRIBUTES),
                 "world_matrix": _world_matrix(self._transform)}
        return state

    def restore_state(self, state=None):
        """Restore a captured state idempotently; returns the residual notes.

        ``state`` may be ``None`` (nothing captured) or a snapshot from
        :meth:`capture_state`. Only the camera this adapter created is
        deleted; a borrowed camera is written back. A resource that cannot be
        restored is reported in the returned ``residual`` list rather than
        raising, so one unrecoverable plug never strands the others.
        """
        require_maya()
        if state is None:
            state, self._state = self._state, None
        elif state is not self._state:
            self._state = state
        residual = []
        name = self.name if state is None else state.get("name", self.name)

        for panel, camera in (state or {}).get("panels", {}).items():
            residual.extend(self._restore_panel(panel, camera))

        for plug, value in sorted((state or {}).get("resolution", {}).items()):
            try:
                require_writable(plug)
                cmds.setAttr(plug, value)
            except (RuntimeError, ValueError, CameraSyncRefused) as error:
                residual.append("{0}: {1}".format(plug, error))

        residual.extend(self._restore_camera(state))

        undo_state = (state or {}).get("undo")
        if undo_state is False:
            try:
                cmds.undoInfo(stateWithoutFlush=False)
            except RuntimeError as error:
                residual.append("undo state: {0}".format(error))

        if self._created and cmds.objExists(name):
            try:
                cmds.delete(name)
            except RuntimeError as error:
                residual.append("camera {0}: {1}".format(name, error))
            else:
                self._created = False
                self._transform = None
                self._shape = None
        return residual

    def _restore_panel(self, panel, camera):
        """Put one model panel's look-through camera back, or report why not."""
        if not camera or not cmds.objExists(panel):
            return []
        try:
            require_writable(panel + ".camera")
            set_panel_camera(panel, camera)
        except (RuntimeError, ValueError, CameraSyncRefused) as error:
            return ["{0}: {1}".format(panel, error)]
        return []

    def _restore_camera(self, state):
        """Write a borrowed camera back, or delete the one this adapter created."""
        borrowed = (state or {}).get("camera")
        if not borrowed:
            return []
        shape = borrowed.get("shape")
        transform = borrowed.get("transform")
        if not shape or not cmds.objExists(shape):
            return []
        residual = []
        try:
            for attribute in TRANSFORM_WRITE_ATTRIBUTES:
                require_writable(transform + "." + attribute)
            for attribute, value in sorted(borrowed.get("attributes", {}).items()):
                cmds.setAttr("{0}.{1}".format(shape, attribute), value)
            cmds.xform(transform, worldSpace=True,
                       matrix=list(borrowed["world_matrix"]))
        except (RuntimeError, ValueError, CameraSyncRefused) as error:
            residual.append("camera {0}: {1}".format(shape, error))
        return residual

    # ------------------------------------------------------------------- time

    def applied_time(self, maya_time):
        """The frame Maya is actually moved to; fractional frames are kept."""
        return applied_maya_time(require_number(maya_time, "maya_time"))

    def set_time(self, maya_time):
        """Move Maya's current time to ``maya_time`` and report the host value.

        Fractional frames are set directly and never rounded. No keyframe is
        created and the Playback Range is untouched.
        """
        require_maya()
        frame = self.applied_time(maya_time)
        cmds.currentTime(frame, edit=True)
        applied = float(cmds.currentTime(query=True))
        if not _closed(applied, frame):
            raise CameraSyncRefused(
                CATEGORY_HOST_STATE_MISMATCH,
                "Maya reports current time {0} after setting {1}".format(applied, frame))
        return applied

    # ------------------------------------------------------------ verification

    #: Written attribute or note key -> the read-back key that reports it.
    _VERIFIED_KEYS = (
        ("focalLength", "focal_length_mm"),
        ("horizontalFilmAperture", "horizontal_film_aperture_in"),
        ("verticalFilmAperture", "vertical_film_aperture_in"),
        ("horizontalFilmOffset", "horizontal_film_offset_in"),
        ("verticalFilmOffset", "vertical_film_offset_in"),
        ("filmFit", "film_fit"),
        ("lensSqueezeRatio", "lens_squeeze_ratio"),
        ("fStop", "f_stop"),
        ("focusDistance", "focus_distance_cm"),
        ("depthOfField", "depth_of_field"),
        ("nearClipPlane", "near_clip_cm"),
        ("farClipPlane", "far_clip_cm"),
    )

    def _verify(self, values, attributes, notes, matrix):
        """Compare the read-back values with the intended ones.

        Maya's own read-back is the evidence Unreal receives, so a mismatch is
        a hard failure: the caller must not claim a target was applied. The
        camera identifies itself by name, so a foreign node cannot stand in.
        """
        differences = []
        for written, reported in self._VERIFIED_KEYS:
            wanted = (attributes[written] if written in attributes
                      else notes["intended"].get(reported))
            if wanted is None:
                continue
            actual = values.get(reported)
            if actual is None:
                differences.append("{0} is missing".format(reported))
            elif not _closed(actual, wanted):
                differences.append("{0}={1!r} != {2!r}".format(reported, actual, wanted))
        if values.get("camera") != self._transform:
            differences.append("camera={0!r}".format(values.get("camera")))
        if values.get("shape") != self._shape:
            differences.append("shape={0!r}".format(values.get("shape")))
        if values.get("far_clip_source") != notes["intended"]["far_clip_source"]:
            differences.append("far_clip_source={0!r} != {1!r}".format(
                values.get("far_clip_source"), notes["intended"]["far_clip_source"]))
        for actual, wanted in zip(values["world_matrix"], matrix):
            if not _closed(actual, wanted, 1e-6):
                differences.append("world_matrix")
                break
        if differences:
            raise CameraSyncRefused(
                CATEGORY_HOST_STATE_MISMATCH,
                "Maya read-back differs from the target: " + ", ".join(differences))


