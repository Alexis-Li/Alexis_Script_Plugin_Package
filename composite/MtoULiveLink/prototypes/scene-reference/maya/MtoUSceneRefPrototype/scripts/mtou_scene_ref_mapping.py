"""Pure Unreal -> Maya scene reference mapping, FBX scanning and comparison.

Implements the shared half of ``composite/MtoULiveLink/prototypes/scene-reference/
transfer.md``: the ``mtou-scene-ref-manifest/1`` reader, the FBX texture-record
detector, the axis candidates, the world-position fit and the per-object
comparison math. Nothing here imports Maya, so every rule can be checked under
plain CPython 3; ``MtoUSceneRefPrototype.py`` reads Maya nodes and feeds the
measurements into ``compare_measurements``.

Conventions
-----------
Unreal world space is left-handed, centimetres, ``+X`` forward, ``+Y`` right,
``+Z`` up. Maya world space is right-handed, centimetres, ``+X`` right, ``+Y``
up, ``-Z`` forward. A linear map that carries Unreal world coordinates to Maya
world coordinates has determinant ``-1`` when it respects both hosts'
forward/right/up notions, and ``+1`` when it only changes the up axis.

Every candidate in :data:`AXIS_CANDIDATES` maps an Unreal world **point** to a
Maya world point (``maya = M . ue``), and :func:`fit_linear_map` fits the same
direction.

Matrix storage
--------------
A 16 value world matrix is stored the way both hosts store it: row major, with
the first three rows holding the images of the object's local ``X``, ``Y`` and
``Z`` axes and the fourth row holding the translation. Unreal's ``FMatrix``
writes ``M[3]`` as the location and Maya's ``xform -m`` prints the same order,
so a matrix read on either host can be compared without a transpose.
:func:`carry_world_matrix` moves a whole world matrix through a candidate
(``ue -> maya``) and :func:`uncarry_world_matrix` moves it the other way.
Three values matrices follow the same reading: the rows of a candidate are the
three Unreal axes in Maya terms, which is why ``signed_permutation`` reads them
row by row.

Comparisons
-----------
:func:`compare_measurements` scores every candidate over the matched objects and
reports the winner's per-object position, world-bounding-box size and -- when
the local mesh has an unambiguous axis -- orientation errors, each against an
explicit centimetre or degree tolerance. A missing node is a reported problem,
never a skipped comparison.

The winner is a named candidate that places every object inside tolerance, so
the report names the convention the handoff really used; the least-squares fit
decides only when no named candidate does, and then only while it is a signed
permutation inside tolerance (``transform_check.decided_by`` says which decided,
``transform_check.winner`` carries the matrix the comparison used). This matters
because the engine's FBX level export and the camera route use *different*
Unreal to Maya maps: the handoff map is
``maya = (ue_x, ue_z, ue_y)`` (determinant -1) while the camera sync prototype
documents ``maya = (ue_y, ue_z, -ue_x)``. The report scores the camera route's
map on the same handoff in ``transform_check.camera_contract`` so the divergence
is on the record, and nothing here corrects the imported geometry.

FBX texture records
-------------------
The handoff may only be imported when its file records no material texture.
:func:`scan_fbx_texture_records` implements the frozen detector both hosts share,
line by line: ``Texture:`` opens a texture record, ``Video:`` opens a video
record, a line that is exactly ``}`` closes the current record, and inside a
record ``FileName:``/``RelativeFilename:`` record the last quoted token on the
line. It reports ``texture_records`` (every ``Texture:`` record, whether or not
it named a file), ``texture_references`` (records that named one),
``video_references`` and the distinct ``files`` in the order found. The importer
refuses on ``texture_records > 0``, because a texture record with no recorded
name is still a material input that cannot be carried.

Binary FBX cannot be read this way; the scan falls back to a documented
heuristic over the FBX string table, reports ``binary: True`` and gates on its
grouped texture nodes, so the caller can judge the evidence.
"""

import json
import math
import os
import re

MANIFEST_SCHEMA = "mtou-scene-ref-manifest/1"
REPORT_SCHEMA = "mtou-scene-ref-report/1"

DEFAULT_CONTAINER = "MtoU_UE_SceneRef"
DEFAULT_GRAY_MATERIAL = "MtoU_UE_SceneRef_Gray"

#: Image suffixes the contract lists for produced files and recorded names.
IMAGE_SUFFIXES = (".png", ".bmp", ".tga", ".jpg", ".jpeg", ".exr", ".hdr",
                  ".dds", ".fbm")

FBX_ASCII_MAGIC = b"; FBX "
FBX_BINARY_MAGIC = b"Kaydara FBX Binary"

#: Default comparison tolerances, reported with every run. ``offset_cm`` is the
#: pivot to bounding-box-centre vector: it is what separates "the same placement"
#: from "a mirrored placement", because a mirror leaves a symmetric mesh's size
#: and position untouched but moves that vector.
TOLERANCES = {"position_cm": 1.0, "size_cm": 1.0, "orientation_deg": 0.5,
              "offset_cm": 1.0, "centroid_cm": 1.0}

#: A fitted matrix counts as a signed permutation when every entry is within
#: this distance of the ideal ``0`` and ``+/-1`` entries.
PERMUTATION_TOLERANCE = 1e-6

#: Relative determinant below which a 3x3 system is treated as singular.
SINGULAR_RELATIVE_EPSILON = 1e-12

#: The camera route's Unreal to Maya map, from the camera sync prototype.
CAMERA_SYNC_CANDIDATE = "maya_x=ue_y, maya_y=ue_z, maya_z=-ue_x"

#: The map the engine's FBX level export round-trips through, measured on
#: 2026-09-28 with Unreal 5.7.4 and Maya 2024: the engine writes the node
#: translation with Y negated and Maya converts the file's Z-up axis system to
#: Y-up, so an Unreal world point (x, y, z) arrives at (x, z, y). Its
#: determinant is -1, which makes it a mirror of the camera route's map: the two
#: conventions are different and a product that carries both must reconcile
#: them.
ENGINE_HANDOFF_CANDIDATE = "maya_x=ue_x, maya_y=ue_z, maya_z=ue_y"

#: Reported as ``best`` when no named candidate places the objects inside
#: tolerance but the least-squares fit does.
FITTED_CANDIDATE_NAME = "fitted signed permutation"

#: Axis candidates, in report order. ``matrix`` maps Unreal world points to Maya
#: world points.
AXIS_CANDIDATES = (
    {
        "name": ENGINE_HANDOFF_CANDIDATE,
        "matrix": (1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0),
        "note": "the map the engine's FBX level export round-trips through, "
                "measured on 2026-09-28: determinant -1",
    },
    {
        "name": CAMERA_SYNC_CANDIDATE,
        "matrix": (0.0, 1.0, 0.0, 0.0, 0.0, 1.0, -1.0, 0.0, 0.0),
        "note": "the camera sync prototype's Unreal to Maya map: determinant -1",
    },
    {
        "name": "maya_x=ue_x, maya_y=ue_z, maya_z=-ue_y",
        "matrix": (1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, -1.0, 0.0),
        "note": "classic Z-up to Y-up axis change: determinant +1",
    },
    {
        "name": "maya_x=-ue_y, maya_y=ue_z, maya_z=ue_x",
        "matrix": (0.0, -1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0),
        "note": "the mirror of the camera sync map, in case the handoff flipped "
                "the front axis: determinant -1",
    },
    {
        "name": "identity",
        "matrix": (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0),
        "note": "no axis change; a diagnostic baseline, not an expected handoff",
    },
)

