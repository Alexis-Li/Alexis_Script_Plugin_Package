"""Rendered framing check for the synced camera.

Opt-in evidence for the marker comparison: the synced camera is rendered with
Arnold at the declared output resolution, each marker is an emissive sphere on a
black background, and the centroid of every sphere in the rendered image is
compared with (a) Maya's own film-aperture projection for that camera and
(b) the Unreal NDC the payload carried.

Markers are white spheres with a pure emission shading group, so the image does
not depend on any light. The image is rendered as Targa and decoded with the
standard library (``MImage.pixels()`` returns a raw pointer in this Maya build,
and no third party decoder is installed). A marker whose expected centre is
outside the rendered frame is reported instead of being matched to a wrong blob.

Everything this module creates is deleted again; only the rendered images stay
behind as evidence.
"""

import math
import sys
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

import mtou_camera_sync_mapping as mapping  # noqa: E402

TOLERANCE_PIXELS = 2.0
BRIGHTNESS_FRACTION = 0.5
CAMERA_NAME = "MtoUCameraSyncRenderCheck"


def _cmds():
    import maya.cmds
    return maya.cmds


def arnold_available():
    """``(available, detail)`` for the Arnold plug-in in this Maya session."""
    cmds = _cmds()
    try:
        cmds.loadPlugin("mtoa")
    except Exception as error:
        return (False, "Arnold (mtoa) is not loadable in this Mayapy session: "
                       "{0}: {1}".format(type(error).__name__, error))
    try:
        version = cmds.pluginInfo("mtoa", query=True, version=True)
    except Exception as error:
        version = "unknown ({0})".format(error)
    return (True, "Arnold (mtoa) {0}".format(version))


def enum_pairs(list_enum):
    """``[(name, value)]`` from Maya's ``listEnum`` string.

    Entries may carry an explicit ``name=value`` (``PNG`` is 32, not its
    position, because earlier entries jump values).
    """
    pairs = []
    value = 0
    for entry in list_enum.split(":"):
        entry = entry.strip()
        name = entry
        if "=" in entry:
            name, _, explicit = entry.partition("=")
            name = name.strip()
            try:
                value = int(explicit.strip())
            except ValueError:
                pass
        pairs.append((name, value))
        value += 1
    return pairs


def image_format_for(cmds, wanted=("targa", "tiff", "iff")):
    """``(enum value, extension)`` for a decodable image format, or ``(None, None)``."""
    try:
        enum = cmds.attributeQuery("imageFormat", node="defaultRenderGlobals",
                                   listEnum=True)
    except Exception:
        return (None, None)
    pairs = enum_pairs(enum[0]) if enum else []
    lowered = {}
    for name, value in pairs:
        lowered[name.split("(")[0].strip().lower()] = value
    extension = {"targa": "tga", "tiff": "tif", "iff": "iff"}
    for name in wanted:
        if name in lowered:
            return (lowered[name], extension[name])
    return (None, None)


def configuration_from_payload(name, camera_payload, markers, resolution, session,
                               frame=None, compare_unreal_ndc=True):
    """Render configuration matching one wire frame payload.

    ``compare_unreal_ndc`` is false for variant configurations that change the
    aspect constraint or resolution: the payload's marker NDC then describes a
    different projection and is only meaningful for the payload it came from.
    """
    right, up, forward = mapping.payload_basis(camera_payload)
    location = mapping.payload_location(camera_payload)
    matrix = mapping.ue_basis_to_maya_matrix(right, up, forward, location)
    attributes, notes = mapping.camera_parameters(camera_payload, session,
                                                  resolution, frame)
    return {
        "name": name,
        "camera_matrix": matrix,
        "focal_length_mm": attributes["focalLength"],
        "film_aperture_inches": (attributes["horizontalFilmAperture"],
                                 attributes["verticalFilmAperture"]),
        "film_offsets_inches": (attributes["horizontalFilmOffset"],
                                attributes["verticalFilmOffset"]),
        "film_fit": attributes["filmFit"],
        "lens_squeeze_ratio": attributes["lensSqueezeRatio"],
        "resolution": resolution,
        "compare_unreal_ndc": compare_unreal_ndc,
        "markers": [
            {"name": marker["name"],
             "point_maya": mapping.ue_to_maya_point(marker["location"]["x"],
                                                    marker["location"]["y"],
                                                    marker["location"]["z"]),
             "expected_ndc": (marker["ndc"]["x"], marker["ndc"]["y"])
             if compare_unreal_ndc and marker.get("ndc") else None}
            for marker in markers
        ],
    }


def _make_camera(cmds, config):
    transform = cmds.createNode("transform", name=CAMERA_NAME + "_xform")
    shape = cmds.createNode("camera", name=CAMERA_NAME + "_shape", parent=transform)
    cmds.xform(transform, worldSpace=True, matrix=list(config["camera_matrix"]))
    cmds.setAttr(shape + ".focalLength", config["focal_length_mm"])
    cmds.setAttr(shape + ".horizontalFilmAperture", config["film_aperture_inches"][0])
    cmds.setAttr(shape + ".verticalFilmAperture", config["film_aperture_inches"][1])
    cmds.setAttr(shape + ".horizontalFilmOffset", config["film_offsets_inches"][0])
    cmds.setAttr(shape + ".verticalFilmOffset", config["film_offsets_inches"][1])
    cmds.setAttr(shape + ".filmFit", config["film_fit"])
    cmds.setAttr(shape + ".lensSqueezeRatio", config["lens_squeeze_ratio"])
    cmds.setAttr(shape + ".nearClipPlane", 1.0)
    cmds.setAttr(shape + ".farClipPlane", 1000000.0)
    cmds.setAttr(shape + ".depthOfField", False)
    return (transform, shape)


def _make_markers(cmds, config, shape):
    """Emissive marker spheres sized to a few pixels at their own depth."""
    group = cmds.createNode("transform", name=CAMERA_NAME + "_markers")
    shader = cmds.createNode("aiStandardSurface", name=CAMERA_NAME + "_emission")
    cmds.setAttr(shader + ".base", 0.0)
    cmds.setAttr(shader + ".emission", 1.0)
    cmds.setAttr(shader + ".emissionColor", 1.0, 1.0, 1.0, type="double3")
    shading_group = cmds.sets(renderable=True, noSurfaceShader=True, empty=True,
                              name=CAMERA_NAME + "_sg")
    cmds.connectAttr(shader + ".outColor", shading_group + ".surfaceShader")
    resolution = config["resolution"]
    extent_x, extent_y = mapping.film_gate_extent_inches(
        config["film_aperture_inches"], config["film_fit"], resolution,
        config["lens_squeeze_ratio"])
    focal_inches = config["focal_length_mm"] / mapping.MM_PER_INCH
    spheres = []
    for marker in config["markers"]:
        local = mapping.camera_space_point(marker["point_maya"], config["camera_matrix"])
        depth = abs(local[2])
        half_height_cm = depth * (extent_y / 2.0) / focal_inches
        radius = max(half_height_cm * (5.0 / (resolution[1] / 2.0)), 0.05)
        sphere = cmds.polySphere(radius=radius, name=CAMERA_NAME + "_" + marker["name"])[0]
        cmds.xform(sphere, worldSpace=True, translation=list(marker["point_maya"]))
        cmds.parent(sphere, group)
        cmds.sets(sphere, edit=True, forceElement=shading_group)
        marker["_sphere"] = sphere
        marker["_radius_cm"] = radius
        spheres.append(sphere)
    return (group, spheres, shader, shading_group)