_QUOTED_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')

#: Record line heads the contract's texture walk opens records with.
_TEXTURE_RECORD_HEAD = "Texture:"
_VIDEO_RECORD_HEAD = "Video:"

#: Recorded file name properties the walk reads inside a record.
_FILE_NAME_HEADS = ("FileName:", "RelativeFilename:")

#: FBX property type names a binary string scan must not report as a file.
_FBX_TYPE_TOKENS = frozenset((
    "kstring", "kbytestring", "ktime", "double", "float", "integer", "bool",
    "color", "colorrgb", "colorrgba", "enum", "vector3d", "number", "url",
    "datetime", "compound", "object", "string",
))


class ManifestError(ValueError):
    """A manifest the importer refuses to read. ``category`` is stable."""

    def __init__(self, category, detail):
        ValueError.__init__(self, "{0}: {1}".format(category, detail))
        self.category = category
        self.detail = detail


# --------------------------------------------------------------- FBX reading

def sniff_fbx_file(path):
    """Identify a handoff file without importing it.

    Returns ``exists``, ``readable``, ``bytes``, ``format``
    (``ascii``/``binary``/``unknown``), the leading magic, and an error string
    when the file cannot be read. The file is recognised by its own header
    rather than its extension, because the bundled ``fbxmaya`` plugin accepts a
    non-FBX file with ``type='FBX'`` without raising and imports nothing.
    """
    result = {"source": str(path), "exists": False, "readable": False,
              "bytes": 0, "format": "unknown", "magic": "", "error": None}
    try:
        size = os.path.getsize(path)
    except OSError as error:
        result["error"] = "cannot stat file: {0}".format(error)
        return result
    result["exists"] = True
    result["bytes"] = int(size)
    try:
        with open(path, "rb") as handle:
            head = handle.read(4096)
    except OSError as error:
        result["error"] = "cannot read file: {0}".format(error)
        return result
    result["readable"] = True
    result["magic"] = head[:16].decode("latin-1", "replace").strip()
    if head.startswith(FBX_BINARY_MAGIC) or b"\x00" in head:
        result["format"] = "binary"
    elif head.startswith(FBX_ASCII_MAGIC) or head.lstrip().startswith(FBX_ASCII_MAGIC):
        result["format"] = "ascii"
    else:
        result["format"] = "unknown"
    return result


def read_fbx_text(path, limit_bytes=None):
    """Decoded ASCII FBX text of ``path`` (``utf-8``, falling back to latin-1)."""
    with open(path, "rb") as handle:
        raw = handle.read() if limit_bytes is None else handle.read(limit_bytes)
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("latin-1")


def scan_fbx_texture_records(text):
    """File texture records of an ASCII FBX document.

    The walk is the contract's own rule, frozen with the Unreal exporter's
    detector so both hosts count the same file identically:

    * a line whose trimmed text starts with ``Texture:`` opens a texture record;
    * a line whose trimmed text starts with ``Video:`` opens a video record;
    * a line that is exactly ``}`` closes the current record;
    * inside a record, a line starting with ``FileName:`` or
      ``RelativeFilename:`` records the last quoted token on that line.

    A record that names no file is still a texture record -- a material input
    that cannot be carried -- which is why the importer refuses on
    ``texture_records`` and not only on ``texture_references``. The rule is
    line based on purpose: the SDK writes a nested ``Properties70`` block before
    the name lines, so names that follow it are outside the record and
    ``texture_references`` can be lower than the names a human would read.
    """
    texture_records = []
    video_records = []
    files = []
    current = None
    for number, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        if not line:
            continue
        if line == "}":
            current = None
            continue
        if line.startswith(_TEXTURE_RECORD_HEAD):
            current = {"kind": "Texture", "line": number, "header": line,
                       "file_names": []}
            texture_records.append(current)
            continue
        if line.startswith(_VIDEO_RECORD_HEAD):
            current = {"kind": "Video", "line": number, "header": line,
                       "file_names": []}
            video_records.append(current)
            continue
        if current is None:
            continue
        if any(line.startswith(head) for head in _FILE_NAME_HEADS):
            quoted = _QUOTED_RE.findall(line)
            if not quoted:
                continue
            name = quoted[-1]
            if not name:
                continue
            current["file_names"].append(name)
            if name not in files:
                files.append(name)
    texture_files = _unique([name for record in texture_records
                             for name in record["file_names"]])
    video_files = _unique([name for record in video_records
                           for name in record["file_names"]])
    image_files = [name for name in files if is_image_file_name(name)]
    records = []
    for record in texture_records:
        entry = dict(record)
        entry["recorded_file_name"] = _preferred_file_name(record["file_names"])
        entry["image"] = is_image_file_name(entry["recorded_file_name"])
        records.append(entry)
    return {
        "texture_records": len(texture_records),
        "texture_references": len([record for record in texture_records
                                   if record["file_names"]]),
        "video_references": len(video_records),
        "files": files,
        "texture_files": texture_files,
        "video_files": video_files,
        "image_files": image_files,
        "image_references": len(image_files),
        "records": records,
        "video_records": video_records,
    }


def scan_fbx_binary_records(raw):
    """Best-effort file texture records of a binary FBX byte stream.

    Binary FBX cannot be read as text, so the scan locates the FBX node table
    entries named ``RelativeFilename``/``FileName`` whose value is a printable
    string with a path or an extension, keeping only records whose nearest
    enclosing node is a ``Texture`` object. FBX type tokens from the property
    templates and the ``Original|FileName`` scene property are rejected, since
    both skip the layout checks below.
    """
    records = []
    for marker in (b"RelativeFilename", b"FileName"):
        for match in re.finditer(marker, raw):
            start = match.start()
            # The byte before a node name is its length: a FileName marker that
            # is the tail of the ``Original|FileName`` property fails here.
            if raw[start - 1:start] != bytes((len(marker),)):
                continue
            kind, node = _binary_enclosing_node(raw, start)
            if kind != "Texture":
                continue
            index = match.end()
            if raw[index:index + 1] != b"S":
                continue
            length = int.from_bytes(raw[index + 1:index + 5], "little")
            if length <= 0 or length > 4096:
                continue
            value = raw[index + 5:index + 5 + length]
            try:
                text = value.decode("utf-8")
            except UnicodeDecodeError:
                text = value.decode("latin-1")
            if not text or text.strip().lower() in _FBX_TYPE_TOKENS:
                continue
            if not all(character.isprintable() for character in text):
                continue
            if "." not in text and "/" not in text and "\\" not in text:
                continue
            records.append({"property": marker.decode("ascii"), "value": text,
                            "kind": "Texture", "node": node, "offset": start})
    # One texture object records both a relative and an absolute name; the
    # records of one node are one file texture reference.
    grouped = {}
    for record in records:
        entry = grouped.setdefault(record["node"], {"kind": "Texture", "node": record["node"],
                                                    "properties": [],
                                                    "line": None,
                                                    "offset": record["offset"]})
        entry["properties"].append({"property": record["property"], "value": record["value"]})
    for entry in grouped.values():
        entry["file_names"] = _unique([item["value"] for item in entry["properties"]])
        entry["recorded_file_name"] = _preferred_file_name(entry["file_names"])
        entry["image"] = is_image_file_name(entry["recorded_file_name"])
    return [grouped[key] for key in sorted(grouped)]