def _set_render_settings(cmds, resolution, prefix, image_format):
    cmds.setAttr("defaultRenderGlobals.currentRenderer", "arnold", type="string")
    cmds.setAttr("defaultRenderGlobals.imageFormat", image_format[0])
    # Maya composes the output path from the project's images directory, so the
    # prefix is a plain name and the result is located afterwards.
    cmds.setAttr("defaultRenderGlobals.imageFilePrefix", prefix, type="string")
    cmds.setAttr("defaultRenderGlobals.animation", 0)
    cmds.setAttr("defaultRenderGlobals.outFormatControl", 0)
    cmds.setAttr("defaultRenderGlobals.putFrameBeforeExt", 0)
    for plug, value in (("defaultRenderGlobals.enableDefaultLight", False),):
        try:
            cmds.setAttr(plug, value)
        except RuntimeError:
            pass
    for plug, value in (("defaultResolution.width", int(resolution[0])),
                        ("defaultResolution.height", int(resolution[1])),
                        ("defaultResolution.pixelAspect", 1.0),
                        ("defaultResolution.deviceAspectRatio",
                         float(resolution[0]) / float(resolution[1]))):
        try:
            cmds.setAttr(plug, lock=False)
        except RuntimeError:
            pass
        cmds.setAttr(plug, value)


def _find_image(cmds, prefix, extension, output_dir, config_name):
    """Locate the rendered file and copy it into the evidence directory."""
    searched = []
    candidates = []

    def collect(root):
        if not root:
            return
        try:
            pattern = str(Path(root) / ("**/" + prefix + "*." + extension))
            candidates.extend(sorted(Path(root).glob("**/" + prefix + "*." + extension)))
            searched.append(pattern)
        except OSError:
            pass

    collect(output_dir)
    try:
        collect(Path(cmds.workspace(expandName="images")))
    except Exception:
        pass
    try:
        collect(Path(cmds.workspace(query=True, rootDirectory=True)) / "images")
    except Exception:
        pass
    existing = [path for path in candidates if path.is_file()]
    if not existing:
        return (None, searched)
    existing.sort(key=lambda path: path.stat().st_mtime)
    source = existing[-1]
    target = Path(output_dir) / (config_name + "." + extension)
    try:
        import shutil
        shutil.copy2(str(source), str(target))
        return (target, searched + [str(path) for path in existing])
    except OSError:
        return (source, searched + [str(path) for path in existing])


def _read_pixels(path):
    """``(width, height, luminance)`` from the rendered file.

    Maya's own ``MImage.pixels()`` returns a raw pointer in this build rather
    than a Python sequence, so the rendered Targa file is decoded here: TGA
    needs only the standard library and no filters or compression tables.
    """
    data = Path(path).read_bytes()
    if len(data) < 18:
        raise ValueError("rendered file {0} is too short to be a TGA".format(path))
    id_length = data[0]
    color_map = data[1]
    image_type = data[2]
    width = data[12] | (data[13] << 8)
    height = data[14] | (data[15] << 8)
    depth = data[16]
    descriptor = data[17]
    if color_map != 0:
        raise ValueError("TGA with a colour map is not supported")
    if image_type not in (2, 10):
        raise ValueError("unsupported TGA image type {0}".format(image_type))
    if depth not in (24, 32):
        raise ValueError("unsupported TGA pixel depth {0}".format(depth))
    step = depth // 8
    offset = 18 + id_length
    count = width * height
    raw = bytearray(count * step)
    if image_type == 2:
        raw[:] = data[offset:offset + count * step]
    else:
        produced = 0
        while produced < count:
            header = data[offset]
            offset += 1
            length = (header & 0x7F) + 1
            if header & 0x80:
                pixel = data[offset:offset + step]
                offset += step
                for _ in range(length):
                    raw[produced * step:(produced + 1) * step] = pixel
                    produced += 1
            else:
                span = length * step
                raw[produced * step:(produced + length) * step] = data[offset:offset + span]
                offset += span
                produced += length
    luminance = [0.0] * count
    for index in range(count):
        base = index * step
        blue = raw[base]
        green = raw[base + 1]
        red = raw[base + 2]
        luminance[index] = max(red, green, blue) / 255.0
    if not descriptor & 0x20:
        # TGA stores rows bottom-up unless the descriptor says otherwise.
        flipped = [0.0] * count
        for row in range(height):
            source = (height - 1 - row) * width
            flipped[row * width:(row + 1) * width] = luminance[source:source + width]
        luminance = flipped
    return (width, height, luminance)