#: Node names a binary FBX node header can carry, with their length byte.
_BINARY_NODE_MARKERS = tuple(
    (name, bytes((len(name),)) + name.encode("ascii"))
    for name in ("Texture", "Video", "Material", "Geometry", "Model", "NodeAttribute",
                 "Objects", "Connections", "Takes"))


def _binary_enclosing_node(raw, offset, window=1 << 14):
    """``(name, offset)`` of the nearest FBX node header preceding ``offset``."""
    best = None
    best_at = -1
    start = max(0, offset - window)
    for name, marker in _BINARY_NODE_MARKERS:
        at = raw.rfind(marker, start, offset)
        if at > best_at:
            best_at = at
            best = name
    return best, best_at


def scan_fbx_file(path):
    """Sniff and scan a handoff file; the result feeds the report's scan block.

    ``texture_references`` counts the *file* texture records the scan found and
    ``files`` holds one recorded name per record, relative names preferred.
    ``format`` is ``unknown`` for a file that is not FBX at all, and a binary
    scan is reported as heuristic evidence rather than as proof of a texture
    free handoff.
    """
    sniffed = sniff_fbx_file(path)
    scan = {"source": str(path), "exists": sniffed["exists"],
            "readable": sniffed["readable"], "bytes": sniffed["bytes"],
            "format": sniffed["format"], "magic": sniffed["magic"],
            "error": sniffed["error"], "binary": sniffed["format"] == "binary",
            "texture_records": 0, "texture_references": 0, "video_references": 0,
            "files": [], "texture_files": [], "video_files": [], "image_files": [],
            "image_references": 0, "records": [], "video_records": [],
            "allowed": False, "note": ""}
    if not sniffed["readable"]:
        return scan
    if sniffed["format"] == "ascii":
        scan.update(scan_fbx_texture_records(read_fbx_text(path)))
        scan["note"] = "ascii FBX: texture objects parsed from the document text"
    elif sniffed["format"] == "binary":
        with open(path, "rb") as handle:
            raw = handle.read()
        records = scan_fbx_binary_records(raw)
        names = _unique([name for record in records for name in record["file_names"]])
        scan["records"] = records
        scan["texture_records"] = len(records)
        scan["texture_references"] = len([record for record in records
                                          if record["file_names"]])
        scan["video_references"] = 0
        scan["files"] = names
        scan["texture_files"] = names
        scan["video_files"] = []
        scan["image_files"] = [name for name in names if is_image_file_name(name)]
        scan["image_references"] = len(scan["image_files"])
        scan["note"] = ("binary FBX: a heuristic string table scan only, node context "
                        "is not fully parsed and a clean scan is not proof")
    else:
        scan["note"] = ("not an FBX file: the header matches neither the ASCII "
                        "nor the binary FBX magic")
    return scan


def is_image_file_name(value):
    """Whether ``value`` ends in one of the contract's image suffixes."""
    if not value:
        return False
    name = str(value).strip().strip('"').replace("\\", "/").lower()
    return name.endswith(IMAGE_SUFFIXES)


def _preferred_file_name(names):
    """The relative recorded name when there is one, else the first name."""
    for name in names:
        stripped = str(name).strip()
        if stripped and not os.path.isabs(stripped) and "\\" not in stripped:
            return stripped
    return str(names[0]).strip() if names else ""


def _unique(values):
    seen = []
    for value in values:
        if value not in seen:
            seen.append(value)
    return seen


# ------------------------------------------------------------------ manifest

def load_manifest(path):
    """Read and validate a ``mtou-scene-ref-manifest/1`` document.

    Raises :class:`ManifestError` with a stable category when the file cannot be
    read, is not JSON, is not an object, or declares another schema; the caller
    turns that into the contract's refusal exit code.
    """
    try:
        with open(path, "r", encoding="utf-8") as handle:
            text = handle.read()
    except OSError as error:
        raise ManifestError("MANIFEST_UNREADABLE",
                            "cannot read manifest {0}: {1}".format(path, error))
    try:
        manifest = json.loads(text)
    except ValueError as error:
        raise ManifestError("MANIFEST_INVALID_JSON",
                            "manifest {0} is not valid JSON: {1}".format(path, error))
    if not isinstance(manifest, dict):
        raise ManifestError("MANIFEST_INVALID_JSON",
                            "manifest {0} is not a JSON object".format(path))
    schema = manifest.get("schema")
    if schema != MANIFEST_SCHEMA:
        raise ManifestError(
            "MANIFEST_SCHEMA_MISMATCH",
            "manifest schema is {0!r}, expected {1!r}".format(schema, MANIFEST_SCHEMA))
    objects = manifest.get("objects")
    if objects is None or not isinstance(objects, list):
        raise ManifestError("MANIFEST_MISSING_OBJECTS",
                            "manifest {0} has no objects list".format(path))
    return manifest


def manifest_summary(manifest):
    """Scope, world and scale summary the importer reports before importing."""
    world = manifest.get("world") if isinstance(manifest.get("world"), dict) else {}
    scope = manifest.get("scope") if isinstance(manifest.get("scope"), dict) else {}
    scale = manifest.get("scale") if isinstance(manifest.get("scale"), dict) else {}
    return {
        "schema": manifest.get("schema"),
        "generated_utc": manifest.get("generated_utc"),
        "engine_version": manifest.get("engine_version"),
        "world": {
            "package": world.get("package"),
            "name": world.get("name"),
            "up_axis": world.get("up_axis"),
            "linear_unit": world.get("linear_unit"),
            "world_partition": world.get("world_partition"),
        },
        "scope": {
            "kind": scope.get("kind"),
            "persistent_level": scope.get("persistent_level"),
            "requested_sublevels": scope.get("requested_sublevels") or [],
            "loaded_sublevels": scope.get("loaded_sublevels") or [],
            "unloaded_sublevels": scope.get("unloaded_sublevels") or [],
            "excluded_sublevels": scope.get("excluded_sublevels") or [],
        },
        "scale": {
            "actors": scale.get("actors"),
            "components": scale.get("components"),
            "objects": scale.get("objects"),
            "triangles": scale.get("triangles"),
            "vertices": scale.get("vertices"),
        },
        "objects_total": len(manifest.get("objects") or []),
        "objects_exported": len(exported_objects(manifest)),
        "unsupported": manifest.get("unsupported") or [],
        "skipped": manifest.get("skipped") or [],
        "output": manifest.get("output") or {},
    }


def exported_objects(manifest):
    """Manifest objects the exporter claims to have written into the handoff."""
    return [entry for entry in (manifest.get("objects") or [])
            if isinstance(entry, dict) and entry.get("exported") is True]


def manifest_object_requirement(entry):
    """``(name, matrix, size, offset, centroid, problems)`` of one object.

    The offset is the vector from the object's world position to the centre of
    its world bounding box and the centroid is the area-weighted centroid of
    the mesh's world space LOD 0 triangles. Together they are what a mirror of the geometry
    moves while the position and the box size stay the same: the offset sees a
    mesh whose box sits off its pivot, the centroid sees any asymmetric mesh,
    including one whose box is centred on the pivot. A missing or malformed
    measurement field is a per-object problem the report carries, not a refusal
    -- except the centroid, which an older manifest may simply not carry, so it
    is reported as unavailable for that object instead.
    """
    problems = []
    name = entry.get("node_name")
    if not isinstance(name, str) or not name.strip():
        problems.append("manifest object {0!r} has no node_name".format(entry.get("id")))
        name = ""
    matrix = None
    values = entry.get("world_matrix")
    if isinstance(values, list) and len(values) == 16:
        try:
            matrix = [float(value) for value in values]
        except (TypeError, ValueError):
            problems.append("{0}: world_matrix is not numeric".format(name))
    else:
        problems.append("{0}: manifest object has no 16 value world_matrix".format(name))
    size = None
    bounds = entry.get("world_bounds_cm")
    if isinstance(bounds, dict):
        candidate = bounds.get("size")
        if isinstance(candidate, dict):
            try:
                size = [float(candidate.get(axis, 0.0)) for axis in ("x", "y", "z")]
            except (TypeError, ValueError):
                problems.append("{0}: world_bounds_cm.size is not numeric".format(name))
        elif isinstance(candidate, list) and len(candidate) == 3:
            try:
                size = [float(value) for value in candidate]
            except (TypeError, ValueError):
                problems.append("{0}: world_bounds_cm.size is not numeric".format(name))
    if size is None:
        problems.append("{0}: manifest object has no world_bounds_cm.size".format(name))
    offset = _manifest_offset(entry, matrix, name, problems)
    centroid = _manifest_centroid(entry, name)
    return name, matrix, size, offset, centroid, problems


def _manifest_centroid(entry, name):
    """The manifest's world space surface centroid, or ``None`` when absent.

    The engine writes ``world_surface_centroid_cm``: the area-weighted centroid
    of the mesh's LOD 0 triangles. A vertex average would compare two different
    vertex sets -- the exported file welds duplicated render vertices away --
    which is why the triangle based quantity is the one both hosts can compare.
    A handoff written before this check existed carries no centroid, which is
    reported as an unavailable check rather than as a problem.
    """
    centroid = entry.get("world_surface_centroid_cm")
    if centroid is None:
        return None
    if not isinstance(centroid, dict):
        return None
    try:
        return [float(centroid.get(axis, 0.0)) for axis in ("x", "y", "z")]
    except (TypeError, ValueError):
        return None


def _manifest_offset(entry, matrix, name, problems):
    """The manifest's position-to-bounding-box-centre vector, or ``None``.

    ``world_location_cm`` is the contract's position; the world matrix's fourth
    row is the same value and is used when the location is missing. The centre
    needs both ``min`` and ``max``, so a manifest that carries only a size
    reports the offset as unmeasured instead of guessing one.
    """
    bounds = entry.get("world_bounds_cm")
    if not isinstance(bounds, dict):
        problems.append("{0}: manifest object has no world_bounds_cm".format(name))
        return None
    minimum = bounds.get("min")
    maximum = bounds.get("max")
    if not isinstance(minimum, dict) or not isinstance(maximum, dict):
        problems.append(
            "{0}: manifest object has no world_bounds_cm.min/max, so the "
            "position to bounds centre offset cannot be checked".format(name))
        return None
    location = entry.get("world_location_cm")
    if isinstance(location, dict):
        try:
            position = [float(location.get(axis, 0.0)) for axis in ("x", "y", "z")]
        except (TypeError, ValueError):
            problems.append("{0}: world_location_cm is not numeric".format(name))
            return None
    elif matrix is not None:
        position = list(matrix4_translation(matrix))
    else:
        problems.append("{0}: manifest object has no world position".format(name))
        return None
    try:
        centre = [(float(minimum.get(axis, 0.0)) + float(maximum.get(axis, 0.0))) / 2.0
                  for axis in ("x", "y", "z")]
    except (TypeError, ValueError):
        problems.append("{0}: world_bounds_cm.min/max is not numeric".format(name))
        return None
    return [centre[axis] - position[axis] for axis in range(3)]


# ------------------------------------------------------------- linear algebra

def matrix3(values):
    """Row-major 3x3 matrix from nine values or from three rows."""
    if len(values) == 3 and all(isinstance(row, (tuple, list)) for row in values):
        return tuple(tuple(float(value) for value in row) for row in values)
    numbers = [float(value) for value in values]
    if len(numbers) != 9:
        raise ValueError("a 3x3 matrix needs 9 values, got {0}".format(len(numbers)))
    return tuple(tuple(numbers[row * 3:row * 3 + 3]) for row in range(3))


def matrix3_values(matrix):
    """Row-major nine values of a 3x3 matrix."""
    rows = matrix3(matrix)
    return [rows[row][column] for row in range(3) for column in range(3)]


def matrix3_determinant(matrix):
    rows = matrix3(matrix)
    return (rows[0][0] * (rows[1][1] * rows[2][2] - rows[1][2] * rows[2][1])
            - rows[0][1] * (rows[1][0] * rows[2][2] - rows[1][2] * rows[2][0])
            + rows[0][2] * (rows[1][0] * rows[2][1] - rows[1][1] * rows[2][0]))


def matrix3_multiply(first, second):
    left = matrix3(first)
    right = matrix3(second)
    return tuple(tuple(sum(left[row][step] * right[step][column] for step in range(3))
                       for column in range(3)) for row in range(3))


def matrix3_apply(matrix, vector):
    rows = matrix3(matrix)
    return tuple(sum(rows[row][column] * float(vector[column]) for column in range(3))
                 for row in range(3))


def matrix3_inverse(matrix):
    """Inverse of a 3x3 matrix, or ``None`` when it is numerically singular."""
    rows = matrix3(matrix)
    determinant = matrix3_determinant(rows)
    scale = max(abs(value) for row in rows for value in row) or 1.0
    if abs(determinant) <= SINGULAR_RELATIVE_EPSILON * (scale ** 3):
        return None
    cofactors = [[0.0] * 3 for _ in range(3)]
    for row in range(3):
        for column in range(3):
            minor = [[rows[r][c] for c in range(3) if c != column]
                     for r in range(3) if r != row]
            value = minor[0][0] * minor[1][1] - minor[0][1] * minor[1][0]
            cofactors[row][column] = value if (row + column) % 2 == 0 else -value
    return tuple(tuple(cofactors[column][row] / determinant for column in range(3))
                 for row in range(3))