def _components(width, height, luminance, threshold):
    """Connected bright regions as ``(size, centroid_x, centroid_y)``, row 0 top."""
    bright = set()
    for index, value in enumerate(luminance):
        if value >= threshold:
            bright.add(index)
    components = []
    while bright:
        stack = [bright.pop()]
        total_x = 0.0
        total_y = 0.0
        size = 0
        while stack:
            current = stack.pop()
            row, column = divmod(current, width)
            total_x += column
            total_y += row
            size += 1
            neighbours = []
            if column > 0:
                neighbours.append(current - 1)
            if column < width - 1:
                neighbours.append(current + 1)
            if row > 0:
                neighbours.append(current - width)
            if row < height - 1:
                neighbours.append(current + width)
            for neighbour in neighbours:
                if neighbour in bright:
                    bright.discard(neighbour)
                    stack.append(neighbour)
        if size >= 2:
            components.append((size, total_x / size, total_y / size))
    return components


def _pixel_to_ndc(pixel, resolution):
    return (2.0 * (pixel[0] + 0.5) / resolution[0] - 1.0,
            1.0 - 2.0 * (pixel[1] + 0.5) / resolution[1])


def check_configuration(cmds, config, output_dir, image_format):
    """Render one configuration and compare marker centroids with the maths."""
    resolution = config["resolution"]
    prefix = config["name"] + "_frame"
    _set_render_settings(cmds, resolution, prefix, image_format)
    transform, shape = _make_camera(cmds, config)
    group, spheres, shader, shading_group = _make_markers(cmds, config, shape)
    scene_objects = [group, transform, shader, shading_group] + spheres
    entry = {"name": config["name"], "resolution": [resolution[0], resolution[1]],
             "film_fit": config["film_fit"], "markers": [], "errors": []}
    try:
        cmds.render(shape)
        image_path, searched = _find_image(cmds, prefix, image_format[1],
                                          output_dir, config["name"])
        entry["searched"] = searched
        if image_path is None:
            entry["errors"].append("rendered image not found for prefix " + prefix)
            return entry
        entry["image"] = str(image_path)
        width, height, luminance = _read_pixels(image_path)
        entry["image_size"] = [width, height]
        peak = max(luminance) if luminance else 0.0
        entry["peak_luminance"] = peak
        if width != int(resolution[0]) or height != int(resolution[1]):
            entry["errors"].append(
                "rendered size {0}x{1} is not the declared output resolution "
                "{2}x{3}".format(width, height, int(resolution[0]), int(resolution[1])))
        if peak <= 0.05:
            entry["errors"].append("rendered image is black (peak {0:.4f})".format(peak))
            return entry
        components = _components(width, height, luminance, peak * BRIGHTNESS_FRACTION)
        entry["component_count"] = len(components)
        expected_sizes = []
        for marker in config["markers"]:
            maya_ndc = mapping.project_point_ndc(
                marker["point_maya"], config["camera_matrix"],
                config["focal_length_mm"], config["film_aperture_inches"],
                config["film_fit"], resolution, config["film_offsets_inches"],
                config["lens_squeeze_ratio"])
            expected_pixel = mapping.ndc_to_pixel(maya_ndc, resolution)
            report = {
                "name": marker["name"],
                "maya_ndc": [maya_ndc[0], maya_ndc[1]],
                "expected_pixel": [expected_pixel[0], expected_pixel[1]],
                "radius_cm": marker.get("_radius_cm"),
            }
            if marker.get("expected_ndc") is not None:
                report["unreal_ndc"] = [marker["expected_ndc"][0], marker["expected_ndc"][1]]
            if not (-1.0 <= maya_ndc[0] <= 1.0 and -1.0 <= maya_ndc[1] <= 1.0):
                # A marker the framing pushes out of the image cannot be
                # compared; it is reported, not counted against the check.
                report["skipped"] = "the expected centre is outside the rendered frame"
                entry["markers"].append(report)
                continue
            match = None
            best = None
            for component in components:
                size, centroid_x, centroid_y = component
                distance = math.hypot(centroid_x - expected_pixel[0],
                                      centroid_y - expected_pixel[1])
                if best is None or distance < best[0]:
                    best = (distance, component)
            if best is not None and best[0] <= max(8.0, width * 0.05):
                match = best[1]
            if match is None:
                report["error"] = "no rendered marker near the expected pixel"
                entry["markers"].append(report)
                continue
            size, centroid_x, centroid_y = match
            rendered_ndc = _pixel_to_ndc((centroid_x, centroid_y), resolution)
            report["rendered_pixel"] = [centroid_x, centroid_y]
            report["rendered_ndc"] = [rendered_ndc[0], rendered_ndc[1]]
            report["component_pixels"] = size
            report["pixel_delta_maya"] = [centroid_x - expected_pixel[0],
                                          centroid_y - expected_pixel[1]]
            report["ndc_delta_maya"] = [rendered_ndc[0] - maya_ndc[0],
                                        rendered_ndc[1] - maya_ndc[1]]
            if marker.get("expected_ndc") is not None:
                unreal_pixel = mapping.ndc_to_pixel(marker["expected_ndc"], resolution)
                report["pixel_delta_unreal"] = [centroid_x - unreal_pixel[0],
                                                centroid_y - unreal_pixel[1]]
                report["ndc_delta_unreal"] = [
                    rendered_ndc[0] - marker["expected_ndc"][0],
                    rendered_ndc[1] - marker["expected_ndc"][1]]
            report["within_tolerance"] = all(
                abs(value) <= TOLERANCE_PIXELS
                for value in (report["pixel_delta_maya"]
                              + report.get("pixel_delta_unreal", [])))
            expected_sizes.append(size)
            entry["markers"].append(report)
        entry["marker_pixel_sizes"] = expected_sizes
        compared = [report for report in entry["markers"] if "rendered_ndc" in report]
        entry["markers_compared"] = len(compared)
        entry["markers_skipped"] = len(entry["markers"]) - len(compared)
        deltas = [abs(value) for report in compared
                  for value in report.get("pixel_delta_maya", [])]
        entry["max_pixel_delta_maya"] = max(deltas) if deltas else None
        deltas = [abs(value) for report in compared
                  for value in report.get("pixel_delta_unreal", [])]
        entry["max_pixel_delta_unreal"] = max(deltas) if deltas else None
        entry["passed"] = (bool(compared) and not entry["errors"]
                           and all(report.get("within_tolerance") for report in compared))
        return entry
    finally:
        for node in scene_objects:
            if cmds.objExists(node):
                try:
                    cmds.delete(node)
                except RuntimeError:
                    pass
        for marker in config["markers"]:
            marker.pop("_sphere", None)