def matrix4_values(values):
    """Validated row-major 4x4 matrix from sixteen values."""
    numbers = [float(value) for value in values]
    if len(numbers) != 16:
        raise ValueError("a 4x4 matrix needs 16 values, got {0}".format(len(numbers)))
    return numbers


def matrix4_linear(values):
    """The 3x3 linear part of a 4x4 matrix: the images of the local axes."""
    numbers = matrix4_values(values)
    return matrix3([numbers[0], numbers[1], numbers[2],
                    numbers[4], numbers[5], numbers[6],
                    numbers[8], numbers[9], numbers[10]])


def matrix4_translation(values):
    """The translation a world matrix stores in its fourth row."""
    numbers = matrix4_values(values)
    return (numbers[12], numbers[13], numbers[14])


def matrix4_from_linear_and_translation(matrix, translation):
    """A 4x4 world matrix from its three axis images and its translation."""
    values = matrix3_values(matrix)
    return [values[0], values[1], values[2], 0.0,
            values[3], values[4], values[5], 0.0,
            values[6], values[7], values[8], 0.0,
            float(translation[0]), float(translation[1]), float(translation[2]), 1.0]


def matrix3_transpose(matrix):
    rows = matrix3(matrix)
    return tuple(tuple(rows[column][row] for column in range(3)) for row in range(3))


def carry_world_matrix(candidate, world_matrix):
    """A world matrix carried through ``candidate`` (``ue -> maya``).

    The candidate is a point map, so carrying a transform composes it with the
    transposed map: the result's rows are the Maya images of the object's local
    axes and its fourth row is the mapped position.
    """
    rows = matrix3(candidate)
    linear = matrix4_linear(world_matrix)
    carried = matrix3_multiply(linear, matrix3_transpose(rows))
    position = matrix3_apply(rows, matrix4_translation(world_matrix))
    return matrix4_from_linear_and_translation(carried, position)


def uncarry_world_matrix(candidate, world_matrix):
    """The inverse of :func:`carry_world_matrix`: a Maya matrix in Unreal terms."""
    rows = matrix3(candidate)
    inverse = matrix3_inverse(rows)
    if inverse is None:
        raise ValueError("the candidate map is singular")
    linear = matrix4_linear(world_matrix)
    uncarried = matrix3_multiply(linear, matrix3_transpose(inverse))
    position = matrix3_apply(inverse, matrix4_translation(world_matrix))
    return matrix4_from_linear_and_translation(uncarried, position)


def vector_length(vector):
    return math.sqrt(sum(float(value) * float(value) for value in vector))


def angle_between_degrees(first, second):
    """Angle in degrees between two vectors, ``None`` for a zero length one."""
    first_length = vector_length(first)
    second_length = vector_length(second)
    if first_length <= 0.0 or second_length <= 0.0:
        return None
    dot = sum(float(a) * float(b) for a, b in zip(first, second))
    cosine = max(-1.0, min(1.0, dot / (first_length * second_length)))
    return math.degrees(math.acos(cosine))


def signed_permutation(matrix, tolerance=PERMUTATION_TOLERANCE):
    """How far ``matrix`` is from a signed permutation.

    ``permutation[row]`` is the Unreal axis each Maya axis takes and
    ``signs[row]`` its sign, so ``maya[row] = signs[row] * ue[permutation[row]]``.
    ``max_permutation_deviation`` is the largest distance from the ideal
    entries.
    """
    rows = matrix3(matrix)
    deviation = 0.0
    permutation = []
    signs = []
    ok = True
    for row in range(3):
        values = rows[row]
        column = max(range(3), key=lambda index: abs(values[index]))
        expected = 1 if values[column] >= 0.0 else -1
        deviation = max(deviation, abs(abs(values[column]) - 1.0))
        for index in range(3):
            if index != column:
                deviation = max(deviation, abs(values[index]))
        permutation.append(column)
        signs.append(expected)
    if sorted(permutation) != [0, 1, 2]:
        ok = False
    if deviation > tolerance:
        ok = False
    return {"signed_permutation": bool(ok), "permutation": permutation,
            "signs": signs, "max_permutation_deviation": deviation,
            "determinant": matrix3_determinant(rows)}


def expected_axis_sizes(matrix, unreal_size):
    """Maya world bounding-box size a candidate predicts from an Unreal size.

    Exact for the signed permutations a handoff produces; for a general matrix
    it is the enclosing box of the mapped box.
    """
    rows = matrix3(matrix)
    return [sum(abs(rows[row][column]) * float(unreal_size[column]) for column in range(3))
            for row in range(3)]


def fit_linear_map(unreal_points, maya_points):
    """Least-squares 3x3 map ``maya = M . unreal`` over paired positions.

    Translation is removed from both point sets before the fit, so ``M``
    carries only rotation, reflection and scale. The fit needs at least three
    non-degenerate Unreal positions; otherwise ``available`` is false with the
    reason and the caller falls back to the named candidates alone.
    """
    result = {"available": False, "reason": "", "points": len(unreal_points),
              "matrix": None, "determinant": None,
              "max_position_error_cm": None, "rms_position_error_cm": None,
              "signed_permutation": False, "permutation": None, "signs": None,
              "max_permutation_deviation": None, "unreal_mean_cm": None,
              "maya_mean_cm": None}
    if len(unreal_points) < 3 or len(unreal_points) != len(maya_points):
        result["reason"] = ("the fit needs at least three matched objects, got "
                            "{0}".format(min(len(unreal_points), len(maya_points))))
        return result
    count = len(unreal_points)
    unreal_mean = [sum(point[axis] for point in unreal_points) / count for axis in range(3)]
    maya_mean = [sum(point[axis] for point in maya_points) / count for axis in range(3)]
    covariance = [[0.0] * 3 for _ in range(3)]
    gram = [[0.0] * 3 for _ in range(3)]
    for unreal, maya in zip(unreal_points, maya_points):
        offset_unreal = [unreal[axis] - unreal_mean[axis] for axis in range(3)]
        offset_maya = [maya[axis] - maya_mean[axis] for axis in range(3)]
        for row in range(3):
            for column in range(3):
                covariance[row][column] += offset_maya[row] * offset_unreal[column]
                gram[row][column] += offset_unreal[row] * offset_unreal[column]
    inverse = matrix3_inverse(gram)
    if inverse is None:
        result["reason"] = ("the matched Unreal positions span a plane or less, so the "
                            "map is not determined: at least four non-coplanar matched "
                            "objects are needed")
        return result
    linear = matrix3_multiply(covariance, inverse)
    errors = [vector_length(tuple(matrix3_apply(linear, unreal)[axis] - maya[axis]
                                  for axis in range(3)))
              for unreal, maya in zip(unreal_points, maya_points)]
    permutation = signed_permutation(linear)
    result.update({
        "available": True,
        "matrix": matrix3_values(linear),
        "determinant": matrix3_determinant(linear),
        "max_position_error_cm": max(errors),
        "rms_position_error_cm": math.sqrt(sum(error * error for error in errors) / count),
        "signed_permutation": permutation["signed_permutation"],
        "permutation": permutation["permutation"],
        "signs": permutation["signs"],
        "max_permutation_deviation": permutation["max_permutation_deviation"],
        "unreal_mean_cm": unreal_mean,
        "maya_mean_cm": maya_mean,
    })
    return result


def orientation_measurable(local_size, relative_tolerance=1e-3):
    """Whether a mesh's own axes are unambiguous.

    A mesh whose local bounding box has three distinct extents pins its axes.
    For anything else a symmetry rotation can produce the same geometry with a
    different node matrix, so the orientation comparison is reported as skipped
    rather than as passed.
    """
    if not local_size or len(local_size) != 3:
        return False
    values = sorted(float(value) for value in local_size)
    largest = values[-1]
    if largest <= 0.0:
        return False
    return all((values[index + 1] - values[index]) / largest > relative_tolerance
               for index in range(2))


def compare_measurements(candidates, measurements, tolerances=None):
    """Score the axis maps and compare every matched object.

    ``measurements`` holds one entry per exported manifest object:
    ``node_name``, ``matched_id``, ``found``, ``path``, ``matched_by``,
    ``ambiguous_matches``, ``problems``, ``ue_matrix`` (16 Unreal values),
    ``ue_size`` (centimetres), ``ue_offset`` (position to bounds centre),
    ``maya_matrix`` (16 Maya values or ``None``), ``maya_size``,
    ``maya_offset`` and ``local_size``.

    Every named candidate is scored on the same handoff, so the axis convention
    is measured rather than assumed, and the least-squares fit of the matched
    positions is scored the same way. The comparison decides with a named
    candidate that places every object inside tolerance; only when none does
    does the fitted signed-permutation map decide. The per-object comparison
    uses that winner, so a placement that is consistent under one map reports no
    position or size problem -- it is the map, not the handoff, that is wrong.
    A map that no candidate and no fit explains still reports every mismatch.

    Every object is also compared by its pivot to bounding-box-centre vector.
    That vector is what separates the same placement from a mirrored one: a
    mirror leaves a symmetric mesh's position and world box size untouched, but
    it moves the box centre relative to the pivot, and the check needs no local
    axis to be unambiguous, so it runs even where the orientation check is
    skipped.

    Returns ``transform_check`` (the contract shape, the scored candidates, the
    winner, the fit and the camera route's divergence), the per-object
    ``objects`` list, and the ``problems``/``warnings`` strings the caller folds
    into the run report.
    """
    limits = dict(TOLERANCES)
    if tolerances:
        limits.update(tolerances)
    usable = [entry for entry in measurements
              if entry.get("found") and entry.get("ue_matrix") and entry.get("maya_matrix")]
    missing = [entry for entry in measurements if not entry.get("found")]
    fit = fit_linear_map([matrix4_translation(entry["ue_matrix"]) for entry in usable],
                         [matrix4_translation(entry["maya_matrix"]) for entry in usable])

    scored = [_score_candidate(candidate["name"], candidate.get("note", ""),
                              matrix3(candidate["matrix"]), usable, index)
              for index, candidate in enumerate(candidates)]
    fitted = None
    if fit["available"] and fit["signed_permutation"]:
        fitted = _score_candidate(FITTED_CANDIDATE_NAME,
                                  "the least-squares fit of the measured positions, "
                                  "reported in the fit block",
                                  matrix3(fit["matrix"]), usable, len(scored))
    winner, decided_by = _select_winner(scored, fitted, limits)
    winner_matrix = matrix3(winner["matrix"]) if winner is not None else None

    problems = []
    warnings = []
    objects = []
    for entry in measurements:
        if not entry.get("found"):
            detail = _object_report(entry)
            detail["problems"] = [
                "MISSING_NODE: {0} ({1}) was not found in the container".format(
                    entry.get("node_name"), entry.get("matched_id") or "no id")]
            problems.extend(detail["problems"])
            objects.append(detail)
            continue
        for problem in entry.get("problems") or []:
            problems.append("MANIFEST_FIELD: {0}".format(problem))
        if entry.get("ambiguous_matches"):
            warnings.append("AMBIGUOUS_NODE: {0} matched by suffix, candidates {1}".format(
                entry.get("node_name"), entry["ambiguous_matches"]))
        expected_position = None
        measured_position = None
        expected_size = None
        expected_offset = None
        measured_offset = entry.get("maya_offset")
        expected_centroid = None
        measured_centroid = entry.get("maya_centroid")
        position_error = None
        size_error = None
        offset_error = None
        centroid_error = None
        orientation_error = None
        checked = False
        object_problems = []
        if winner_matrix is not None and entry.get("ue_matrix") and entry.get("maya_matrix"):
            expected_position = matrix3_apply(winner_matrix,
                                              matrix4_translation(entry["ue_matrix"]))
            measured_position = matrix4_translation(entry["maya_matrix"])
            position_error = vector_length(tuple(
                expected_position[axis] - measured_position[axis] for axis in range(3)))
            if entry.get("ue_size") and entry.get("maya_size"):
                expected_size = expected_axis_sizes(winner_matrix, entry["ue_size"])
                size_error = max(abs(expected_size[axis] - float(entry["maya_size"][axis]))
                                 for axis in range(3))
            if entry.get("ue_offset") and measured_offset:
                expected_offset = matrix3_apply(winner_matrix, entry["ue_offset"])
                offset_error = vector_length(tuple(
                    expected_offset[axis] - float(measured_offset[axis])
                    for axis in range(3)))
            if entry.get("ue_centroid") and measured_centroid:
                expected_centroid = matrix3_apply(winner_matrix, entry["ue_centroid"])
                centroid_error = vector_length(tuple(
                    expected_centroid[axis] - float(measured_centroid[axis])
                    for axis in range(3)))
            orientation_error = _orientation_error(winner_matrix, entry)
            checked = orientation_error is not None
            if position_error > limits["position_cm"]:
                object_problems.append(
                    "POSITION_MISMATCH: {0} expected ({1}) measured ({2}) error "
                    "{3:.4f} cm (tolerance {4} cm)".format(
                        entry.get("node_name"), _format_vector(expected_position),
                        _format_vector(measured_position), position_error,
                        limits["position_cm"]))
            if size_error is not None and size_error > limits["size_cm"]:
                object_problems.append(
                    "SIZE_MISMATCH: {0} expected size ({1}) measured ({2}) error "
                    "{3:.4f} cm (tolerance {4} cm)".format(
                        entry.get("node_name"), _format_vector(expected_size),
                        _format_vector(entry["maya_size"]), size_error, limits["size_cm"]))
            if offset_error is not None and offset_error > limits["offset_cm"]:
                object_problems.append(
                    "OFFSET_MISMATCH: {0} expected pivot to bounds centre ({1}) "
                    "measured ({2}) error {3:.4f} cm (tolerance {4} cm)".format(
                        entry.get("node_name"), _format_vector(expected_offset),
                        _format_vector(measured_offset), offset_error,
                        limits["offset_cm"]))
            if centroid_error is not None and centroid_error > limits["centroid_cm"]:
                object_problems.append(
                    "CENTROID_MISMATCH: {0} expected surface centroid ({1}) measured "
                    "({2}) error {3:.4f} cm (tolerance {4} cm, {5} triangles{6})".format(
                        entry.get("node_name"), _format_vector(expected_centroid),
                        _format_vector(measured_centroid), centroid_error,
                        limits["centroid_cm"], entry.get("centroid_triangles_used"),
                        ", sampled" if entry.get("centroid_sampled") else ""))
            if orientation_error is not None and \
                    orientation_error > limits["orientation_deg"]:
                object_problems.append(
                    "ORIENTATION_MISMATCH: {0} error {1:.4f} deg (tolerance {2} deg)".format(
                        entry.get("node_name"), orientation_error, limits["orientation_deg"]))
        detail = _object_report(entry, position_error=position_error,
                                size_error=size_error,
                                expected_position=expected_position,
                                measured_position=measured_position,
                                expected_size=expected_size,
                                offset_error=offset_error,
                                expected_offset=expected_offset,
                                centroid_error=centroid_error,
                                expected_centroid=expected_centroid,
                                orientation_error=orientation_error,
                                orientation_checked=checked)
        detail["problems"] = list(object_problems)
        problems.extend(object_problems)
        objects.append(detail)

    without_centroid = [entry.get("node_name") for entry in measurements
                        if entry.get("found") and not entry.get("ue_centroid")]
    if without_centroid:
        warnings.append(
            "CENTROID_UNAVAILABLE: {0} matched object(s) carry no "
            "world_surface_centroid_cm in the manifest, so the mirror check is "
            "unavailable for them: {1}".format(len(without_centroid),
                                               without_centroid))
    unreadable = [entry.get("node_name") for entry in measurements
                  if entry.get("found") and not entry.get("maya_centroid")
                  and entry.get("centroid_note")]
    if unreadable:
        warnings.append(
            "CENTROID_UNREADABLE: the vertices of {0} could not be read, so the "
            "mirror check is unavailable for them".format(unreadable))

    duplicates = {}
    for entry in measurements:
        if entry.get("found") and entry.get("path"):
            duplicates.setdefault(entry["path"], []).append(entry.get("node_name"))
    for path, names in duplicates.items():
        if len(names) > 1:
            warnings.append("SHARED_NODE: manifest objects {0} all matched {1}".format(
                ", ".join(str(name) for name in names), path))

    matched = (winner is not None and not missing
               and _within_tolerance(winner, limits))
    transform_check = {
        "candidate": winner["name"] if winner is not None else None,
        "best": winner["name"] if winner is not None else None,
        "candidates": [
            {"name": entry["name"], "note": entry["note"], "matrix": entry["matrix"],
             "determinant": entry["determinant"],
             "objects_compared": entry["objects_compared"],
             "max_position_error_cm": entry["max_position_error_cm"],
             "max_size_error_cm": entry["max_size_error_cm"],
             "max_offset_error_cm": entry["max_offset_error_cm"],
             "offset_objects": entry["offset_objects"],
             "max_centroid_error_cm": entry["max_centroid_error_cm"],
             "centroid_objects": entry["centroid_objects"],
             "max_orientation_error_deg": entry["max_orientation_error_deg"],
             "orientation_objects": entry["orientation_objects"]}
            for entry in scored],
        "matched": matched,
        "decided_by": decided_by,
        "winner": ({"name": winner["name"], "source": decided_by,
                    "matrix": winner["matrix"], "determinant": winner["determinant"],
                    "max_position_error_cm": winner["max_position_error_cm"],
                    "max_size_error_cm": winner["max_size_error_cm"],
                    "max_offset_error_cm": winner["max_offset_error_cm"],
                    "max_centroid_error_cm": winner["max_centroid_error_cm"],
                    "max_orientation_error_deg": winner["max_orientation_error_deg"]}
                   if winner is not None else None),
        "max_position_error_cm": winner["max_position_error_cm"] if winner else None,
        "max_size_error_cm": winner["max_size_error_cm"] if winner else None,
        "max_offset_error_cm": winner["max_offset_error_cm"] if winner else None,
        "max_centroid_error_cm": winner["max_centroid_error_cm"] if winner else None,
        "max_orientation_error_deg": (winner["max_orientation_error_deg"]
                                      if winner else None),
        "fit": fit,
        "camera_contract": _camera_contract(scored, limits),
        "tolerances": limits,
        "objects_compared": len(usable),
        "objects_missing": len(missing),
    }
    return {"transform_check": transform_check, "objects": objects,
            "problems": problems, "warnings": warnings}