def run_render_check(configurations, output_dir, tolerance=TOLERANCE_PIXELS):
    """Render every configuration. Returns an honest availability report."""
    cmds = _cmds()
    available, detail = arnold_available()
    report = {"available": False, "reason": detail, "tolerance_pixels": tolerance,
              "configurations": []}
    if not available:
        return report
    image_format = image_format_for(cmds)
    if image_format[0] is None:
        report["reason"] = ("no readable image format in this Maya session; "
                            "defaultRenderGlobals.imageFormat has no png/tif/iff entry")
        return report
    report["renderer"] = detail
    report["image_format"] = image_format[1]
    Path(output_dir).mkdir(parents=True, exist_ok=True)
    previous_renderer = cmds.getAttr("defaultRenderGlobals.currentRenderer")
    report["available"] = True
    report["reason"] = None
    try:
        for config in configurations:
            report["configurations"].append(
                check_configuration(cmds, config, output_dir, image_format))
    except Exception as error:
        report["available"] = False
        report["reason"] = "{0}: {1}".format(type(error).__name__, error)
    finally:
        try:
            cmds.setAttr("defaultRenderGlobals.currentRenderer", previous_renderer,
                         type="string")
        except Exception:
            pass
    deltas = [entry.get("max_pixel_delta_maya") for entry in report["configurations"]]
    deltas = [delta for delta in deltas if delta is not None]
    report["max_pixel_delta_maya"] = max(deltas) if deltas else None
    report["passed"] = bool(report["configurations"]) and all(
        entry.get("passed") for entry in report["configurations"])
    return report