def _score_candidate(name, note, matrix, usable, index):
    """The errors one map produces over the matched objects."""
    positions = []
    sizes = []
    orientations = []
    offsets = []
    centroids = []
    for entry in usable:
        expected = matrix3_apply(matrix, matrix4_translation(entry["ue_matrix"]))
        measured = matrix4_translation(entry["maya_matrix"])
        positions.append(vector_length(tuple(expected[axis] - measured[axis]
                                             for axis in range(3))))
        if entry.get("ue_size") and entry.get("maya_size"):
            expected_size = expected_axis_sizes(matrix, entry["ue_size"])
            sizes.append(max(abs(expected_size[axis] - float(entry["maya_size"][axis]))
                             for axis in range(3)))
        offset_error = _offset_error(matrix, entry)
        if offset_error is not None:
            offsets.append(offset_error)
        centroid_error = _centroid_error(matrix, entry)
        if centroid_error is not None:
            centroids.append(centroid_error)
        error = _orientation_error(matrix, entry)
        if error is not None:
            orientations.append(error)
    return {
        "name": name,
        "note": note,
        "matrix": matrix3_values(matrix),
        "determinant": matrix3_determinant(matrix),
        "objects_compared": len(usable),
        "max_position_error_cm": max(positions) if positions else None,
        "max_size_error_cm": max(sizes) if sizes else None,
        "max_offset_error_cm": max(offsets) if offsets else None,
        "offset_objects": len(offsets),
        "max_centroid_error_cm": max(centroids) if centroids else None,
        "centroid_objects": len(centroids),
        "max_orientation_error_deg": max(orientations) if orientations else None,
        "orientation_objects": len(orientations),
        "index": index,
    }


def _offset_error(matrix, entry):
    """How far the pivot to bounds-centre vector is from the mapped expectation.

    The expectation is the manifest's own offset carried through the map, so a
    mirror of the geometry -- which leaves a symmetric mesh's world position and
    box size untouched -- shows up here. It needs no local axis to be
    unambiguous, so it is computed even when the orientation check is skipped.
    """
    if not entry.get("ue_offset") or not entry.get("maya_offset"):
        return None
    expected = matrix3_apply(matrix, entry["ue_offset"])
    measured = entry["maya_offset"]
    return vector_length(tuple(expected[axis] - float(measured[axis]) for axis in range(3)))


def _within_tolerance(score, limits):
    """Whether a scored map places every matched object inside the tolerances."""
    if score is None or score["objects_compared"] <= 0:
        return False
    if score["max_position_error_cm"] is None or \
            score["max_position_error_cm"] > limits["position_cm"]:
        return False
    if score["max_size_error_cm"] is not None and \
            score["max_size_error_cm"] > limits["size_cm"]:
        return False
    if score["max_offset_error_cm"] is not None and \
            score["max_offset_error_cm"] > limits["offset_cm"]:
        return False
    if score["max_centroid_error_cm"] is not None and \
            score["max_centroid_error_cm"] > limits["centroid_cm"]:
        return False
    if score["max_orientation_error_deg"] is not None and \
            score["max_orientation_error_deg"] > limits["orientation_deg"]:
        return False
    return True


def _centroid_error(matrix, entry):
    """How far the mesh's world surface centroid is from its mapped expectation.

    The centroid is the decisive mirror signal for a mesh whose bounding box is
    centred on its pivot: a mirror moves the surface even though the position,
    the box size and the box centre all stay where they were. It is reported as
    unavailable -- not as a failure -- for a manifest that carries no centroid.
    """
    if not entry.get("ue_centroid") or not entry.get("maya_centroid"):
        return None
    expected = matrix3_apply(matrix, entry["ue_centroid"])
    measured = entry["maya_centroid"]
    return vector_length(tuple(expected[axis] - float(measured[axis])
                               for axis in range(3)))


def _candidate_order(score):
    return (score["max_position_error_cm"] if score["max_position_error_cm"] is not None
            else float("inf"),
            score["max_size_error_cm"] if score["max_size_error_cm"] is not None
            else float("inf"),
            score["max_offset_error_cm"] if score["max_offset_error_cm"] is not None
            else float("inf"),
            score["max_centroid_error_cm"] if score["max_centroid_error_cm"] is not None
            else float("inf"),
            score["index"])


def _select_winner(scored, fitted, limits):
    """``(score, source)`` of the map the comparison decides with.

    A named candidate inside tolerance wins, because that names the measured
    convention instead of describing it numerically. The fitted map decides only
    when no named candidate fits, and then only while it is a signed permutation
    inside tolerance; otherwise the best scoring named candidate is reported so
    the mismatches can be described against a concrete map.
    """
    named = sorted([entry for entry in scored if entry["objects_compared"] > 0],
                   key=_candidate_order)
    for score in named:
        if _within_tolerance(score, limits):
            return score, "candidate"
    if _within_tolerance(fitted, limits):
        return fitted, "fit"
    if named:
        return named[0], "candidate"
    if fitted is not None:
        return fitted, "fit"
    return None, None


def _camera_contract(scored, limits):
    """How the camera route's map scores on this handoff, and that it differs."""
    camera = None
    for score in scored:
        if score["name"] == CAMERA_SYNC_CANDIDATE:
            camera = score
            break
    block = {
        "candidate": CAMERA_SYNC_CANDIDATE,
        "matrix": None,
        "max_position_error_cm": None,
        "max_size_error_cm": None,
        "max_offset_error_cm": None,
        "max_centroid_error_cm": None,
        "max_orientation_error_deg": None,
        "objects_compared": 0,
        "within_tolerance": False,
        "note": ("the geometry handoff and the camera route use different Unreal to "
                 "Maya conventions; a product that carries both must reconcile them, "
                 "and this prototype does not correct the imported geometry"),
    }
    if camera is not None:
        block.update({
            "matrix": camera["matrix"],
            "max_position_error_cm": camera["max_position_error_cm"],
            "max_size_error_cm": camera["max_size_error_cm"],
            "max_offset_error_cm": camera["max_offset_error_cm"],
            "max_centroid_error_cm": camera["max_centroid_error_cm"],
            "max_orientation_error_deg": camera["max_orientation_error_deg"],
            "objects_compared": camera["objects_compared"],
            "within_tolerance": _within_tolerance(camera, limits),
        })
    return block


def _orientation_error(matrix, entry):
    """Worst axis angle for one object, or ``None`` when it cannot be measured."""
    if not entry.get("local_size") or not orientation_measurable(entry["local_size"]):
        return None
    if not entry.get("ue_matrix") or not entry.get("maya_matrix"):
        return None
    # The rows of a stored linear part are the object's axis images.
    expected = matrix4_linear(carry_world_matrix(matrix, entry["ue_matrix"]))
    measured = matrix4_linear(entry["maya_matrix"])
    errors = [angle_between_degrees(expected_axis, measured_axis)
              for expected_axis, measured_axis in zip(expected, measured)]
    errors = [error for error in errors if error is not None]
    return max(errors) if errors else None


def _object_report(entry, position_error=None, size_error=None, expected_position=None,
                   measured_position=None, expected_size=None, offset_error=None,
                   expected_offset=None, centroid_error=None, expected_centroid=None,
                   orientation_error=None, orientation_checked=False):
    return {
        "node_name": entry.get("node_name"),
        "path": entry.get("path"),
        "matched_id": entry.get("matched_id"),
        "found": bool(entry.get("found")),
        "matched_by": entry.get("matched_by"),
        "match_distance_cm": entry.get("match_distance_cm"),
        "match_candidate": entry.get("match_candidate"),
        "ambiguous_matches": entry.get("ambiguous_matches") or [],
        "world_matrix": entry.get("maya_matrix"),
        "world_bounds_size_cm": _size_text(entry.get("maya_size")),
        "local_bounds_size_cm": _size_text(entry.get("local_size")),
        "position_error_cm": position_error,
        "size_error_cm": size_error,
        "offset_error_cm": offset_error,
        "expected_offset_cm": _size_text(expected_offset),
        "measured_offset_cm": _size_text(entry.get("maya_offset")),
        "centroid_error_cm": centroid_error,
        "expected_centroid_cm": _size_text(expected_centroid),
        "measured_centroid_cm": _size_text(entry.get("maya_centroid")),
        "centroid_checked": bool(entry.get("ue_centroid") and entry.get("maya_centroid")),
        "centroid_faces_used": entry.get("centroid_faces_used"),
        "centroid_triangles_used": entry.get("centroid_triangles_used"),
        "centroid_sampled": bool(entry.get("centroid_sampled")),
        "centroid_note": entry.get("centroid_note"),
        "orientation_error_deg": orientation_error,
        "orientation_checked": bool(orientation_checked),
        "expected_position_cm": _size_text(expected_position, keys=("x", "y", "z")),
        "measured_position_cm": _size_text(measured_position, keys=("x", "y", "z")),
        "expected_size_cm": _size_text(expected_size),
        "problems": [],
    }


def _size_text(values, keys=("x", "y", "z")):
    if not values:
        return None
    return {key: float(values[index]) for index, key in enumerate(keys)}


def _format_vector(vector):
    return ", ".join("{0:.4f}".format(float(value)) for value in vector)
