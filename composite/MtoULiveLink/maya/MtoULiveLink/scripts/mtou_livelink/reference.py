"""Unreal -> Maya scene reference handoff: validation, import and comparison.

Two layers live here. The upper one is pure Python and never imports Maya: the
``mtou-scene-ref-manifest/1`` reader, the FBX texture and media detector both
hosts share, the axis candidates, the world-position fit and the per-object
comparison math ported from the scene-reference prototype (``composite/
MtoULiveLink/prototypes/scene-reference``); :func:`inspect` reads a handoff
directory and decides whether it may be imported. The lower one is the Maya host
adapter: :func:`import_handoff` stages the handoff into a free
``<container>_Incoming`` namespace, verifies it against the manifest and only
then lets the staging namespace take the container's name, with the previous
owned reference renamed aside so a failure anywhere before the takeover is
proven can put it back. :class:`ReferenceContainer` removes or recovers the
tool-owned container, and :class:`ReferenceRefused` carries the frozen refusal
codes of ``protocol/v11.md``.

Conventions (identical to the prototype)
----------------------------------------
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
because the engine's FBX level export and the camera sync *prototype* use
different Unreal to Maya maps: the handoff map is ``maya = (ue_x, ue_z, ue_y)``
(determinant -1), the prototype documented ``maya = (ue_y, ue_z, -ue_x)``. The
product's linked camera uses the handoff's map, the v11 common space
``C(x, y, z) = (x, z, y)``, applied to a camera expressed relative to the
anchor, so the prototype's map is only a diagnostic: the report scores it on
the same handoff in ``transform_check.camera_contract`` so the divergence is on
the record.

The orientation comparison is *not* scored per candidate: a node matrix is not a
point, and the file writes each node in the file's own local frame, so the
expectation is ``frame . L_ue . handoff_map`` (:func:`expected_node_matrix`) with
the frame factor measured for that export option (:data:`ENGINE_HANDOFF_LOCAL_FRAME`,
reported as ``transform_check.node_frame_factor``). It answers whether the file
wrote the frames the convention promises, which is a property of the handoff;
a handoff whose export option has no measured factor reports the check as
unavailable. :func:`world_conversion_matrix` composes the two named worlds into
one Maya to Maya map (determinant ``+1``), which is what the caller applies when
it asks for the other world.

FBX records
-----------
The handoff may carry material assignments, texture records and recorded texture
paths; what it may not deliver is image data, and the scan is what tells the two
apart. :func:`scan_fbx_texture_records` implements the frozen detector both hosts
share: ``Texture:`` opens a texture record, ``Video:`` opens a video record, and
the record ends when the brace depth returns to zero (the SDK writes a nested
``Properties70`` block before the name and ``Content`` lines, so a nested closing
brace ends nothing). Inside a record, ``FileName:``/``Filename:``/
``RelativeFilename:`` record the last quoted token on the line, and a ``Content:``
line whose payload sits on the same line or on the following quoted line is an
embedded media record. A ``NodeAttribute:`` line whose quoted tokens include the
class ``Camera`` or ``Light`` counts as a camera or light record; the SDK writes
no ``Camera:``/``Light:`` record head at all.

The scan reports the texture counts (``texture_records``, ``texture_references``,
``video_references``), the media counts (``content_records``,
``embedded_media_records``), ``camera_records``, ``light_records`` and the
distinct ``files`` in the order found. Only the media counts, together with the
image files the handoff directory holds (:func:`image_files_beside`), refuse a
run; the texture counts are information.

Binary FBX cannot be read this way; the scan falls back to a documented
heuristic over the FBX string table (including embedded media as a ``Content``
node with a raw byte array), reports ``binary: True`` and sets
``media_heuristic``, so the caller can judge the evidence.
"""

import ctypes
import hashlib
import json
import math
import os
import re
import time
import traceback

from .config import (
    MAX_REFERENCE_BYTES,
    MAX_REFERENCE_OBJECTS,
    MAX_REFERENCE_TRIANGLES,
    REFERENCE_ANCHOR_ATTRIBUTE,
    REFERENCE_ANCHOR_TOLERANCE_CM,
    REFERENCE_ANCHOR_TOLERANCE_DEG,
    REFERENCE_CONTAINER,
    REFERENCE_GRAY_SHADER,
    REFERENCE_MARK_ATTRIBUTE,
    REFERENCE_MARK_VALUE,
)
from .mayaapi import cmds, om

MANIFEST_SCHEMA = "mtou-scene-ref-manifest/1"
REPORT_SCHEMA = "mtou-scene-ref-report/1"

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

#: The camera sync prototype's Unreal to Maya map. The product's camera does not
#: use it: v11 maps every camera and reference point with ``C(x, y, z) =
#: (x, z, y)`` after expressing it relative to the anchor. It stays as the
#: diagnostic ``transform_check.camera_contract`` scores.
CAMERA_SYNC_CANDIDATE = "maya_x=ue_y, maya_y=ue_z, maya_z=-ue_x"

#: The map the engine's FBX level export round-trips through, measured on
#: 2026-09-28 with Unreal 5.7.4 and Maya 2024: the engine writes the node
#: translation with Y negated and Maya converts the file's Z-up axis system to
#: Y-up, so an Unreal world point (x, y, z) arrives at (x, z, y). Its
#: determinant is -1, and it is the v11 common space ``C`` the linked camera
#: also uses; the prototype's map above is its mirror.
ENGINE_HANDOFF_CANDIDATE = "maya_x=ue_x, maya_y=ue_z, maya_z=ue_y"

#: Reported as ``best`` when no named candidate places the objects inside
#: tolerance but the least-squares fit does.
FITTED_CANDIDATE_NAME = "fitted signed permutation"

#: The local frame factor the engine's FBX level export writes into every node.
#:
#: The file's node matrices are not ``L . map``: the exporter writes each node in
#: the file's own local frame, so a node's Maya matrix is
#: ``frame . L_ue . map``. The factor was measured on 2026-09-29 over the ten
#: object fixture handoff (Unreal 5.7.4, ``bForceFrontXAxis = false``, read back
#: by Maya 2024 ``fbxmaya`` 2020.3.4): every measured node matrix matches
#: ``frame . L_ue . map`` to ``1.3e-15``, while ``L_ue . map^T`` is off by up to
#: ``7.7``. It is a mirror in the file's second axis, which the Maya reader
#: applies to each node's local space as an up-axis conversion, and it is the
#: reason the node matrix carries a positive determinant while the point map
#: does not.
ENGINE_HANDOFF_LOCAL_FRAME = ((1.0, 0.0, 0.0),
                              (0.0, -1.0, 0.0),
                              (0.0, 0.0, 1.0))

#: Export axis options whose local frame factor has been measured. A handoff
#: that declares another option reports the orientation check as unavailable
#: instead of comparing against a factor it never measured.
MEASURED_ASYMMETRIC_FRAME_OPTIONS = ("bForceFrontXAxis=false",)

#: The world the reference geometry can be placed in.
#:
#: ``engine`` is the world the engine's FBX level export round-trips through
#: (``ENGINE_HANDOFF_CANDIDATE``). It is also the world of the product's Maya to
#: Unreal animation route: ``MtoULiveLink.py``'s ``convert_transform`` maps
#: ``maya (x, y, z) -> ue (x, z, y)``, which is its own inverse and the same map
#: as the handoff, so a level reference imported without conversion agrees with
#: the character and prop animation the artist sends to Unreal (checked
#: 2026-09-29).
#:
#: ``camera`` is the camera sync prototype's world (``CAMERA_SYNC_CANDIDATE``);
#: the two differ by a rotation, and moving the reference into it is an explicit
#: conversion the caller asks for. The product's linked camera lives in the
#: ``engine`` world, relative to the anchor.
WORLD_ENGINE = "engine"
WORLD_CAMERA = "camera"
WORLD_TARGETS = (
    (WORLD_ENGINE, ENGINE_HANDOFF_CANDIDATE,
     "the engine FBX level handoff's own world, shared with the product's "
     "Maya to Unreal animation route"),
    (WORLD_CAMERA, CAMERA_SYNC_CANDIDATE,
     "the camera sync prototype's world, kept for comparison; the linked camera "
     "uses the engine world relative to the anchor"),
)

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
        "note": "the camera sync prototype's Unreal to Maya map, not the product "
                "camera's: determinant -1",
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

#: The FBX class token a camera or light node attribute carries. Measured on an
#: ASCII FBX written by this host's ``fbxmaya`` 2020.3.4: cameras and lights are
#: ``NodeAttribute: <id>, "NodeAttribute::<name>", "Camera"|"Light" {`` records,
#: not ``Camera:``/``Light:`` records.
_NODE_ATTRIBUTE_HEAD = "NodeAttribute:"
_CAMERA_CLASS = "Camera"
_LIGHT_CLASS = "Light"

#: The record property that carries embedded media data. The SDK writes it only
#: when media is embedded, with the payload as a quoted string on the same line
#: or on the following non-empty line.
_CONTENT_HEAD = "Content:"

#: Recorded file name properties the walk reads inside a record. The Texture
#: record uses ``FileName`` while the Video record uses ``Filename``; both are
#: recorded, as the FBX SDK writes them side by side.
_FILE_NAME_HEADS = ("FileName:", "Filename:", "RelativeFilename:")

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
    """Material media records of an ASCII FBX document.

    The walk is the contract's own rule, frozen with the Unreal exporter's
    detector so both hosts count the same file identically:

    * a line whose trimmed text starts with ``Texture:`` opens a texture record;
    * a line whose trimmed text starts with ``Video:`` opens a video record;
    * a line that is exactly ``}`` closes the current record;
    * inside a record, a line starting with ``FileName:``, ``Filename:`` or
      ``RelativeFilename:`` records the last quoted token on that line;
    * inside a record, a line starting with ``Content:`` is an embedded media
      record when the line itself carries data or the next non-empty line starts
      with a quoted payload (measured: this host's ``fbxmaya`` writes
      ``Content: ,`` followed by the base64 payload on the next line, and writes
      no ``Content:`` line at all for media that is not embedded);
    * a line starting with ``NodeAttribute:`` whose quoted tokens include the
      class ``Camera`` or ``Light`` is a camera or light node record, which a
      static reference handoff must not carry.

    A texture record is not a refusal reason: the entry may carry material
    assignments, and a recorded file name is a reference, not a copied file. The
    facts a refusal is based on are the embedded media records and the image
    files the handoff directory holds (the caller checks the directory).
    """
    texture_records = []
    video_records = []
    files = []
    embedded_media = []
    content_records = 0
    camera_records = 0
    light_records = 0
    current = None
    depth = 0
    pending_content = None
    for number, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        if not line:
            continue
        if pending_content is not None:
            # The payload of a ``Content:`` line: a quoted base64 string on the
            # line that follows it. A line that is not a payload belongs to the
            # record and is handled below.
            if line.startswith('"'):
                payload = line.strip('"')
                pending_content["payload_characters"] = len(payload)
                embedded_media.append(pending_content)
                pending_content = None
                continue
            pending_content = None
        if line == "}":
            # Only the brace that closes the record itself ends the walk: the
            # SDK writes a nested ``Properties70`` block before the name and
            # ``Content`` lines, so a nested closing brace ends nothing.
            if current is not None:
                depth -= 1
                if depth <= 0:
                    current = None
                    depth = 0
            continue
        if line.startswith(_TEXTURE_RECORD_HEAD):
            current = {"kind": "Texture", "line": number, "header": line,
                       "file_names": []}
            depth = 1
            texture_records.append(current)
            continue
        if line.startswith(_VIDEO_RECORD_HEAD):
            current = {"kind": "Video", "line": number, "header": line,
                       "file_names": []}
            depth = 1
            video_records.append(current)
            continue
        if line.startswith(_NODE_ATTRIBUTE_HEAD):
            classes = _QUOTED_RE.findall(line)
            if _CAMERA_CLASS in classes:
                camera_records += 1
            if _LIGHT_CLASS in classes:
                light_records += 1
            continue
        if current is None:
            continue
        if line.endswith("{"):
            depth += 1
        if line.startswith(_CONTENT_HEAD):
            content_records += 1
            remainder = line[len(_CONTENT_HEAD):].strip().strip(",").strip()
            record = {"kind": current["kind"], "line": number,
                      "header": current["header"], "payload_characters": 0,
                      "same_line": bool(remainder)}
            if remainder:
                record["payload_characters"] = len(remainder.strip('"'))
                embedded_media.append(record)
            else:
                pending_content = record
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
        "content_records": content_records,
        "embedded_media_records": len(embedded_media),
        "embedded_media": embedded_media,
        "camera_records": camera_records,
        "light_records": light_records,
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


def scan_fbx_binary_media(raw):
    """Best-effort embedded media and camera/light records of a binary FBX.

    Embedded media is an ``Content`` node whose value is an FBX raw byte array
    (property type ``R``) with a non-zero length, inside a ``Texture`` or
    ``Video`` object. Camera and light node attributes are counted from the
    class tokens ``Camera`` and ``Light`` in the byte stream. Both are string
    table heuristics on a format this contract does not accept as a handoff, so
    the caller reports them as heuristic evidence.
    """
    embedded = []
    marker = _CONTENT_HEAD[:-1].encode("ascii")
    for match in re.finditer(re.escape(marker), raw):
        start = match.start()
        if raw[start - 1:start] != bytes((len(marker),)):
            continue
        kind, node = _binary_enclosing_node(raw, start)
        if kind not in ("Texture", "Video"):
            continue
        index = match.end()
        if raw[index:index + 1] != b"R":
            continue
        length = int.from_bytes(raw[index + 1:index + 5], "little")
        if length <= 0:
            continue
        embedded.append({"kind": kind, "node": node, "line": None,
                         "header": "{0} node {1}".format(kind, node),
                         "payload_characters": length, "same_line": False})
    classes = []
    for name in (_CAMERA_CLASS, _LIGHT_CLASS):
        token = name.encode("ascii")
        count = 0
        for match in re.finditer(re.escape(token), raw):
            start = match.start()
            if raw[start - 1:start] == bytes((len(token),)):
                count += 1
        classes.append(count)
    return embedded, classes[0], classes[1]


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
    ``embedded_media_records`` and ``camera_records``/``light_records`` are the
    facts a static reference handoff is judged on; a texture record is
    informational. ``format`` is ``unknown`` for a file that is not FBX at all,
    and a binary scan is reported as heuristic evidence (``media_heuristic``)
    rather than as proof.
    """
    sniffed = sniff_fbx_file(path)
    scan = {"source": str(path), "exists": sniffed["exists"],
            "readable": sniffed["readable"], "bytes": sniffed["bytes"],
            "format": sniffed["format"], "magic": sniffed["magic"],
            "error": sniffed["error"], "binary": sniffed["format"] == "binary",
            "texture_records": 0, "texture_references": 0, "video_references": 0,
            "content_records": 0, "embedded_media_records": 0,
            "embedded_media": [], "media_heuristic": False,
            "camera_records": 0, "light_records": 0,
            "files": [], "texture_files": [], "video_files": [], "image_files": [],
            "image_references": 0, "records": [], "video_records": [],
            "allowed": False, "note": ""}
    if not sniffed["readable"]:
        return scan
    if sniffed["format"] == "ascii":
        scan.update(scan_fbx_texture_records(read_fbx_text(path)))
        scan["note"] = "ascii FBX: texture, media and node attribute records parsed"
    elif sniffed["format"] == "binary":
        with open(path, "rb") as handle:
            raw = handle.read()
        records = scan_fbx_binary_records(raw)
        embedded, cameras, lights = scan_fbx_binary_media(raw)
        names = _unique([name for record in records for name in record["file_names"]])
        scan["records"] = records
        scan["texture_records"] = len(records)
        scan["texture_references"] = len([record for record in records
                                          if record["file_names"]])
        scan["video_references"] = 0
        scan["embedded_media"] = embedded
        scan["embedded_media_records"] = len(embedded)
        scan["camera_records"] = cameras
        scan["light_records"] = lights
        scan["media_heuristic"] = True
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


def image_files_beside(path):
    """Image files the handoff's own directory holds, sorted by name.

    A static reference handoff must not deliver image data, and the directory
    the exporter owns is the place a copied or baked image would land. The
    caller compares this list with the exporter's own ``output.image_files``.
    """
    directory = os.path.dirname(os.path.abspath(path))
    try:
        names = os.listdir(directory)
    except OSError:
        return []
    return sorted(name for name in names if is_image_file_name(name))


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
    """Scope, world, conventions and scale summary the importer reports."""
    world = manifest.get("world") if isinstance(manifest.get("world"), dict) else {}
    scope = manifest.get("scope") if isinstance(manifest.get("scope"), dict) else {}
    scale = manifest.get("scale") if isinstance(manifest.get("scale"), dict) else {}
    conventions = manifest.get("conventions")
    if not isinstance(conventions, dict):
        conventions = None
    component_filter = manifest.get("filter")
    if not isinstance(component_filter, dict):
        component_filter = None
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
        "conventions": conventions,
        "filter": component_filter,
        "scope": {
            "kind": scope.get("kind"),
            "persistent_level": scope.get("persistent_level"),
            "coverage": scope.get("coverage"),
            "completeness": scope.get("completeness"),
            "coverage_note": scope.get("coverage_note"),
            "world_partition": _scope_world_partition(scope.get("world_partition")),
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


def _scope_world_partition(value):
    """The manifest's World Partition inventory block, or ``None``.

    The block is what tells a reader how complete a partitioned scope is: the
    resolver counts the actor descriptors of the partition, says how many of them
    the loaded world actually spawned, and names the ones it did not. A manifest
    written before this block existed carries none, which is reported as
    unavailable rather than read as complete.
    """
    if not isinstance(value, dict):
        return None
    return {
        "detected": value.get("detected"),
        "inventory_available": value.get("inventory_available"),
        "containers": value.get("containers"),
        "actor_descriptors": value.get("actor_descriptors"),
        "loaded_actor_descriptors": value.get("loaded_actor_descriptors"),
        "unloaded_actor_count": value.get("unloaded_actor_count"),
        "unloaded_hlod_count": value.get("unloaded_hlod_count"),
        "unloaded_actors": value.get("unloaded_actors") or [],
        "inventory_truncated": value.get("inventory_truncated"),
        "note": value.get("note"),
    }


def scope_coverage_warning(summary):
    """A warning line when the manifest's scope is not stated as complete.

    A World Partition world streams actors through cells, so a level range of a
    partitioned world holds the actors the loaded cells have spawned. The exporter
    decides completeness from the partition's own actor descriptors and puts that
    decision in the manifest (``scope.completeness``, with ``scope.world_partition``
    carrying the inventory behind it); a manifest that says ``not_confirmed`` -- or
    that predates the block and marks the world partitioned -- is reported here
    instead of being read as a complete level.
    """
    scope = (summary or {}).get("scope") or {}
    world = (summary or {}).get("world") or {}
    completeness = scope.get("completeness")
    partition = scope.get("world_partition") or {}
    if completeness == "confirmed":
        return None
    partitioned = bool(world.get("world_partition")) or bool(partition.get("detected"))
    if completeness is None and not partitioned:
        return None
    detail = ""
    if partition:
        detail = (" (actor descriptors {0}, spawned {1}, authored actors not spawned {2}, "
                  "generated HLOD proxies not spawned {3}{4})").format(
            partition.get("actor_descriptors"),
            partition.get("loaded_actor_descriptors"),
            partition.get("unloaded_actor_count"),
            partition.get("unloaded_hlod_count"),
            ", recorded list capped" if partition.get("inventory_truncated") else "")
    elif partitioned:
        detail = " (the manifest carries no descriptor inventory)"
    return ("SCOPE_LOADED_ONLY: the manifest's scope is not confirmed complete{0}; "
            "the transfer carries only the content the loaded levels spawned and "
            "never loads anything on its own, so the level range may be "
            "incomplete".format(detail))


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
    """A Maya node matrix carried through ``candidate`` (``maya -> maya``).

    The candidate is a point map, so carrying a transform composes it with the
    transposed map: the result's rows are the Maya images of the object's local
    axes and its fourth row is the mapped position. This is the operation the
    importer applies when it moves an imported level into another world; it is
    *not* the relation between an Unreal world matrix and the node matrix the
    handoff writes, which is :func:`expected_node_matrix`.
    """
    rows = matrix3(candidate)
    linear = matrix4_linear(world_matrix)
    carried = matrix3_multiply(linear, matrix3_transpose(rows))
    position = matrix3_apply(rows, matrix4_translation(world_matrix))
    return matrix4_from_linear_and_translation(carried, position)


def expected_node_matrix(candidate, world_matrix, frame=ENGINE_HANDOFF_LOCAL_FRAME,
                         conversion=None):
    """The node matrix a handoff writes for an Unreal world matrix.

    ``frame . L_ue . candidate``, with the position mapped by ``candidate`` and
    then by ``conversion`` when the geometry was moved into another Maya world.
    ``frame`` is the local frame factor the file's own axis convention implies
    (:data:`ENGINE_HANDOFF_LOCAL_FRAME`); ``None`` reports the expectation as
    unavailable rather than comparing against a factor that was never measured.
    """
    if frame is None:
        return None
    rows = matrix3(candidate)
    linear = matrix3_multiply(matrix3_multiply(matrix3(frame), matrix4_linear(world_matrix)),
                              rows)
    position = matrix3_apply(rows, matrix4_translation(world_matrix))
    if conversion is not None:
        rows = matrix3(conversion)
        linear = matrix3_multiply(linear, matrix3_transpose(rows))
        position = matrix3_apply(rows, position)
    return matrix4_from_linear_and_translation(linear, position)


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


def parse_anchor(value):
    """The anchor a manifest's ``world.anchor`` names, or the reason it is unusable.

    ``value`` is ``{"location_cm": [x, y, z], "rotation_deg": {"roll", "pitch",
    "yaw"}}``: the paired target's placement in Unreal world axes (Unreal's
    ``FRotator``: yaw about Z, pitch about Y, roll about X). Returns
    ``(anchor, None)`` with the anchor's unit axes (its forward, right and up in
    Unreal world axes), ``(None, None)`` when the manifest names no anchor, or
    ``(None, reason)``.
    """
    if value is None:
        return None, None
    if not isinstance(value, dict):
        return None, "the manifest's world.anchor is not an object"
    try:
        location = tuple(float(entry) for entry in value["location_cm"])
        rotation = value["rotation_deg"]
        roll, pitch, yaw = (float(rotation[name])
                            for name in ("roll", "pitch", "yaw"))
    except (KeyError, TypeError, ValueError):
        return None, ("the manifest's world.anchor needs location_cm and "
                      "rotation_deg with roll, pitch and yaw")
    if len(location) != 3:
        return None, "the manifest's world.anchor location_cm needs three numbers"
    if any(not math.isfinite(entry) for entry in location + (roll, pitch, yaw)):
        return None, "the manifest's world.anchor holds a non-finite number"
    cr, sr = math.cos(math.radians(roll)), math.sin(math.radians(roll))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    axes = ((cp * cy, cp * sy, sp),
            (sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp),
            (-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp))
    return {"location_cm": location, "axes": axes}, None


#: The anchor of a manifest without ``world.anchor``: the Maya origin frame.
IDENTITY_ANCHOR = {"location_cm": [0.0, 0.0, 0.0],
                   "rotation_deg": {"roll": 0.0, "pitch": 0.0, "yaw": 0.0}}


def anchor_difference(recorded, current,
                      tolerance_cm=REFERENCE_ANCHOR_TOLERANCE_CM,
                      tolerance_deg=REFERENCE_ANCHOR_TOLERANCE_DEG):
    """Why a reference placed for ``recorded`` does not fit ``current``, or None.

    Both are ``world.anchor`` shaped values. The positions must agree within
    ``tolerance_cm`` and every unit axis within ``tolerance_deg``, the same
    tolerances that bind the targets of one set to one placement.
    """
    placed, problem = parse_anchor(recorded)
    if problem or placed is None:
        return "the reference records no usable anchor ({0})".format(
            problem or "none")
    wanted, problem = parse_anchor(current)
    if problem or wanted is None:
        return "the linkage names no usable anchor ({0})".format(problem or "none")
    distance = math.sqrt(sum(
        (placed["location_cm"][index] - wanted["location_cm"][index]) ** 2
        for index in range(3)))
    angle = max(
        math.degrees(math.acos(max(-1.0, min(1.0, sum(
            left[index] * right[index] for index in range(3))))))
        for left, right in zip(placed["axes"], wanted["axes"]))
    if distance <= tolerance_cm and angle <= tolerance_deg:
        return None
    return ("the reference was placed for the anchor at ({0}) and the linkage "
            "target now stands at ({1}): {2:.3f} cm and {3:.3f} deg apart").format(
                ", ".join("{0:g}".format(value) for value in placed["location_cm"]),
                ", ".join("{0:g}".format(value) for value in wanted["location_cm"]),
                distance, angle)


def anchor_container_matrix(anchor):
    """The Maya matrix that moves the reference container into the anchor frame.

    The staged reference sits in Maya world space, ``maya = C . ue`` with
    ``C(x, y, z) = (x, z, y)``. The character and the linked camera live in the
    anchor frame: ``ue_local = A^-1 . ue``. The container therefore takes
    ``C . A^-1 . C^-1``. Maya matrices are row-major with the translation in the
    fourth row, so the returned 16 values act on row vectors.
    """
    swap = matrix3(candidate_matrix_by_name(ENGINE_HANDOFF_CANDIDATE))
    inverse_linear = matrix3(anchor["axes"])
    linear = matrix3_multiply(matrix3_multiply(swap, inverse_linear), swap)
    shift = matrix3_apply(linear, matrix3_apply(swap, anchor["location_cm"]))
    return matrix4_from_linear_and_translation(
        matrix3_transpose(linear), tuple(-value for value in shift))


def vector_length(vector):
    return math.sqrt(sum(float(value) * float(value) for value in vector))


def candidate_matrix_by_name(name):
    """The point map of the named axis candidate (``maya = M . ue``)."""
    for candidate in AXIS_CANDIDATES:
        if candidate["name"] == name:
            return matrix3(candidate["matrix"])
    raise ValueError("no axis candidate named {0!r}".format(name))


def world_target_map(world):
    """The axis candidate name that places geometry in ``world``."""
    for name, candidate, _note in WORLD_TARGETS:
        if name == world:
            return candidate
    raise ValueError(
        "unknown world {0!r}; the worlds are {1}".format(
            world, ", ".join(name for name, _candidate, _note in WORLD_TARGETS)))


def world_target_note(world):
    """The explanatory note of one world target."""
    for name, _candidate, note in WORLD_TARGETS:
        if name == world:
            return note
    raise ValueError("unknown world {0!r}".format(world))


def world_conversion_matrix(from_world, to_world):
    """The Maya to Maya map that moves geometry from one world into another.

    ``from_world`` maps Unreal points onto Maya points as ``p_maya = S . p_ue``
    and ``to_world`` as ``p_maya = T . p_ue``, so the conversion applied inside
    Maya is ``A = T . S^-1``. Both named maps have determinant ``-1``, so ``A``
    is a proper rotation (determinant ``+1``) and the geometry keeps its
    handedness; the returned ``(matrix, determinant)`` lets the caller report
    that instead of asserting it.
    """
    source = candidate_matrix_by_name(world_target_map(from_world))
    target = candidate_matrix_by_name(world_target_map(to_world))
    inverse = matrix3_inverse(source)
    if inverse is None:
        raise ValueError("the {0!r} map is singular".format(from_world))
    matrix = matrix3_multiply(target, inverse)
    return matrix, matrix3_determinant(matrix)


def matrix3_rotation_angle_degrees(matrix):
    """Angle of a proper rotation, or ``None`` for a matrix that is not one.

    The trace of a rotation is ``1 + 2 cos(angle)``; a matrix whose determinant
    is not ``+1`` describes a reflection or a scale, which has no single angle.
    """
    rows = matrix3(matrix)
    determinant = matrix3_determinant(rows)
    if abs(determinant - 1.0) > 1e-6:
        return None
    trace = sum(rows[axis][axis] for axis in range(3))
    cosine = max(-1.0, min(1.0, (trace - 1.0) / 2.0))
    return math.degrees(math.acos(cosine))


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

    A mesh whose identification size has three distinct extents pins its axes.
    The caller passes that size -- the shape's local bounding-box extents scaled
    by the node's own axis scale factors -- because a non-uniform scale makes
    axes distinguishable that a symmetric mesh alone would leave ambiguous: a
    cube scaled ``(2, 0.5, 0.25)`` has images of different lengths on all three
    axes, so a map that permutes them changes the geometry and has to be
    reported. For anything else a symmetry rotation can produce the same
    geometry with a different node matrix, so the orientation comparison is
    reported as skipped rather than as passed.
    """
    if not local_size or len(local_size) != 3:
        return False
    values = sorted(float(value) for value in local_size)
    largest = values[-1]
    if largest <= 0.0:
        return False
    return all((values[index + 1] - values[index]) / largest > relative_tolerance
               for index in range(2))


def compare_measurements(candidates, measurements, tolerances=None,
                         frame=ENGINE_HANDOFF_LOCAL_FRAME, conversion=None,
                         handoff_map=ENGINE_HANDOFF_CANDIDATE):
    """Score the axis maps and compare every matched object.

    ``measurements`` holds one entry per exported manifest object:
    ``node_name``, ``matched_id``, ``found``, ``path``, ``matched_by``,
    ``ambiguous_matches``, ``problems``, ``ue_matrix`` (16 Unreal values),
    ``ue_size`` (centimetres), ``ue_offset`` (position to bounds centre),
    ``maya_matrix`` (16 Maya values or ``None``), ``maya_size``,
    ``maya_offset``, ``local_size`` and ``identification_size``.

    ``frame`` is the local frame factor the handoff's axis convention writes
    into every node (:data:`ENGINE_HANDOFF_LOCAL_FRAME`) and ``handoff_map`` is
    the point map that factor was measured with; the orientation check compares
    the measured node matrix against ``frame . ue . handoff_map`` and is
    reported as unavailable when no factor is known. The check is anchored to
    the file's own convention on purpose: it answers whether the file wrote the
    frames the convention promises, not which candidate explains the positions,
    so its error is the same for every scored candidate. ``conversion`` is the
    Maya to Maya map the caller applied to move the geometry into another world;
    a rotated world composes it on the right of the node matrix.

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
    handoff_matrix = candidate_matrix_by_name(handoff_map)
    fit = fit_linear_map([matrix4_translation(entry["ue_matrix"]) for entry in usable],
                         [matrix4_translation(entry["maya_matrix"]) for entry in usable])

    scored = [_score_candidate(candidate["name"], candidate.get("note", ""),
                              matrix3(candidate["matrix"]), usable, index,
                              frame=frame, conversion=conversion,
                              handoff_map=handoff_matrix)
              for index, candidate in enumerate(candidates)]
    fitted = None
    if fit["available"] and fit["signed_permutation"]:
        fitted = _score_candidate(FITTED_CANDIDATE_NAME,
                                  "the least-squares fit of the measured positions, "
                                  "reported in the fit block",
                                  matrix3(fit["matrix"]), usable, len(scored),
                                  frame=frame, conversion=conversion,
                                  handoff_map=handoff_matrix)
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
            orientation_error = _orientation_error(entry, handoff_matrix, frame,
                                                   conversion)
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
        "node_frame_factor": matrix3_values(frame) if frame is not None else None,
        "world_conversion_matrix": (matrix3_values(conversion)
                                    if conversion is not None else None),
        "orientation_available": bool(frame is not None),
        "orientation_note": (
            "the orientation comparison checks each node matrix against "
            "'frame . ue . map' with the handoff's measured local frame factor "
            "and runs for objects whose identification size has three distinct "
            "extents"
            if frame is not None else
            "no local frame factor is known for this handoff's export axis "
            "option, so the orientation comparison is unavailable"),
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


def _score_candidate(name, note, matrix, usable, index,
                     frame=ENGINE_HANDOFF_LOCAL_FRAME, conversion=None,
                     handoff_map=None):
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
        error = _orientation_error(entry, handoff_map, frame, conversion)
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
        "note": ("the camera sync prototype's map differs from the handoff's map, "
                 "which is also the product camera's map C(x, y, z) = (x, z, y); the "
                 "prototype map is a diagnostic and does not correct the imported "
                 "geometry"),
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


def _orientation_error(entry, handoff_map, frame=ENGINE_HANDOFF_LOCAL_FRAME,
                       conversion=None):
    """Worst axis angle for one object, or ``None`` when it cannot be measured.

    The expectation is the node matrix the handoff writes,
    ``frame . ue . handoff_map`` (or its image under ``conversion``, which a
    rotated world composes on the right), so the comparison is against the
    file's own convention rather than against the scored candidate: the frame
    factor is a mirror and would otherwise report every object as 180 degrees
    wrong on one axis. A wrong candidate still shows up in the position, size,
    offset and centroid comparisons, which are the ones a candidate decides.
    """
    identification = entry.get("identification_size") or entry.get("local_size")
    if not identification or not orientation_measurable(identification):
        return None
    if not entry.get("ue_matrix") or not entry.get("maya_matrix"):
        return None
    if frame is None:
        return None
    expected_matrix = expected_node_matrix(handoff_map, entry["ue_matrix"],
                                           frame=frame, conversion=conversion)
    # The rows of a stored linear part are the object's axis images.
    expected = matrix4_linear(expected_matrix)
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
        "category": entry.get("category"),
        "found": bool(entry.get("found")),
        "matched_by": entry.get("matched_by"),
        "match_distance_cm": entry.get("match_distance_cm"),
        "match_candidate": entry.get("match_candidate"),
        "ambiguous_matches": entry.get("ambiguous_matches") or [],
        "world_matrix": entry.get("maya_matrix"),
        "world_bounds_size_cm": _size_text(entry.get("maya_size")),
        "local_bounds_size_cm": _size_text(entry.get("local_size")),
        "identification_size_cm": _size_text(
            entry.get("identification_size") or entry.get("local_size")),
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


# ---------------------------------------------------------------- Maya host

#: The namespace a run stages its import in before it replaces the reference. A
#: run takes the first free name of this family, so a namespace or node of that
#: name that belongs to the scene is never emptied, reused or deleted.
STAGING_SUFFIX = "_Incoming"

#: The name family a takeover moves the previous reference to, so the staged
#: namespace can take the container's name while the old one stays restorable.
RETIRING_SUFFIX = "_Retiring"

#: How many names of one family a run looks for before it refuses to run.
NAME_ATTEMPTS = 32

#: Node types that may sit at the top of an owned container namespace without
#: making it a namespace this tool refuses: the FBX plugin places a 3D texture
#: placement beside the geometry it imports, and such a node is part of the
#: reference, not a scene object of its own.
TOLERATED_CONTAINER_TOP_TYPES = ("place3dTexture",)

#: The shading modes the caller can ask for.
SHADING_DISPLAY = "display"
SHADING_MATERIAL = "material"
SHADING_KEEP = "keep"
SHADING_MODES = (SHADING_DISPLAY, SHADING_MATERIAL, SHADING_KEEP)

#: The gray the uniform display uses.
GRAY_DISPLAY_COLOR = (0.5, 0.5, 0.5)

#: Most faces accumulated per object for the world surface centroid check. Above
#: it the faces are sampled evenly, so a huge handoff cannot turn the check into
#: a hang; ``centroid_faces_used``, ``centroid_triangles_used`` and
#: ``centroid_sampled`` say what happened.
CENTROID_FACE_CAP = 20000

#: Maya time units the importer accepts; it never changes them.
MANAGED_TIME_UNITS = frozenset((
    "game", "film", "pal", "ntsc", "show", "palf", "ntscf", "millisec", "sec",
    "min", "hour",
))

#: The manifest file suffix the handoff directory owns: the exporter writes
#: ``<scope>.manifest.json`` beside the ``<scope>.fbx`` geometry.
MANIFEST_SUFFIX = ".manifest.json"

#: The frozen refusal codes of protocol/v11.md. Only these leave this module as
#: :class:`ReferenceRefused`; every other refusal is carried by a report whose
#: ``problems`` names it, so the panel can render the outcome either way.
CODE_STALE = "REFERENCE_STALE"
CODE_DIGEST_MISMATCH = "REFERENCE_DIGEST_MISMATCH"
CODE_IMAGE_DATA = "REFERENCE_IMAGE_DATA"
CODE_OVER_BUDGET = "REFERENCE_OVER_BUDGET"
CODE_CONTAINER_NOT_OWNED = "CONTAINER_NOT_OWNED"
CODE_RECOVERY_REFUSED = "RECOVERY_REFUSED"

#: Report categories that are not frozen protocol codes. They stay on the record
#: as ``REFUSED <category>:`` problem lines, exactly as the prototype reported
#: them, so a report keeps its reason even where the panel has no localized
#: diagnostic text for the code.
CATEGORY_CONTAINER_NAME = "CONTAINER_NAME_INVALID"
CATEGORY_UNSUPPORTED_SCENE_UNIT = "UNSUPPORTED_SCENE_LINEAR_UNIT"
CATEGORY_FBX_UNREADABLE = "FBX_UNREADABLE"
CATEGORY_FBX_NOT_FBX = "FBX_NOT_FBX"
CATEGORY_FBX_PLUGIN = "FBX_PLUGIN_UNAVAILABLE"
CATEGORY_IMPORT_FAILED = "FBX_IMPORT_FAILED"
CATEGORY_GRAY_MATERIAL = "GRAY_MATERIAL_CONFLICT"
CATEGORY_SWAP_FAILED = "STAGED_SWAP_FAILED"
CATEGORY_ANCHOR_INVALID = "ANCHOR_INVALID"


class ReferenceRefused(RuntimeError):
    """A handoff or a container this module refuses. ``code`` is stable."""

    def __init__(self, code, detail):
        RuntimeError.__init__(self, "{0}: {1}".format(code, detail))
        self.code = code
        self.detail = detail


class _Refusal(Exception):
    """A refusal that is reported instead of raised; ``category`` is stable."""

    def __init__(self, category, detail):
        Exception.__init__(self, "{0}: {1}".format(category, detail))
        self.category = category
        self.detail = detail


def require_maya():
    """Refuse a host call when this module runs outside Maya."""
    if cmds is None or om is None:
        raise RuntimeError("the scene reference importer must run inside Maya")


def _budget(budget):
    """The frozen budgets, filled in from ``config`` when a caller omits them."""
    limits = {"bytes": MAX_REFERENCE_BYTES, "objects": MAX_REFERENCE_OBJECTS,
              "triangles": MAX_REFERENCE_TRIANGLES}
    if isinstance(budget, dict):
        for key in limits:
            value = budget.get(key)
            if isinstance(value, (int, float)) and not isinstance(value, bool):
                limits[key] = int(value)
    return limits


def _text(value):
    return str(value).strip() if value is not None else ""


def _count(value):
    """A count the offer or the manifest declared, or ``None``."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return int(value)


# ------------------------------------------------------------- handoff files

def handoff_files(directory):
    """``{manifest, geometry, ...}`` of one handoff directory.

    The exporter writes one ``<scope>.manifest.json`` and one ``<scope>.fbx``
    into the directory it owns; both are found by name rather than through a
    registry of known scopes, because a handoff is a plain directory on the
    local disk.
    """
    result = {"directory": str(directory), "exists": False, "manifest": None,
              "geometry": [], "error": None}
    try:
        names = sorted(os.listdir(str(directory)))
    except OSError as error:
        result["error"] = "the handoff directory cannot be read: {0}".format(error)
        return result
    result["exists"] = True
    manifests = [name for name in names if name.endswith(MANIFEST_SUFFIX)]
    geometry = [name for name in names if name.lower().endswith(".fbx")]
    if len(manifests) > 1:
        result["error"] = ("the handoff directory holds {0} manifests {1}; a run "
                           "imports exactly one handoff".format(len(manifests),
                                                                manifests))
    elif manifests:
        result["manifest"] = os.path.join(str(directory), manifests[0])
    else:
        result["error"] = ("the handoff directory holds no {0} "
                           "manifest".format(MANIFEST_SUFFIX))
    result["geometry"] = [os.path.join(str(directory), name) for name in geometry]
    return result


def read_handoff_manifest(path):
    """``(manifest, geometry_name, error)`` of one manifest file.

    Unlike :func:`load_manifest` this never raises: the product's exporter
    writes a manifest that may carry no per-object ``objects`` list, so the
    reader reports what is usable and leaves the geometric verification to
    decide what it can compare.
    """
    try:
        with open(str(path), "r", encoding="utf-8") as handle:
            manifest = json.load(handle)
    except OSError as error:
        return None, None, "the manifest cannot be read: {0}".format(error)
    except ValueError as error:
        return None, None, "the manifest is not valid JSON: {0}".format(error)
    if not isinstance(manifest, dict):
        return None, None, "the manifest is not a JSON object"
    files = manifest.get("files") if isinstance(manifest.get("files"), dict) else {}
    return manifest, _text(files.get("geometry")), None


def handoff_digests(directory, manifest=None):
    """The content digests this module can compute for one handoff directory.

    A digest covers the directory's own content, so a handoff that changed after
    it was offered cannot keep the old offer. Several spellings of the field
    exist in this repository: the product exporter hashes the manifest text
    followed by the geometry file's own bytes (``FMD5::HashBytes(ManifestUtf8 +
    GeometryBytes)``), an older spelling used the geometry file's MD5 instead of
    its bytes, and the frozen conformance corpus names the same field
    ``sha256:<hex>``. All are computed here and any one of them matches, so a
    changed directory is refused under every spelling and a handoff written by
    either side imports without a manual digest migration.
    """
    result = {"available": False, "digests": {}, "manifest_bytes": None,
              "geometry_bytes": None, "error": None}
    files = handoff_files(directory)
    manifest_path = manifest if manifest is not None else files["manifest"]
    if not files["exists"] or not manifest_path:
        result["error"] = "no manifest to hash"
        return result
    try:
        with open(str(manifest_path), "rb") as handle:
            manifest_bytes = handle.read()
        result["manifest_bytes"] = len(manifest_bytes)
    except OSError as error:
        result["error"] = "the manifest cannot be read: {0}".format(error)
        return result
    geometry_md5 = None
    geometry_raw = None
    if files["geometry"]:
        try:
            with open(files["geometry"][0], "rb") as handle:
                geometry_raw = handle.read()
                geometry_md5 = hashlib.md5(geometry_raw).hexdigest()
                result["geometry_bytes"] = len(geometry_raw)
        except OSError as error:
            result["error"] = "the geometry cannot be read: {0}".format(error)
    # A byte order mark is written by some hosts and is not part of the text an
    # exporter hashed, and a manifest written with CRLF line endings hashes to a
    # different digest than the same text with LF, so both readings of the
    # manifest are offered and the digest is stable across hosts.
    texts = [manifest_bytes]
    if manifest_bytes.startswith(b"\xef\xbb\xbf"):
        texts.append(manifest_bytes[3:])
    normalised = manifest_bytes.replace(b"\r\n", b"\n")
    if normalised not in texts:
        texts.append(normalised)
    for text in texts:
        try:
            decoded = text.decode("utf-8")
        except UnicodeDecodeError:
            decoded = text.decode("latin-1")
        if geometry_md5 is not None:
            payload = (decoded + geometry_md5).encode("utf-8", "surrogateescape")
            result["digests"]["manifest+geometry_md5-md5"] = hashlib.md5(
                payload).hexdigest()
            result["digests"]["manifest+geometry_md5-sha256"] = hashlib.sha256(
                payload).hexdigest()
        if geometry_raw is not None:
            # The product exporter hashes the manifest text followed by the
            # geometry file's own bytes; this is the spelling a real handoff
            # carries, so it must be verifiable here.
            raw_payload = text + geometry_raw
            result["digests"]["manifest+geometry_bytes-md5"] = hashlib.md5(
                raw_payload).hexdigest()
            result["digests"]["manifest+geometry_bytes-sha256"] = hashlib.sha256(
                raw_payload).hexdigest()
        result["digests"]["manifest-sha256"] = hashlib.sha256(text).hexdigest()
        result["digests"]["manifest-md5"] = hashlib.md5(text).hexdigest()
    result["available"] = True
    return result


def _normalized_digest(value):
    """A digest in comparable form: no algorithm prefix, lowercase."""
    text = _text(value).lower()
    for prefix in ("sha256:", "md5:", "sha1:"):
        if text.startswith(prefix):
            return text[len(prefix):]
    return text


def digest_matches(offered, computed):
    """``(matched, algorithm)`` of an offered digest against computed ones."""
    wanted = _normalized_digest(offered)
    if not wanted:
        return False, None
    digests = (computed or {}).get("digests") or {}
    for name in sorted(digests):
        if digests[name].lower() == wanted:
            return True, name
    return False, None


# ----------------------------------------------------------- the entry points

def _new_handoff_report(action):
    return {
        "schema": REPORT_SCHEMA,
        "phase": "start",
        "action": action,
        "accepted": False,
        "detail": "",
        "transfer_id": None,
        "space_revision": None,
        "scope": {},
        "digest": {"offered": None, "matched": None, "algorithm": None,
                   "available": False, "computed": {}},
        "counts": {"objects": 0, "triangles": 0, "bytes": 0},
        "container": {"name": REFERENCE_CONTAINER},
        "media": {},
        "texture_scan": {},
        "manifest": {},
        "budget": {},
        "discarded": False,
        "rolled_back": False,
        "residual": None,
        "problems": [],
        "warnings": [],
    }


def _handoff_validation(handoff, report, budget):
    """Validate one offered handoff and fill ``report``; raises frozen refusals.

    The checks own the frozen contract of ``protocol/v11.md``: the offer must
    name a space revision, the directory content must match the offered digest,
    it must not deliver image data, and it must stay inside the frozen file,
    object and triangle budgets. Everything else the handoff declares -- scope,
    counts, texture records -- is reported for the panel.
    """
    limits = _budget(budget)
    report["budget"] = dict(limits)
    if not isinstance(handoff, dict) or not handoff:
        report["detail"] = "no reference handoff was offered"
        report["problems"].append(
            "REFUSED HANDOFF_ABSENT: the offer names no handoff, so there is "
            "nothing to validate")
        return report
    transfer_id = _text(handoff.get("transfer_id"))
    directory = _text(handoff.get("directory"))
    report["transfer_id"] = transfer_id
    report["digest"]["offered"] = _text(handoff.get("digest"))
    scope = handoff.get("scope")
    report["scope"] = dict(scope) if isinstance(scope, dict) else {}
    counts = handoff.get("counts")
    offered_counts = dict(counts) if isinstance(counts, dict) else {}
    space_revision = handoff.get("space_revision")
    if (isinstance(space_revision, bool) or not isinstance(space_revision, (int, float))
            or float(space_revision) != int(space_revision)):
        raise ReferenceRefused(
            CODE_STALE,
            "the reference offer {0!r} names no space_revision, so it cannot be "
            "told apart from a reference of an older common space; ask Unreal to "
            "offer the handoff again".format(transfer_id or "unknown"))
    space_revision = int(space_revision)
    report["space_revision"] = space_revision
    session_revision = handoff.get("session_space_revision")
    if (isinstance(session_revision, int) and not isinstance(session_revision, bool)
            and session_revision != space_revision):
        raise ReferenceRefused(
            CODE_STALE,
            "the reference offer {0} belongs to space revision {1} while the live "
            "session is at {2}; re-labelling an old reference is not an "
            "update".format(transfer_id, space_revision, session_revision))
    files = handoff_files(directory)
    report["media"]["handoff_directory"] = files["directory"]
    report["media"]["handoff_directory_exists"] = files["exists"]
    if not files["exists"]:
        report["detail"] = "the handoff directory is not readable"
        report["problems"].append(
            "REFUSED HANDOFF_DIRECTORY_ABSENT: the handoff directory {0!r} cannot "
            "be read; ask Unreal to offer the handoff again".format(directory))
        return report
    if files["error"] or not files["manifest"]:
        raise ReferenceRefused(
            CODE_DIGEST_MISMATCH,
            "the handoff directory {0} does not hold exactly one manifest: "
            "{1}".format(directory, files["error"] or "no manifest was found"))
    manifest, declared_geometry, manifest_error = read_handoff_manifest(
        files["manifest"])
    if manifest is None:
        raise ReferenceRefused(
            CODE_DIGEST_MISMATCH,
            "the handoff manifest {0} cannot be read: {1}".format(files["manifest"],
                                                                  manifest_error))
    report["manifest"] = manifest_summary(manifest)
    geometry_path = list(files["geometry"])
    if declared_geometry:
        declared_path = os.path.join(files["directory"], declared_geometry)
        if os.path.isfile(declared_path):
            if declared_path not in geometry_path:
                geometry_path.append(declared_path)
        else:
            report["warnings"].append(
                "GEOMETRY_FILE_ABSENT: the manifest names the geometry file {0!r}, "
                "which is not in the handoff directory".format(declared_geometry))
    if not geometry_path:
        report["detail"] = "the handoff carries no geometry file"
        report["problems"].append(
            "REFUSED HANDOFF_GEOMETRY_ABSENT: the handoff directory {0} holds no "
            "FBX geometry, so there is nothing to import".format(directory))
        return report
    scan = scan_fbx_file(geometry_path[0])
    report["texture_scan"] = scan
    image_files = image_files_beside(geometry_path[0])
    report["media"].update({
        "image_files_in_handoff_directory": image_files,
        "embedded_media_records": scan["embedded_media_records"],
        "embedded_media": scan["embedded_media"],
        "content_records": scan["content_records"],
        "texture_records": scan["texture_records"],
        "texture_references": scan["texture_references"],
        "video_references": scan["video_references"],
        "camera_records": scan["camera_records"],
        "light_records": scan["light_records"],
        "media_heuristic": scan["media_heuristic"],
        "geometry_file": os.path.basename(geometry_path[0]),
    })
    if image_files:
        raise ReferenceRefused(
            CODE_IMAGE_DATA,
            "the handoff directory {0} holds image files {1}; a static reference "
            "handoff must not deliver copied or baked images - turn texture "
            "baking off in Unreal and export again".format(directory, image_files))
    if scan["embedded_media_records"]:
        detail = [(record.get("kind"), record.get("line"),
                   record.get("payload_characters"))
                  for record in scan["embedded_media"]]
        raise ReferenceRefused(
            CODE_IMAGE_DATA,
            "the handoff embeds media data in {0} record(s) {1}; the reference "
            "geometry must not carry image data - turn texture baking off in "
            "Unreal and export again".format(scan["embedded_media_records"], detail))
    computed = handoff_digests(files["directory"], manifest=files["manifest"])
    matched, algorithm = digest_matches(report["digest"]["offered"], computed)
    report["digest"]["matched"] = matched
    report["digest"]["algorithm"] = algorithm
    report["digest"]["available"] = bool(computed.get("available"))
    report["digest"]["computed"] = dict(computed.get("digests") or {})
    if not matched:
        raise ReferenceRefused(
            CODE_DIGEST_MISMATCH,
            "the handoff directory {0} does not match the offered digest {1} "
            "(this run computes {2}); the directory may have changed after Unreal "
            "offered it - export the handoff again".format(
                directory, report["digest"]["offered"] or "none",
                ", ".join(sorted((computed.get("digests") or {}).values())) or
                "no digest at all"))
    handoff_bytes = sum(os.path.getsize(path) for path in [files["manifest"]] + geometry_path
                        if os.path.isfile(path))
    manifest_counts = (manifest.get("counts")
                       if isinstance(manifest.get("counts"), dict) else {})
    declared = {"objects": _count(manifest_counts.get("objects")),
                "triangles": _count(manifest_counts.get("triangles")),
                "bytes": _count(manifest_counts.get("bytes"))}
    report["declared_counts"] = declared
    report["counts"] = {"objects": _count(offered_counts.get("objects")),
                        "triangles": _count(offered_counts.get("triangles")),
                        "bytes": handoff_bytes}
    for key, label in (("objects", "imported roots"), ("triangles", "triangles"),
                       ("bytes", "bytes")):
        for source, value in (("the offer", report["counts"][key]),
                              ("the manifest", declared[key])):
            if value is not None and value > limits[key]:
                raise ReferenceRefused(
                    CODE_OVER_BUDGET,
                    "the handoff declares {0} {1} in {2}, above the frozen budget "
                    "of {3}; shrink the reference scope or the object count in "
                    "Unreal and export again".format(value, label, source,
                                                     limits[key]))
    report["detail"] = "the handoff passed validation"
    return report


def inspect(handoff, budget=None):
    """Validate one offered handoff without touching the scene.

    The report carries ``accepted``, ``detail`` and the scope, digest and counts
    information the panel renders; a frozen refusal is raised rather than
    reported, so the controller can name its code.
    """
    report = _new_handoff_report("inspect")
    try:
        _handoff_validation(handoff, report, budget)
    except ReferenceRefused:
        raise
    except Exception as error:  # noqa: BLE001 - validation reports, never raises
        report["detail"] = "the handoff could not be validated: {0}".format(error)
        report["problems"].append(
            "INTERNAL_ERROR: the validation failed: {0}".format(error))
        return report
    report["accepted"] = not report["problems"]
    return report


# ------------------------------------------------------------- the importer

def process_memory_mb():
    """Resident set size of this process in MB, or ``None`` when unreadable.

    Windows uses ``GetProcessMemoryInfo``, every other host reads
    ``/proc/self/statm``. A host that offers neither reports ``None`` instead of
    failing the run, because a memory reading must never decide an import.
    """
    try:
        if os.name == "nt":
            class ProcessMemoryCounters(ctypes.Structure):
                _fields_ = [
                    ("cb", ctypes.c_ulong),
                    ("PageFaultCount", ctypes.c_ulong),
                    ("PeakWorkingSetSize", ctypes.c_size_t),
                    ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t),
                    ("QuotaPagedPagefileUsage", ctypes.c_size_t),
                ]

            counters = ProcessMemoryCounters()
            counters.cb = ctypes.sizeof(counters)
            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            psapi = ctypes.WinDLL("psapi", use_last_error=True)
            kernel32.GetCurrentProcess.restype = ctypes.c_void_p
            psapi.GetProcessMemoryInfo.restype = ctypes.c_int
            psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p,
                                                   ctypes.POINTER(ProcessMemoryCounters),
                                                   ctypes.c_ulong]
            process = kernel32.GetCurrentProcess()
            if psapi.GetProcessMemoryInfo(process, ctypes.byref(counters), counters.cb):
                return (round(counters.WorkingSetSize / (1024.0 * 1024.0), 3),
                        "win32_GetProcessMemoryInfo")
            return None, "win32_GetProcessMemoryInfo error {0}".format(
                ctypes.get_last_error())
        with open("/proc/self/statm", "r", encoding="ascii") as handle:
            fields = handle.read().split()
        pages = int(fields[1])
        return (round(pages * os.sysconf("SC_PAGE_SIZE") / (1024.0 * 1024.0), 3),
                "linux_proc_self_statm")
    except Exception as error:  # noqa: BLE001 - memory reporting must never fail
        return None, "unavailable: {0}".format(error)


class ReferenceImporter(object):
    """Imports one handoff into the tool-owned container and verifies it.

    The update is staged: the handoff is imported into a free
    ``<container>_Incoming`` namespace, verified against the manifest and only
    then does the staging namespace take the container's name, with the previous
    owned reference renamed aside so a failure anywhere before the takeover is
    proven can put it back. Nothing the scene owns is ever deleted: a name this
    tool cannot prove it owns is refused, and the refusal names what was found.
    """

    def __init__(self, handoff, budget=None, container=REFERENCE_CONTAINER,
                 shading=SHADING_DISPLAY, allow_image_data=False, dry_run=False):
        self.handoff = dict(handoff) if isinstance(handoff, dict) else {}
        self.budget = _budget(budget)
        self.container = str(container)
        self.shading = str(shading).strip().lower()
        self.allow_image_data = bool(allow_image_data)
        self.dry_run = bool(dry_run)
        self.validation = None
        self.raw_manifest = None
        self.manifest = None
        self.manifest_path = None
        self.fbx = None
        self.summary = {}
        self.exported = []
        self.scan = {}
        self.staging = None
        self.active_namespace = None
        self.staged = False
        self.takeover = None
        self.retired = None
        self.previous_state = None
        self.frame_factor_reported = False
        self.anchor = None
        self.anchor_record = None
        self.report = {
            "schema": REPORT_SCHEMA,
            "phase": "start",
            "ok": False,
            "maya": {},
            "container": {"name": self.container, "group": self.container,
                          "group_path": None, "staging_namespace": None,
                          "staging_group_path": None, "previous_existed": False,
                          "previous_container_group": None,
                          "previous_reference_uuid": None,
                          "ownership_attribute": REFERENCE_MARK_ATTRIBUTE,
                          "ownership_token": REFERENCE_MARK_VALUE,
                          "swapped": False, "kept_existing": False,
                          "namespace_created": False, "after_run": None},
            "update": {"mode": None, "staging_namespace": None,
                       "existing_container": False, "swapped": False,
                       "discarded": False, "discarded_nodes": [],
                       "stale_staging_removed": False,
                       "stale_staging_namespaces": [], "swap_seconds": None,
                       "post_swap_paths_checked": False,
                       "post_swap_paths_missing": [], "retired": None,
                       "retired_removal": None, "rolled_back": False,
                       "rollback_reason": None, "recovery": None,
                       "discard_attempted": False, "discard_errors": [],
                       "residual": None, "cleanup_errors": []},
            "counts": {"manifest_objects": 0, "container_nodes": 0,
                       "file_texture_nodes": 0, "image_nodes_loaded": 0},
            "media": {},
            "materials": {"mode": self.shading, "gray_material": None,
                          "display": None, "preserved_assignment": [],
                          "imported_material_nodes": [], "file_nodes": []},
            "texture_scan": {},
            "transform_check": {},
            "objects": [],
            "timing": {"staging_seconds": None, "import_seconds": None,
                       "verify_seconds": None},
            "memory": {},
            "problems": [],
            "warnings": [],
            "manifest": {},
            "fbx": {},
            "scene": {},
        }

    # ------------------------------------------------------------- phases

    def run(self):
        """Run every phase and return the report; raises only frozen refusals."""
        started = time.time()
        try:
            self._validate_handoff()
            self._read_anchor()
            self._read_manifest()
            self._read_scene()
            self._stage_handoff()
            self._apply_shading()
            self._verify()
            self._finalise()
        except ReferenceRefused as refusal:
            self.report["phase"] = "refused"
            self.report["detail"] = refusal.detail
            self.report["problems"].append(
                "REFUSED {0}: {1}".format(refusal.code, refusal.detail))
            self._cleanup_after_failure("refused")
            self._finish_report(started)
            raise
        except _Refusal as refusal:
            self.report["phase"] = "refused"
            self.report["detail"] = refusal.detail
            self.report["problems"].append(
                "REFUSED {0}: {1}".format(refusal.category, refusal.detail))
            self._cleanup_after_failure("refused")
        except Exception:  # noqa: BLE001 - the report carries the failure
            self.report["phase"] = "failed"
            self.report["detail"] = "the import failed"
            self.report["error"] = traceback.format_exc()
            self.report["problems"].append(
                "INTERNAL_ERROR: the import failed, see the report's error field")
            self._cleanup_after_failure("failed")
        self._finish_report(started)
        return self.report

    def _validate_handoff(self):
        """Run the frozen handoff validation before anything touches the scene."""
        validation = _new_handoff_report("import")
        _handoff_validation(self.handoff, validation, self.budget)
        self.validation = validation
        self.report["transfer_id"] = validation["transfer_id"]
        self.report["space_revision"] = validation["space_revision"]
        self.report["media"] = validation["media"]
        self.report["texture_scan"] = validation["texture_scan"]
        self.report["budget"] = validation["budget"]
        self.report["scope"] = validation["scope"]
        self.report["validation"] = validation
        files = handoff_files(validation["media"]["handoff_directory"])
        self.manifest_path = files["manifest"]
        manifest, _declared, manifest_error = read_handoff_manifest(self.manifest_path)
        if manifest is None:
            raise _Refusal("MANIFEST_UNREADABLE", manifest_error)
        self.raw_manifest = manifest
        self.scan = validation["texture_scan"]
        geometry = list(files["geometry"])
        declared = validation["manifest"].get("files") if isinstance(
            validation["manifest"].get("files"), dict) else {}
        declared_name = _text(declared.get("geometry"))
        if declared_name:
            declared_path = os.path.join(files["directory"], declared_name)
            if os.path.isfile(declared_path) and declared_path not in geometry:
                geometry.append(declared_path)
        if not geometry:
            raise _Refusal("HANDOFF_GEOMETRY_ABSENT",
                           "the handoff directory holds no FBX geometry")
        self.fbx = geometry[0]
        self.report["phase"] = "validated"

    def _read_anchor(self):
        """Read the anchor the manifest names; none means the Maya origin frame.

        The reference is placed in the frame the character and the linked
        camera share. A manifest without ``world.anchor`` (an export from before
        the anchor existed) leaves the container at identity and says so in the
        report instead of guessing a frame.
        """
        world = (self.raw_manifest or {}).get("world")
        anchor, problem = parse_anchor(
            world.get("anchor") if isinstance(world, dict) else None)
        if problem:
            raise _Refusal(CATEGORY_ANCHOR_INVALID, problem)
        self.anchor = anchor
        self.anchor_record = (
            {"location_cm": list(anchor["location_cm"]),
             "rotation_deg": {name: float(world["anchor"]["rotation_deg"][name])
                              for name in ("roll", "pitch", "yaw")}}
            if anchor else IDENTITY_ANCHOR)
        self.report["anchor"] = {
            "applied": False,
            "source": "manifest" if anchor else "absent",
            "location_cm": list(anchor["location_cm"]) if anchor else None,
        }
        if anchor is None:
            self.report["warnings"].append(
                "ANCHOR_ABSENT: the manifest names no anchor, so the reference stays "
                "at the Maya origin and will not line up with a moved or rotated "
                "target; export again in Unreal and re-import")

    def _place_anchor(self):
        """Move the verified staged container into the anchor frame.

        The container also records the anchor it was placed for (identity when
        the manifest names none), so linkage can tell a reference exported
        before the target moved from a current one.
        """
        try:
            group = self._container_group(self.active_namespace)
            if not cmds.attributeQuery(
                    REFERENCE_ANCHOR_ATTRIBUTE, node=group, exists=True):
                cmds.addAttr(group, longName=REFERENCE_ANCHOR_ATTRIBUTE,
                             dataType="string")
            cmds.setAttr(group + "." + REFERENCE_ANCHOR_ATTRIBUTE,
                         json.dumps(self.anchor_record, sort_keys=True),
                         type="string")
            if self.anchor is None:
                return
            cmds.xform(group, matrix=anchor_container_matrix(self.anchor))
        except _Refusal:
            raise
        except Exception as error:  # noqa: BLE001 - nothing was swapped yet
            raise _Refusal(
                CATEGORY_ANCHOR_INVALID,
                "the container could not be placed in the anchor frame: {0}".format(
                    error))
        self.report["anchor"]["applied"] = True

    def _read_manifest(self):
        """Read the manifest and report the scope before anything is imported.

        A handoff manifest that carries per-object measurements is verified
        object by object. A manifest written without them -- the product's own
        exporter may declare only the scope and the counts -- reports the
        geometric verification as unavailable rather than as a failure: the
        frozen budgets, the content digest and the image-data rules were still
        checked.
        """
        manifest = self.raw_manifest or {}
        self.report["manifest"] = manifest_summary(manifest)
        if not isinstance(manifest.get("objects"), list):
            self.report["warnings"].append(
                "MANIFEST_OBJECTS_ABSENT: the handoff manifest carries no "
                "per-object measurements, so the geometric verification is "
                "unavailable for this handoff")
            self.manifest = manifest
            self.exported = []
            self.report["counts"]["manifest_objects"] = 0
            self.report["counts"]["manifest_objects_total"] = \
                manifest_summary(manifest)["objects_total"]
            self.report["phase"] = "manifest"
            return
        try:
            load_manifest(self.manifest_path)
        except ManifestError as error:
            raise _Refusal(error.category, error.detail)
        self.manifest = load_manifest(self.manifest_path)
        self.summary = manifest_summary(self.manifest)
        self.exported = exported_objects(self.manifest)
        self.report["counts"]["manifest_objects"] = len(self.exported)
        self.report["counts"]["manifest_objects_total"] = \
            self.summary["objects_total"]
        coverage_warning = scope_coverage_warning(self.summary)
        if coverage_warning:
            self.report["warnings"].append(coverage_warning)
        if not self.exported:
            self.report["warnings"].append(
                "NO_EXPORTED_OBJECTS: the manifest declares no exported objects, "
                "so there is nothing to compare")
        self.report["phase"] = "manifest"

    def _read_scene(self):
        """Record the Maya session and refuse a scene the import cannot use."""
        if not _container_name_valid(self.container):
            raise _Refusal(
                CATEGORY_CONTAINER_NAME,
                "container name {0!r} is not a plain Maya node name".format(
                    self.container))
        if self.shading not in SHADING_MODES:
            raise _Refusal(
                "UNKNOWN_SHADING_MODE",
                "shading mode {0!r} is not one of {1}".format(
                    self.shading, ", ".join(SHADING_MODES)))
        try:
            cmds.loadPlugin("fbxmaya", quiet=True)
        except Exception as error:  # noqa: BLE001 - reported as a refusal
            raise _Refusal(CATEGORY_FBX_PLUGIN,
                           "the bundled fbxmaya plugin did not load: {0}".format(error))
        try:
            plugin_version = cmds.pluginInfo("fbxmaya", query=True, version=True)
        except Exception:  # noqa: BLE001
            plugin_version = None
        linear_unit = str(cmds.currentUnit(query=True, linear=True))
        time_unit = str(cmds.currentUnit(query=True, time=True))
        up_axis = str(cmds.upAxis(query=True, axis=True)).lower()
        self.report["maya"] = {
            "version": str(cmds.about(version=True)),
            "api": str(cmds.about(apiVersion=True)),
            "batch": bool(cmds.about(batch=True)),
            "linear_unit": linear_unit,
            "up_axis": up_axis,
            "time_unit": time_unit,
            "plugins": {"fbxmaya": plugin_version},
        }
        if linear_unit.strip().lower() != "cm":
            raise _Refusal(
                CATEGORY_UNSUPPORTED_SCENE_UNIT,
                "the Maya scene linear unit is {0!r}; the manifest is in "
                "centimetres and this importer does not rescale".format(linear_unit))
        if time_unit.strip().lower() not in MANAGED_TIME_UNITS:
            self.report["warnings"].append(
                "UNKNOWN_TIME_UNIT: the scene time unit {0!r} is not one this "
                "importer recognises; it is never changed".format(time_unit))
        self.report["scene"] = {
            "linear_unit": linear_unit,
            "up_axis": up_axis,
            "time_unit": time_unit,
            "current_namespace": cmds.namespaceInfo(currentNamespace=True),
            "playback_range": {
                "min": float(cmds.playbackOptions(query=True, minTime=True)),
                "max": float(cmds.playbackOptions(query=True, maxTime=True)),
            },
            "current_time": float(cmds.currentTime(query=True)),
            "namespaces": list(cmds.namespaceInfo(listOnlyNamespaces=True,
                                                  recurse=True) or []),
        }
        self.report["phase"] = "scene"

    def _stage_handoff(self):
        """Import the handoff into a staging namespace, without touching the reference.

        The staging namespace is what makes the update safe: the previous
        reference stays exactly as it was while the new import is measured, and a
        run that fails deletes only the namespace this run created. Two rules keep
        the run off everything it does not own:

        * A container is only replaceable while it carries this tool's ownership
          mark. A namespace, group or root node of the same name that carries no
          mark is refused, never emptied, and the refusal names what was found.
        * The staging namespace is a free name of the ``<container>_Incoming``
          family, so a namespace of that name that belongs to the scene is neither
          reused nor deleted. A namespace of that family left behind by an
          interrupted run is deleted only when it carries the mark again.

        From the moment the staging namespace exists this run is responsible for
        it: every later failure -- a refusal, a partial import, an internal error
        -- deletes it again and reports what was removed.
        """
        namespace_before = cmds.namespaceInfo(currentNamespace=True)
        state_before = self._scene_state()
        recovery = _recover_previous_runs(self.container)
        state = _container_state(self.container)
        if state["conflict"]:
            raise ReferenceRefused(CODE_CONTAINER_NOT_OWNED, state["conflict"])
        self.previous_state = state
        staging = _free_name(self.container + STAGING_SUFFIX)
        self.staging = staging
        # The container block is written before anything is imported, so a run that
        # fails during the import still reports the previous reference it found and
        # whether it is still there afterwards.
        previous_group = state["group"] or state["root_group"]
        previous_uuid = None
        if previous_group:
            uuids = cmds.ls(previous_group, uuid=True) or []
            previous_uuid = uuids[0] if uuids else None
        self.report["container"].update({
            "staging_namespace": staging,
            "staging_group_path": None,
            "staging_group_uuid": None,
            "previous_existed": bool(previous_group),
            "previous_container_group": previous_group,
            "previous_reference_uuid": previous_uuid,
            "namespace_created": not bool(previous_group),
            "group_path": previous_group,
        })
        self.report["update"].update({
            "mode": "dry_run" if self.dry_run else "staged_swap",
            "staging_namespace": staging,
            "existing_container": bool(state["group"] or state["root_group"]),
            "previous_container_group": previous_group,
            "stale_staging_removed": bool(recovery["stale_staging_namespaces"]),
            "stale_staging_namespaces": recovery["stale_staging_namespaces"],
            "recovery": recovery,
            "previous_state": state["report"],
        })
        for removed in recovery["stale_staging_namespaces"]:
            self.report["warnings"].append(
                "STALE_STAGING_REMOVED: the staging namespace {0} was left behind by "
                "an interrupted run of this tool and has been deleted".format(removed))
        for event in recovery["events"]:
            self.report["warnings"].append(event)

        # The run owns the staging namespace from here on, including a namespace that
        # fails halfway through creation, so every later failure cleans up.
        self.staged = True
        cmds.namespace(add=staging)
        cmds.namespace(set=staging)
        try:
            cmds.createNode("transform", name=staging)
        finally:
            cmds.namespace(set=namespace_before)
        group = self._container_group(staging)
        _mark_owned(group, staging)

        started = time.time()
        cmds.namespace(set=staging)
        try:
            imported = cmds.file(self.fbx, i=True, ns=staging, type="FBX",
                                 ignoreVersion=True, mergeNamespacesOnClash=False,
                                 options="v=0;")
        except Exception as error:  # noqa: BLE001 - reported as a refusal
            raise _Refusal(
                CATEGORY_IMPORT_FAILED,
                "the FBX plugin refused {0}: {1}".format(self.fbx, error))
        finally:
            cmds.namespace(set=namespace_before)
            self.report["scene"]["import_side_effects"] = self._restore_scene_state(
                state_before)
        self.active_namespace = staging
        roots = self._imported_roots(group, staging)
        for node in roots:
            cmds.parent(node, group, relative=True)
        self.report["timing"]["staging_seconds"] = round(time.time() - started, 3)
        self.report["timing"]["import_seconds"] = self.report["timing"]["staging_seconds"]
        self.report["fbx"] = {
            "file": self.fbx,
            "bytes": self.scan.get("bytes") or os.path.getsize(self.fbx),
            "format": self.scan.get("format"),
            "plugin": "fbxmaya",
            "plugin_version": self.report["maya"].get("plugins", {}).get("fbxmaya"),
            "import_options": "v=0;",
            "namespace_flag": staging,
            "namespace_mechanism": ("current namespace during import; this host's "
                                    "fbxmaya ignores the ns flag for node placement"),
            "import_return": str(imported),
            "root_nodes_parented": roots,
        }
        self.report["counts"]["container_nodes"] = len(_namespace_members(staging))
        group_uuid = cmds.ls(group, uuid=True) or []
        self.report["container"].update({
            "staging_group_path": group,
            "staging_group_uuid": group_uuid[0] if group_uuid else None,
        })
        self.report["phase"] = "staging"

    def _apply_shading(self):
        """Show the reference in one gray, without pretending it carries no material.

        The material assignment the FBX carries is kept: the model may have
        material balls, and a texture path in a material is a reference, not a
        delivered image. What the artist needs to see is uniform gray, and that
        is a display treatment: the viewport override colors every imported shape
        gray and hides its textures, while the shading networks stay untouched and
        are reported.
        """
        namespace = self.active_namespace
        shapes = self._container_nodes_of_type("mesh", namespace)
        assignment = []
        for shape in shapes:
            assignment.append({
                "shape": shape,
                "shading_groups": list(cmds.listConnections(shape,
                                                            type="shadingEngine") or []),
            })
        materials = {
            "mode": self.shading,
            "gray_material": None,
            "display": None,
            "preserved_assignment": assignment,
            "imported_material_nodes": self._leftovers(namespace),
            "file_nodes": sorted(self._container_nodes_of_type("file", namespace)),
        }
        if self.shading == SHADING_DISPLAY:
            overridden = []
            for shape in shapes:
                cmds.setAttr(shape + ".overrideEnabled", True)
                cmds.setAttr(shape + ".overrideShading", True)
                cmds.setAttr(shape + ".overrideTexturing", True)
                cmds.setAttr(shape + ".overrideColorRGB", *GRAY_DISPLAY_COLOR,
                             type="double3")
                overridden.append(shape)
            materials["display"] = {
                "color": list(GRAY_DISPLAY_COLOR),
                "shapes_overridden": len(overridden),
                "shapes": overridden,
                "note": ("a viewport display override: the shapes keep the material "
                         "assignment the handoff carries, and a render still uses "
                         "those materials"),
            }
        elif self.shading == SHADING_MATERIAL:
            materials["gray_material"] = self._assign_gray_material(shapes)
        self.report["materials"] = materials

        file_nodes = materials["file_nodes"]
        image_present = [node for node in file_nodes if _file_node_image_exists(node)]
        self.report["counts"]["file_texture_nodes"] = len(file_nodes)
        self.report["counts"]["image_nodes_loaded"] = len(image_present)
        self.report["counts"]["meshes_assigned"] = len(shapes)
        self.report["media"]["file_nodes_created"] = len(file_nodes)
        self.report["media"]["image_nodes_loaded"] = len(image_present)
        self.report["media"]["image_paths_present"] = [
            _file_node_image_path(node) for node in image_present]
        if image_present:
            self.report["warnings"].append(
                "REFERENCED_IMAGE_FILES_PRESENT: {0} imported file node(s) name an "
                "image that exists on this machine ({1}); the handoff delivered no "
                "image, but Maya resolves those references when it evaluates "
                "shading".format(len(image_present),
                                 self.report["media"]["image_paths_present"]))
        self.report["phase"] = "shading"

    def _assign_gray_material(self, shapes):
        """Create or reuse the gray lambert and assign it to every mesh shape."""
        gray_name = REFERENCE_GRAY_SHADER
        existing = cmds.ls(gray_name, type="lambert")
        reused = bool(existing)
        if existing:
            shader = existing[0]
        elif cmds.objExists(gray_name):
            raise _Refusal(
                CATEGORY_GRAY_MATERIAL,
                "a node named {0} exists and is not a lambert; the importer does "
                "not rename or replace scene objects it did not create".format(
                    gray_name))
        else:
            shader = cmds.shadingNode("lambert", asShader=True, name=gray_name)
        cmds.setAttr(shader + ".color", 0.5, 0.5, 0.5, type="double3")
        color = [float(value) for value in cmds.getAttr(shader + ".color")[0]]
        group_name = gray_name + "SG"
        if cmds.objExists(group_name):
            shading_group = cmds.ls(group_name, type="shadingEngine")[0]
        else:
            shading_group = cmds.sets(renderable=True, noSurfaceShader=True,
                                      empty=True, name=group_name)
        if not cmds.isConnected(shader + ".outColor",
                                shading_group + ".surfaceShader"):
            cmds.connectAttr(shader + ".outColor",
                             shading_group + ".surfaceShader", force=True)
        replaced = 0
        for shape in shapes:
            before = cmds.listConnections(shape, type="shadingEngine") or []
            cmds.sets(shape, edit=True, forceElement=shading_group)
            after = cmds.listConnections(shape, type="shadingEngine") or []
            if before != after:
                replaced += 1
        return {
            "name": shader,
            "type": str(cmds.nodeType(shader)),
            "color": color,
            "shading_group": shading_group,
            "reused": reused,
            "meshes_assigned": len(shapes),
            "shading_groups_replaced": replaced,
        }

    def _verify(self):
        """Compare every exported manifest object with its Maya node.

        Every exported object is matched by name first. Objects the engine names
        after an instance index -- a purely numeric node name, which Maya is free
        to rename -- are then matched by world position under the best axis
        candidate, and the method used is reported per object as ``matched_by``
        (``name`` or ``transform``). A node that cannot be matched either way is
        a reported problem, never a skipped comparison.
        """
        started = time.time()
        comparison = self._compare_against_manifest()
        self.report["objects"] = comparison["objects"]
        self.report["transform_check"] = comparison["transform_check"]
        self.report["problems"].extend(comparison["problems"])
        self.report["warnings"].extend(comparison["warnings"])
        counts = comparison["transform_check"]
        transform_matched = [entry.get("node_name") for entry in comparison["objects"]
                             if entry.get("matched_by") == "transform"]
        if transform_matched:
            self.report["warnings"].append(
                "TRANSFORM_MATCH: {0} object(s) were matched by world position "
                "rather than by name: {1}".format(len(transform_matched),
                                                  transform_matched))
        self.report["counts"]["matched_objects"] = counts.get("objects_compared", 0)
        self.report["counts"]["matched_by_name"] = len(
            [entry for entry in comparison["objects"]
             if entry.get("matched_by") == "name"])
        self.report["counts"]["matched_by_transform_count"] = len(transform_matched)
        self.report["timing"]["verify_seconds"] = round(time.time() - started, 3)
        self.report["phase"] = "verify"
        self._record_scene_after()

    def _compare_against_manifest(self):
        """Measure the staged container and compare it against the manifest."""
        if not self.exported:
            return {"transform_check": {}, "objects": [], "problems": [], "warnings": []}
        index = self._container_index(self.active_namespace)
        entries = []
        for entry in self.exported:
            name, unreal_matrix, unreal_size, unreal_offset, unreal_centroid, \
                problems = manifest_object_requirement(entry)
            path, matched_by, ambiguous = _match_container_node(index, name)
            entries.append({
                "manifest": entry,
                "node_name": name,
                "ue_matrix": unreal_matrix,
                "ue_size": unreal_size,
                "ue_offset": unreal_offset,
                "ue_centroid": unreal_centroid,
                "problems": problems,
                "path": path,
                "matched_by": matched_by,
                "ambiguous_matches": ambiguous,
                "match_distance_cm": None,
                "match_candidate": None,
            })
        measurements = [self._measure_manifest_object(entry) for entry in entries]
        comparison = compare_measurements(AXIS_CANDIDATES, measurements,
                                          frame=self._frame_factor())
        if self._match_pending_by_transform(entries, comparison):
            measurements = [self._measure_manifest_object(entry) for entry in entries]
            comparison = compare_measurements(AXIS_CANDIDATES, measurements,
                                              frame=self._frame_factor())
        return comparison

    def _frame_factor(self):
        """The node frame factor of this handoff, or ``None`` when unmeasured.

        The factor is a property of the export axis option the manifest declares.
        A manifest that carries no conventions block, or one that names an option
        whose factor was never measured, reports the orientation comparison as
        unavailable instead of comparing against a guess.
        """
        conventions = (self.manifest or {}).get("conventions")
        if not isinstance(conventions, dict) or not conventions:
            return None
        option = conventions.get("export_axis_option")
        if option in MEASURED_ASYMMETRIC_FRAME_OPTIONS:
            return ENGINE_HANDOFF_LOCAL_FRAME
        if not self.frame_factor_reported:
            self.frame_factor_reported = True
            self.report["warnings"].append(
                "ORIENTATION_UNAVAILABLE: the manifest declares the export axis "
                "option {0!r}, whose node frame factor was never measured; the "
                "orientation comparison is reported as unavailable".format(option))
        return None


    # ------------------------------------------------ takeover and rollback

    def _finalise(self):
        """Replace the previous reference with the staged one, or put it back.

        The takeover runs only after the comparison reported no problem, and it
        never deletes the previous reference before the new one has taken its
        place and proven it:

        1. the previous, owned container is renamed into a free retiring name --
           renamed, not deleted, so it can be put back by name and keep its UUID;
        2. the staging namespace takes the container's name and its group is
           renamed to the name the contract promises -- its ownership mark still
           names the staging namespace, so an interruption here is recognisable and
           recoverable rather than a finished commit;
        3. every recorded path is read back from the new container;
        4. only then is the group marked for the container's own name -- the proof
           that the container is a finished takeover -- and the retired reference
           deleted.

        Every step after the retire runs inside one rollback boundary: an exception
        from any of them is caught here, the container this run created is deleted,
        the retired reference is renamed back, and the run reports the rollback
        instead of leaving a half-committed container behind.
        """
        update = self.report["update"]
        if self.dry_run or self.report["problems"]:
            self._discard_staging("dry_run" if self.dry_run else "check_failed")
            self.report["phase"] = "dry_run" if self.dry_run else "check_failed"
            return
        self._place_anchor()
        started = time.time()
        try:
            self.retired = _retire_container(self.container, self.previous_state)
        except Exception as error:  # noqa: BLE001 - nothing was created yet
            raise _Refusal(
                CATEGORY_SWAP_FAILED,
                "the previous reference could not be renamed aside: {0}".format(error))
        update["retired"] = self.retired
        self.takeover = {"namespace_renamed": False, "group_renamed": False,
                         "paths_checked": False, "marked": False}
        failure = None
        try:
            cmds.namespace(rename=(self.staging, self.container))
            self.takeover["namespace_renamed"] = True
            cmds.rename(self.container + ":" + self.staging,
                        self.container + ":" + self.container)
            self.takeover["group_renamed"] = True
            self.active_namespace = self.container
            update.update({
                "mode": "staged_swap",
                "swapped": True,
                "discarded": False,
                "swap_seconds": round(time.time() - started, 3),
            })
            self.report["container"].update({
                "group_path": self._container_group(self.container),
                "swapped": True,
            })
            # The measurements were taken in the staging namespace; a rename moves no
            # node, so only the recorded paths are rehomed, and each of them is read
            # back to prove it still resolves.
            self._rehome_paths()
            self.takeover["paths_checked"] = True
            missing = update["post_swap_paths_missing"]
            if missing:
                raise RuntimeError(
                    "the swapped container does not resolve {0} recorded path(s): "
                    "{1}".format(len(missing), missing))
            # The container is complete: mark it for its own name, which is what makes
            # a later run treat it as a finished takeover rather than a half one.
            _mark_owned(self._container_group(self.container), self.container)
            self.takeover["marked"] = True
        except Exception as error:  # noqa: BLE001 - rolled back below
            failure = ("the takeover of {0} failed after {1}: {2}".format(
                self.container, self._takeover_stage(), error))
            update["rollback_error"] = traceback.format_exc()
        if failure is None:
            self.takeover = None
            self.staged = False
            # The takeover is complete: the retired reference can go now.
            if self.retired:
                update["retired_removal"] = _remove_retired(self.retired)
            self.report["counts"]["container_nodes"] = len(
                _namespace_members(self.container))
            self.report["phase"] = "finalise"
            return
        # The previous reference is still in its retiring name: put it back and delete
        # the container this run created, then report what happened.
        rollback = self._rollback_swap()
        update.update({
            "mode": "staged_swap_rolled_back",
            "swapped": False,
            # The staged import is gone with its namespace, which is what the
            # artist's "the new reference was discarded" question answers.
            "discarded": bool(rollback.get("staging_namespace_removed")
                              or rollback.get("container_namespace_removed")),
            "rolled_back": True,
            "rollback_reason": failure,
            "rollback": rollback,
        })
        self.report["problems"].append("STAGED_SWAP_FAILED: {0}".format(failure))
        self.report["detail"] = failure
        self.report["phase"] = "rolled_back"

    def _takeover_stage(self):
        """The step the in-flight takeover reached, for the rollback report."""
        stage = self.takeover or {}
        reached = [name for name in ("namespace_renamed", "group_renamed",
                                     "paths_checked", "marked") if stage.get(name)]
        return ", ".join(reached) if reached else "the retire"

    def _cleanup_after_failure(self, reason):
        """Undo what this run created after a failure, and never raise.

        A failure can arrive while a takeover is in flight (any exception between
        the retire and the verified mark) or before one starts. This restores the
        previous reference in the first case and deletes the staging namespace in
        both, recording every cleanup error and whatever nodes this tool still
        occupies instead of letting a host refusal escape the public API.
        """
        update = self.report["update"]
        if self.takeover:
            try:
                rollback = self._rollback_swap()
                update.update({
                    "mode": "staged_swap_rolled_back",
                    "swapped": False,
                    "discarded": False,
                    "rolled_back": True,
                    "rollback_reason": (update.get("rollback_reason")
                                        or "the run failed with the takeover in "
                                           "flight ({0})".format(
                                               self._takeover_stage())),
                    "rollback": rollback,
                })
            except Exception:  # noqa: BLE001 - reported, never raised
                self._record_cleanup_error("the takeover could not be rolled back")
        try:
            self._discard_staging(reason)
        except Exception:  # noqa: BLE001 - reported, never raised
            self._record_cleanup_error("the staging namespace could not be discarded")

    def _record_cleanup_error(self, message):
        """Append one cleanup failure to the report, with the host's traceback."""
        update = self.report["update"]
        entry = "{0}: {1}".format(message, traceback.format_exc())
        update.setdefault("cleanup_errors", []).append(entry)
        self.report["problems"].append(
            "CLEANUP_ERROR: {0}".format(entry.splitlines()[0]))

    def _discard_staging(self, reason):
        """Delete the staging namespace, leaving the previous reference untouched.

        This covers every failure before and during the import. The staging
        namespace is deleted from the moment it exists, so a partial import leaves
        no node behind; a failure that happens after the staged namespace took the
        container's name is undone by :meth:`_finalise` itself.

        A host that refuses the removal is reported, never raised: the run keeps its
        original failure, records the cleanup error and the nodes that are still
        there, and leaves a retry to the next run's recovery.
        """
        if not self.staged or not self.staging:
            return
        update = self.report["update"]
        nodes = _namespace_members(self.staging)
        # This run created this namespace -- its name was free when the run took it --
        # so everything in it appeared while the run held it and the namespace can go
        # as a whole, a partially imported node included.
        removed, error = _remove_namespace(self.staging)
        self.staged = False
        self.active_namespace = None
        update["discard_attempted"] = True
        if error:
            update.setdefault("discard_errors", []).append(error)
        update["residual"] = _residual_state(self.staging)
        if not removed:
            # A cleanup the host refused is a problem of its own: the run must not look
            # successful while its staging nodes are still in the scene.
            self.report["problems"].append(
                "STAGING_CLEANUP_FAILED: the staging namespace {0!r} is still in the "
                "scene; the next run sweeps it".format(self.staging))
        if not update.get("swapped"):
            update["mode"] = ("dry_run" if reason == "dry_run"
                              else "staged_swap_discarded")
            update["discarded"] = bool(removed)
            update["discard_reason"] = reason
            update["discarded_nodes"] = nodes
            update["discarded_node_count"] = len(nodes)
            update["staging_namespace_removed"] = bool(removed)

    def _residual_state(self, namespace):
        """What this tool still occupies of one namespace, read after a cleanup."""
        state = {"namespace": namespace, "namespace_present": False, "nodes": []}
        try:
            state["namespace_present"] = bool(cmds.namespace(exists=namespace))
            state["nodes"] = _namespace_members(namespace)
        except Exception:  # noqa: BLE001 - reporting must never fail a run
            state["unreadable"] = traceback.format_exc().splitlines()[-1]
        return state

    def _rehome_paths(self):
        """Rewrite recorded paths after the swap, reading every one back.

        A rename moves no node, so the measurements stay valid; only the recorded
        paths change. Each path is resolved again by short name inside the final
        container, which also proves the swap left a container the contract can
        address.
        """
        missing = []

        def resolve(value):
            if not isinstance(value, str) or not value:
                return value
            short = _short_node_name(value)
            if not short:
                return value
            found = cmds.ls(self.container + ":" + short, long=True) or []
            if found:
                return found[0]
            missing.append(value)
            return value

        for entry in self.report["objects"]:
            entry["path"] = resolve(entry.get("path"))
        materials = self.report["materials"]
        for entry in materials.get("preserved_assignment") or []:
            entry["shape"] = resolve(entry.get("shape"))
        materials["file_nodes"] = [resolve(node)
                                   for node in materials.get("file_nodes") or []]
        display = materials.get("display")
        if display:
            display["shapes"] = [resolve(shape) for shape in display.get("shapes") or []]
        self.report["update"]["post_swap_paths_checked"] = True
        self.report["update"]["post_swap_paths_missing"] = missing
        if missing:
            self.report["problems"].append(
                "POST_SWAP_PATHS: {0} recorded path(s) do not resolve after the "
                "swap: {1}".format(len(missing), missing))

    def _measure_manifest_object(self, entry):
        """Maya readings for one manifest object, or an unfound placeholder."""
        measurement = {
            "node_name": entry["node_name"],
            "matched_id": entry["manifest"].get("id"),
            "category": entry["manifest"].get("category"),
            "found": bool(entry["path"]),
            "path": entry["path"],
            "matched_by": entry["matched_by"],
            "ambiguous_matches": entry["ambiguous_matches"],
            "problems": list(entry["problems"]),
            "ue_matrix": entry["ue_matrix"],
            "ue_size": entry["ue_size"],
            "ue_offset": entry["ue_offset"],
            "ue_centroid": entry["ue_centroid"],
            "maya_matrix": None,
            "maya_size": None,
            "maya_offset": None,
            "maya_centroid": None,
            "centroid_faces_used": 0,
            "centroid_triangles_used": 0,
            "centroid_sampled": False,
            "local_size": None,
            "identification_size": None,
            "match_distance_cm": entry["match_distance_cm"],
            "match_candidate": entry["match_candidate"],
        }
        if not entry["path"]:
            return measurement
        try:
            matrix = cmds.xform(entry["path"], query=True, worldSpace=True, matrix=True)
        except Exception as error:  # noqa: BLE001 - reported, never skipped
            measurement["problems"].append(
                "NODE_UNREADABLE: {0} could not be read: {1}".format(entry["path"],
                                                                     error))
            return measurement
        measurement["maya_matrix"] = [float(value) for value in matrix]
        try:
            bounds = _world_bounds(entry["path"])
            measurement["maya_size"] = [bounds[1][axis] - bounds[0][axis]
                                        for axis in range(3)]
            position = matrix4_translation(matrix)
            measurement["maya_offset"] = [
                (bounds[0][axis] + bounds[1][axis]) / 2.0 - position[axis]
                for axis in range(3)]
            measurement["local_size"] = _local_bounds_size(entry["path"])
            measurement["identification_size"] = _identification_size(
                measurement["local_size"], measurement["maya_matrix"])
        except Exception as error:  # noqa: BLE001 - size checks are reported missing
            measurement["problems"].append(
                "BOUNDS_UNREADABLE: {0} has no readable bounding box: {1}".format(
                    entry["path"], error))
        try:
            centroid, faces, triangles, sampled = _world_surface_centroid(entry["path"])
            measurement["maya_centroid"] = centroid
            measurement["centroid_faces_used"] = faces
            measurement["centroid_triangles_used"] = triangles
            measurement["centroid_sampled"] = sampled
        except Exception as error:  # noqa: BLE001 - the check reports itself unavailable
            measurement["centroid_note"] = "the faces could not be read: {0}".format(
                error)
        return measurement

    def _fallback_maps(self, comparison):
        """The maps to try when matching by world position, most likely first.

        The winner of the comparison decides -- a named candidate or, when no
        named one fits, the fitted signed permutation -- and the remaining named
        candidates follow in the contract's order, so a handoff whose axis
        convention is not one of the named maps has still a chance to match.
        """
        transform_check = comparison["transform_check"]
        winner = transform_check.get("winner") or {}
        maps = []
        if winner.get("matrix"):
            maps.append({"name": winner["name"],
                         "matrix": matrix3(winner["matrix"])})
        for candidate in (transform_check.get("candidates") or []):
            if winner.get("name") == candidate["name"]:
                continue
            maps.append({"name": candidate["name"],
                         "matrix": matrix3(candidate["matrix"])})
        return maps

    def _match_pending_by_transform(self, entries, comparison):
        """Match the remaining objects by world position; returns whether any moved.

        A missing node is never skipped: objects that cannot be matched by name
        and do not sit where the comparison's winning map puts them stay reported
        as missing. Positions are compared against the container transforms this
        run has not matched yet, so one Maya node never answers for two objects.
        The engine names instanced children after their instance index, and this
        host's FBX plugin escapes a purely numeric node name (``0`` arrives as
        ``FBXASC048``), which is exactly the case this fallback exists for.
        """
        pending = [entry for entry in entries
                   if entry["path"] is None and entry["ue_matrix"]]
        if not pending:
            return False
        group = self._container_group(self.active_namespace)
        used = {entry["path"] for entry in entries if entry["path"]}
        available = [node for node in self._container_nodes_of_type("transform",
                                                                   self.active_namespace)
                     if node not in used and node != group]
        if not available:
            return False
        positions = {}
        for node in available:
            matrix = cmds.xform(node, query=True, worldSpace=True, matrix=True)
            positions[node] = matrix4_translation([float(value) for value in matrix])
        tolerance = TOLERANCES["position_cm"]
        maps = self._fallback_maps(comparison)
        moved = False
        for candidate in maps:
            matrix = candidate["matrix"]
            taken = set()
            assignments = []
            for entry in pending:
                expected = matrix3_apply(
                    matrix, matrix4_translation(entry["ue_matrix"]))
                best_node = None
                best_distance = None
                for node, position in positions.items():
                    if node in taken:
                        continue
                    distance = vector_length(
                        tuple(expected[axis] - position[axis] for axis in range(3)))
                    if distance <= tolerance and (best_distance is None
                                                  or distance < best_distance):
                        best_node, best_distance = node, distance
                if best_node is not None:
                    taken.add(best_node)
                    assignments.append((entry, best_node, best_distance))
            if not assignments:
                continue
            for entry, node, distance in assignments:
                entry["path"] = node
                entry["matched_by"] = "transform"
                entry["match_distance_cm"] = distance
                entry["match_candidate"] = candidate["name"]
            moved = True
            break
        return moved

    def _record_scene_after(self):
        """Prove on the record that the run left the session where it found it."""
        scene = self.report.get("scene") or {}
        before_range = scene.get("playback_range") or {}
        before_time = scene.get("current_time")
        after_range = {
            "min": float(cmds.playbackOptions(query=True, minTime=True)),
            "max": float(cmds.playbackOptions(query=True, maxTime=True)),
        }
        after_time = float(cmds.currentTime(query=True))
        scene["playback_range_after"] = after_range
        scene["current_time_after"] = after_time
        scene["namespaces_after"] = list(
            cmds.namespaceInfo(listOnlyNamespaces=True, recurse=True) or [])
        scene["time_unit_after"] = str(cmds.currentUnit(query=True, time=True))
        scene["linear_unit_after"] = str(cmds.currentUnit(query=True, linear=True))
        scene["playback_range_unchanged"] = (
            before_range.get("min") == after_range["min"]
            and before_range.get("max") == after_range["max"])
        scene["current_time_unchanged"] = before_time == after_time
        self.report["scene"] = scene

    # -------------------------------------------------------------- helpers

    def _rollback_swap(self):
        """Put the previous reference back after a takeover that did not complete.

        The staged content is deleted first -- it is this run's own, and its group
        carries the mark of the staging name -- so the retiring name is free when
        the previous reference is renamed back under the container's name.
        """
        rollback = {"container_namespace_removed": False, "container_nodes_removed": [],
                    "staging_namespace_removed": False, "container_skip_reason": None,
                    "errors": [], "previous_reference_restored": False, "restore": None}
        if self.staged and self.staging and cmds.namespace(exists=self.staging):
            removed, error = _remove_namespace(self.staging)
            rollback["staging_namespace_removed"] = bool(removed)
            if error:
                rollback["errors"].append(error)
        marks = [_ownership_mark(self.container)]
        if self.staging:
            marks.append(_ownership_mark(self.staging))
        removal = _remove_created_container(self.container, marks)
        rollback["container_namespace_removed"] = removal["namespace_removed"]
        rollback["container_nodes_removed"] = removal["nodes_removed"]
        rollback["container_skip_reason"] = removal["skipped_reason"]
        if removal["skipped_reason"] and not removal["namespace_removed"]:
            rollback["errors"].append(removal["skipped_reason"])
        self.staged = False
        self.takeover = None
        self.active_namespace = None
        restore = _restore_retired(self.container, self.retired)
        rollback["restore"] = restore
        rollback["previous_reference_restored"] = bool(restore.get("restored"))
        rollback["errors"].extend(restore.get("errors") or [])
        rollback["residual"] = _residual_state(self.container)
        return rollback

    def _record_container_state(self):
        """Write the container fields from the scene as it is, not from intent.

        ``kept_existing`` answers a question about the scene: is the very node this
        run found still there, read back by UUID? A completed swap deletes it, a
        failed one keeps or restores it, and the report says which happened even
        when the failure was not the one the run expected.
        """
        try:
            container = self.report.get("container") or {}
            previous_uuid = container.get("previous_reference_uuid")
            found = cmds.ls(previous_uuid, long=True) if previous_uuid else None
            state = _container_state(self.container)
            staging_present = bool(self.staging and cmds.namespace(exists=self.staging))
        except Exception:  # noqa: BLE001 - reporting must never fail a run
            return
        container["group_path"] = state["group"] or state["root_group"]
        container["after_run"] = {
            "namespace": state["namespace"],
            "group_path": container["group_path"],
            "incomplete_groups": state["incomplete"],
            "foreign_nodes": state["foreign_nodes"],
            "staging_namespace_present": staging_present,
        }
        # `None` says the run found no previous reference at all, which is a different
        # statement from a reference that was there and is gone now.
        container["previous_reference_present_after_run"] = (bool(found)
                                                             if previous_uuid else None)
        container["previous_reference_path_after_run"] = found[0] if found else None
        container["kept_existing"] = bool(found)

    def _scene_state(self):
        """The session state the importer promises not to keep."""
        return {
            "current_time": float(cmds.currentTime(query=True)),
            "playback_min": float(cmds.playbackOptions(query=True, minTime=True)),
            "playback_max": float(cmds.playbackOptions(query=True, maxTime=True)),
            "time_unit": str(cmds.currentUnit(query=True, time=True)),
        }

    def _restore_scene_state(self, before):
        """Undo the frame rate, playback range and current time an import moved.

        ``cmds.file`` importing an FBX moves Maya's current time to the imported
        take and can adopt the file's frame rate, so the state captured before
        the import is put back and every difference is reported.
        """
        after = self._scene_state()
        changed = {key: {"before": before[key], "after": after[key]}
                   for key in before if before[key] != after[key]}
        if not changed:
            return changed
        if "time_unit" in changed:
            cmds.currentUnit(time=before["time_unit"])
        if "playback_min" in changed or "playback_max" in changed:
            cmds.playbackOptions(minTime=before["playback_min"],
                                 maxTime=before["playback_max"])
        if "current_time" in changed:
            cmds.currentTime(before["current_time"])
        return changed

    def _namespace_members(self, namespace):
        return list(cmds.namespaceInfo(namespace, listNamespace=True) or [])

    def _container_nodes_of_type(self, node_type, namespace=None):
        """Nodes of ``node_type`` inside one namespace."""
        namespace = namespace or self.active_namespace or self.container
        return list(cmds.ls(namespace + ":*", long=True, type=node_type) or [])

    def _container_group(self, namespace):
        """Long name of the container transform inside its namespace."""
        nodes = cmds.ls(namespace + ":" + namespace, long=True,
                       type="transform") or []
        if not nodes:
            raise _Refusal(CATEGORY_SWAP_FAILED,
                           "the container group {0}:{0} was not created".format(
                               namespace))
        return nodes[0]

    def _imported_roots(self, group, namespace):
        """Imported container transforms that are still at the top of the scene."""
        roots = []
        for node in self._container_nodes_of_type("transform", namespace):
            if node == group:
                continue
            parent = cmds.listRelatives(node, parent=True, fullPath=True)
            if not parent:
                roots.append(node)
        return roots

    def _container_index(self, namespace):
        """Container transforms by short node name, with their long paths."""
        index = {}
        for node in self._container_nodes_of_type("transform", namespace):
            short = _short_node_name(node)
            index.setdefault(short, []).append(node)
        return index

    def _leftovers(self, namespace):
        """Imported shading nodes the container still holds after the import."""
        leftovers = {"materials": [], "file": [], "place2d": [], "other": []}
        for node in _namespace_members(namespace):
            node_type = cmds.nodeType(node)
            if node_type in ("lambert", "blinn", "phong", "standardSurface",
                             "anisotropic", "shadingEngine"):
                leftovers["materials"].append(node)
            elif node_type == "file":
                leftovers["file"].append(node)
            elif node_type in ("place2dTexture", "projection", "bump2d",
                               "materialInfo"):
                leftovers["place2d"].append(node)
        leftovers["count"] = sum(len(values) for key, values in leftovers.items()
                                 if key != "count")
        return leftovers

    def _finish_report(self, started):
        """Close the report: timing, memory, the scene's real container state."""
        self.report["timing"]["total_seconds"] = round(time.time() - started, 3)
        rss, source = process_memory_mb()
        self.report["memory"] = {"process_rss_mb": rss,
                                 "process_rss_available": rss is not None,
                                 "source": source}
        # The container report is written from the scene as it is now, never from
        # what this run intended: whether the previous reference is still there is
        # read back, not assumed.
        self._record_container_state()
        self._apply_product_fields()

    def _apply_product_fields(self):
        """The report fields the panel, the protocol message and the caller use.

        The protocol's ``reference_result`` counts one shape -- objects, triangles
        and bytes -- so the importer's detailed counts are kept beside it instead
        of replacing it, and the container block is flattened to what the panel
        renders.
        """
        problems = self.report["problems"]
        update = self.report["update"]
        detail_counts = self.report["counts"]
        validation = self.validation or {}
        declared = validation.get("declared_counts") or {}
        reported = validation.get("counts") or {}
        triangles = (declared.get("triangles") if declared.get("triangles") is not None
                     else reported.get("triangles"))
        # The reported count is the objects the comparison matched when the
        # manifest declared per-object measurements, and the handoff's own
        # declared count when it did not.
        objects = (detail_counts.get("matched_objects")
                   or declared.get("objects")
                   or reported.get("objects")
                   or detail_counts.get("container_nodes")
                   or 0)
        self.report["detail_counts"] = detail_counts
        self.report["counts"] = {"objects": objects or 0,
                                 "triangles": triangles or 0,
                                 "bytes": reported.get("bytes") or 0}
        container = dict(self.report["container"])
        container["name"] = self.container
        self.report["container"] = container
        self.report["discarded"] = bool(update.get("discarded"))
        self.report["rolled_back"] = bool(update.get("rolled_back"))
        self.report["residual"] = (update.get("residual")
                                   if update.get("residual") is not None
                                   else _residual_state(update.get("staging_namespace")))
        self.report["accepted"] = not problems
        if not self.report.get("detail"):
            self.report["detail"] = _import_detail(self.report["phase"], problems,
                                                   self.report["counts"],
                                                   self.container)


def _import_detail(phase, problems, counts, container):
    """One status line for a finished import, whatever its outcome."""
    if problems:
        return problems[0]
    if phase == "dry_run":
        return "the handoff was staged and verified and discarded again"
    return "imported {0} object(s) into {1}".format(counts.get("objects") or 0,
                                                    container)


def import_handoff(handoff, budget=None):
    """Import one offered handoff into the tool-owned container.

    The handoff is validated first -- a frozen refusal is raised before anything
    touches the scene -- then staged, verified and swapped. The report carries
    ``accepted``, ``detail``, ``counts``, ``container``, ``discarded``,
    ``rolled_back`` and ``residual``, so the panel can render the outcome and the
    protocol message can answer Unreal.
    """
    report = inspect(handoff, budget)
    if not report["accepted"]:
        return report
    return ReferenceImporter(handoff, budget).run()


# ------------------------------------------------------- ownership lifecycle

def _container_name_valid(name):
    """Whether ``name`` is a plain Maya node name this tool may own."""
    if not isinstance(name, str) or not name.strip():
        return False
    if any(character in name for character in (":", "|", " ", "\t", "\n")):
        return False
    return re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) is not None


def _ownership_mark(name):
    """The ownership value this tool writes for the namespace ``name``."""
    return "{0} {1}".format(REFERENCE_MARK_VALUE, name)


def _ownership_token(node):
    """The ownership value recorded on ``node``, or ``None`` when it carries none.

    The mark is an ordinary string attribute, so it is read back from the node
    itself: a node without it may belong to the scene, and this tool never deletes
    such a node. An attribute that cannot be read counts as no mark.
    """
    try:
        if not cmds.attributeQuery(REFERENCE_MARK_ATTRIBUTE, node=node, exists=True):
            return None
        value = cmds.getAttr(node + "." + REFERENCE_MARK_ATTRIBUTE)
    except Exception:  # noqa: BLE001 - an unreadable mark is no mark
        return None
    return str(value) if value is not None else None


def _mark_owned(node, name):
    """Write this tool's ownership mark for ``name`` onto a group it created."""
    if not cmds.attributeQuery(REFERENCE_MARK_ATTRIBUTE, node=node, exists=True):
        cmds.addAttr(node, longName=REFERENCE_MARK_ATTRIBUTE, dataType="string")
    cmds.setAttr(node + "." + REFERENCE_MARK_ATTRIBUTE, _ownership_mark(name),
                 type="string")


def _free_name(base):
    """The first free name of ``base``, ``base_1``, ... no namespace or node uses.

    A run only ever takes a free name, so a namespace or node of the same name
    that belongs to the scene is never emptied, reused or deleted.
    """
    for index in range(NAME_ATTEMPTS):
        candidate = base if index == 0 else "{0}_{1}".format(base, index)
        if not _container_name_valid(candidate):
            continue
        if cmds.namespace(exists=candidate):
            continue
        if _root_nodes(candidate) or cmds.objExists(candidate):
            continue
        return candidate
    raise _Refusal(CATEGORY_CONTAINER_NAME,
                   "no free name of the {0!r} family is left for this run".format(base))


def _existing_family_names(base):
    """Namespace names of one family that exist: ``base``, ``base_1``, ..."""
    names = []
    for name in cmds.namespaceInfo(listOnlyNamespaces=True, recurse=True) or []:
        text = str(name)
        if text == base or re.fullmatch(re.escape(base) + r"_\d+", text):
            names.append(text)
    return sorted(names)


def _owns_marked_group(namespace, mark):
    """Whether ``namespace`` holds a top level group carrying ``mark``.

    Used only where the run created the namespace itself -- the staging namespace
    it took a free name for, and the container name its own staging namespace was
    renamed into -- so a matching mark is proof that the group is the one it put
    there. A namespace the run found in the scene never reaches a wholesale removal
    through this check.
    """
    return bool([path for path in _namespace_top_nodes(namespace)
                 if _ownership_token(path) == mark])


def _namespace_ownership(name, accepted_marks):
    """Whether every node at the top of one namespace is provably this tool's.

    A run marks the group it creates and nothing else, so the only nodes it can
    prove are its own are marked top-level nodes plus the placement nodes the FBX
    plugin adds beside the geometry. A namespace that also holds unmarked top-level
    nodes is *not* entirely ours: cleanup then deletes only the marked nodes and
    reports the rest, because the tool cannot tell a leftover of its own interrupted
    import from an object the scene put there.
    """
    marks = [mark for mark in accepted_marks if mark]
    state = {"namespace": name, "exists": bool(cmds.namespace(exists=name)),
             "marked": [], "foreign": []}
    for path in _namespace_top_nodes(name):
        token = _ownership_token(path)
        if token in marks:
            state["marked"].append(path)
        elif _node_type_in(path, TOLERATED_CONTAINER_TOP_TYPES):
            continue
        else:
            state["foreign"].append(path)
    state["foreign"].extend(_foreign_descendants(_namespace_top_nodes(name), name))
    state["ours_only"] = bool(state["marked"]) and not state["foreign"]
    return state


def _remove_owned_nodes(namespace, accepted_marks):
    """Delete the top level nodes of one namespace that carry an accepted mark.

    Returns ``(removed, left, errors)``: the marked nodes that are gone, the
    top-level nodes that are still there (unmarked ones are never deleted) and the
    host's messages for the deletions it refused.
    """
    marks = [mark for mark in accepted_marks if mark]
    removed = []
    left = []
    errors = []
    for path in _namespace_top_nodes(namespace):
        token = _ownership_token(path)
        if token not in marks:
            if not _node_type_in(path, TOLERATED_CONTAINER_TOP_TYPES):
                left.append(path)
            continue
        foreign = _foreign_descendants([path], namespace)
        if foreign:
            # Deleting the group would delete the scene's nodes under it.
            errors.append("the marked node {0} holds nodes this tool did not "
                          "create: {1}".format(path, _node_summary(foreign)))
            left.append(path)
            continue
        try:
            cmds.delete(path)
        except Exception as error:  # noqa: BLE001 - reported, never raised
            errors.append("the marked node {0} could not be deleted: {1}".format(path,
                                                                                error))
            left.append(path)
            continue
        if cmds.objExists(path):
            errors.append("the marked node {0} is still there after the host "
                          "accepted its deletion".format(path))
            left.append(path)
        else:
            removed.append(path)
    return removed, left, errors


def _remove_namespace(namespace):
    """Delete a namespace and everything in it; never raises.

    Returns ``(removed, error)``. A host that refuses the removal is a cleanup
    failure the caller records, so a run always returns its report and names the
    resources it could not delete instead of raising out of the API.
    """
    if not namespace or not cmds.namespace(exists=namespace):
        return False, None
    try:
        cmds.namespace(removeNamespace=namespace, deleteNamespaceContent=True)
    except Exception as error:  # noqa: BLE001 - reported by the caller
        return False, "the namespace {0!r} could not be deleted: {1}".format(
            namespace, error)
    if cmds.namespace(exists=namespace):
        return False, ("the namespace {0!r} is still there after the host accepted "
                       "its removal".format(namespace))
    return True, None


def _namespace_members(namespace):
    return list(cmds.namespaceInfo(namespace, listNamespace=True) or [])


def _residual_state(namespace):
    """What this tool still occupies of one namespace, read after a cleanup."""
    state = {"namespace": namespace, "namespace_present": False, "nodes": []}
    try:
        state["namespace_present"] = bool(cmds.namespace(exists=namespace))
        state["nodes"] = _namespace_members(namespace)
    except Exception:  # noqa: BLE001 - reporting must never fail a run
        state["unreadable"] = traceback.format_exc().splitlines()[-1]
    return state


def _container_state(name):
    """What the scene holds under ``name``, and whether it may be replaced.

    The answer comes from the nodes themselves, never from the name alone: a
    container is replaceable only while the group inside it -- or, for a build that
    wrote no group yet, the root level node of that name -- carries the ownership
    mark this tool writes with ``name``. Everything else found under that name is
    reported as foreign, so the caller can refuse instead of deleting scene content.
    """
    mark = _ownership_mark(name)
    state = {"name": name,
             "namespace": bool(cmds.namespace(exists=name)),
             "group": None, "root_group": None,
             "incomplete": [], "placement_nodes": [], "foreign_nodes": [],
             "conflict": None}
    for path in _namespace_top_nodes(name):
        token = _ownership_token(path)
        short = _short_node_name(path)
        if token == mark and short == name:
            state["group"] = path
        elif token is not None:
            # A marked group of another name inside this namespace is what an
            # interrupted takeover leaves behind; it is this tool's, so it is
            # recovered instead of refused.
            state["incomplete"].append(path)
        elif _node_type_in(path, TOLERATED_CONTAINER_TOP_TYPES):
            state["placement_nodes"].append(path)
        else:
            state["foreign_nodes"].append(path)
    for path in _root_nodes(name):
        token = _ownership_token(path)
        if token == mark:
            state["root_group"] = path
        elif token is not None:
            state["incomplete"].append(path)
        else:
            state["foreign_nodes"].append(path)
    state["foreign_nodes"].extend(_foreign_descendants(_namespace_top_nodes(name), name))
    if state["foreign_nodes"]:
        state["conflict"] = (
            "the name {0!r} is used by nodes this tool does not own and never "
            "deletes: {1}; rename or remove them, or choose another container "
            "name".format(name, _node_summary(state["foreign_nodes"])))
    elif state["incomplete"]:
        state["conflict"] = (
            "the namespace {0!r} holds a group without this tool's ownership mark "
            "naming it: {1}; the run does not touch it".format(
                name, _node_summary(state["incomplete"])))
    elif state["namespace"] and not state["group"]:
        state["conflict"] = (
            "the namespace {0!r} exists but holds no container group with this "
            "tool's ownership mark ({1}), so it cannot be told apart from scene "
            "content; rename or remove it, or choose another container name".format(
                name, REFERENCE_MARK_ATTRIBUTE))
    state["report"] = {
        "namespace": state["namespace"],
        "group": state["group"],
        "root_group": state["root_group"],
        "incomplete": state["incomplete"],
        "placement_nodes": state["placement_nodes"],
        "foreign_nodes": state["foreign_nodes"],
        "owned": bool(state["group"] or state["root_group"]),
    }
    return state


def _namespace_top_nodes(namespace):
    """Long paths of the DAG nodes sitting at the top of one namespace.

    A node at the top of a namespace has a one step long path; anything deeper
    hangs under one of those nodes, so only the top level answers whether the
    namespace holds content of its own.
    """
    nodes = cmds.ls(namespace + ":*", long=True, type="transform") or []
    return [path for path in nodes if str(path).count("|") == 1]


def _foreign_descendants(paths, namespace):
    """DAG nodes under ``paths`` that the import into ``namespace`` did not create.

    Everything an import creates lives in its namespace, so a node of another
    namespace hanging anywhere below the container -- a scene object a person
    parented under a reference mesh, for example -- belongs to the scene.
    Deleting the namespace would delete it with its parent, so its presence
    makes the container not entirely this tool's.
    """
    found = []
    for path in paths:
        try:
            below = cmds.listRelatives(path, allDescendents=True, fullPath=True) or []
        except Exception:  # noqa: BLE001 - an unreadable tree is not provably ours
            found.append(path)
            continue
        for node in below:
            leaf = str(node).rsplit("|", 1)[-1]
            node_namespace = leaf.rsplit(":", 1)[0] if ":" in leaf else ""
            if node_namespace != namespace                     and not node_namespace.startswith(namespace + ":"):
                found.append(str(node))
    return found


def _root_nodes(name):
    """Long paths of the nodes carrying ``name`` at the top of the scene."""
    nodes = cmds.ls(name, type="transform", long=True) or []
    return [path for path in nodes
            if str(path).count("|") == 1 and ":" not in str(path)]


def _node_type_in(path, types):
    """Whether the node's type is one of ``types``."""
    try:
        return str(cmds.nodeType(path)) in types
    except Exception:  # noqa: BLE001 - an unreadable type is not a tolerated one
        return False


def _node_summary(paths, limit=6):
    """A short, readable list of node paths for a report line."""
    shown = [str(path) for path in paths[:limit]]
    if len(paths) > limit:
        shown.append("... {0} more".format(len(paths) - limit))
    return ", ".join(shown)


def _short_node_name(path):
    """Short name of a (possibly namespaced, possibly long) node path."""
    return str(path).rsplit("|", 1)[-1].split(":", 1)[-1]


def _retire_container(container, previous_state):
    """Move the previous, owned reference to a free retiring name.

    Renaming, never deleting: the previous reference keeps every node and its
    UUID, so a takeover that fails anywhere before it is complete puts it back
    under the container's name without changing what scene references point at.
    """
    state = previous_state or _container_state(container)
    previous_group = state["group"] or state["root_group"]
    if not previous_group:
        return None
    retired = {"previous_group": previous_group, "retired_namespace": None,
               "retired_root_group": None, "previous_uuid": None}
    uuid_values = cmds.ls(previous_group, uuid=True) or []
    retired["previous_uuid"] = uuid_values[0] if uuid_values else None
    if state["group"]:
        name = _free_name(container + RETIRING_SUFFIX)
        cmds.namespace(rename=(container, name))
        retired["retired_namespace"] = name
    if state["root_group"]:
        name = _free_name(container + RETIRING_SUFFIX)
        cmds.rename(state["root_group"], name)
        retired["retired_root_group"] = name
    return retired


def _remove_retired(retired):
    """Delete the retired previous reference once the takeover is complete."""
    if not retired:
        return {"removed": False, "namespace_removed": False,
                "root_group_removed": False, "errors": []}
    errors = []
    namespace_removed = False
    if retired.get("retired_namespace"):
        namespace_removed, error = _remove_namespace(retired["retired_namespace"])
        if error:
            errors.append(error)
    root_group_removed = False
    if retired.get("retired_root_group"):
        try:
            if cmds.objExists(retired["retired_root_group"]):
                cmds.delete(retired["retired_root_group"])
                root_group_removed = True
        except Exception as error:  # noqa: BLE001 - reported, the run continues
            errors.append("the retired group {0} could not be deleted: {1}".format(
                retired["retired_root_group"], error))
    return {"removed": bool(namespace_removed or root_group_removed),
            "namespace_removed": namespace_removed,
            "root_group_removed": root_group_removed, "errors": errors}


def _restore_retired(container, retired):
    """Put the retired previous reference back under the container's name.

    A restore that the host refuses is reported, never raised: the caller has to
    keep the scene's real state on the record, and the previous reference is still
    in its retiring name in that case.
    """
    if not retired:
        return {"restored": False, "note": "no previous reference was retired"}
    restored = {"restored": False, "namespace": False, "root_group": False,
                "note": None, "errors": []}
    if retired.get("retired_namespace"):
        if cmds.namespace(exists=container):
            restored["errors"].append(
                "the container name {0!r} is still taken, so the retired namespace "
                "{1!r} was left in place".format(container,
                                                 retired["retired_namespace"]))
            restored["note"] = ("the previous reference stays in its retiring name "
                                "and is recoverable by the next run")
            return restored
        try:
            cmds.namespace(rename=(retired["retired_namespace"], container))
            restored["namespace"] = True
        except Exception as error:  # noqa: BLE001 - reported, never raised
            restored["errors"].append(
                "the retired namespace {0} could not be renamed back to {1}: "
                "{2}".format(retired["retired_namespace"], container, error))
    if retired.get("retired_root_group"):
        try:
            cmds.rename(retired["retired_root_group"], container)
            restored["root_group"] = True
        except Exception as error:  # noqa: BLE001 - reported, never raised
            restored["errors"].append(
                "the retired group {0} could not be renamed back to {1}: {2}".format(
                    retired["retired_root_group"], container, error))
    restored["restored"] = bool(restored["namespace"] or restored["root_group"])
    if not restored["restored"]:
        restored["note"] = (
            "the previous reference could not be put back under the container's "
            "name" if restored["errors"]
            else "the previous reference was already removed")
    return restored


def _remove_created_container(container, accepted_marks):
    """Delete a container namespace this run created out of its staging import.

    Only a namespace that holds a marked group this run wrote is deleted: the mark
    it staged under (the staging name), or the mark it rewrote once the group had
    taken the container's name. A namespace with no such mark is left untouched and
    reported, because this run cannot prove it created it.
    """
    marks = [mark for mark in accepted_marks if mark]
    result = {"namespace_removed": False, "nodes_removed": [],
              "skipped_reason": None, "accepted_marks": marks}
    if not cmds.namespace(exists=container):
        return result
    if not any(_owns_marked_group(container, mark) for mark in marks):
        result["skipped_reason"] = (
            "the namespace {0!r} holds no group this run staged (accepted marks "
            "{1}), so it is left untouched".format(container, marks))
        return result
    result["nodes_removed"] = _namespace_members(container)
    removed, error = _remove_namespace(container)
    result["namespace_removed"] = bool(removed)
    if error:
        result["skipped_reason"] = error
    return result


def _finish_interrupted_container(container, state):
    """Rename and re-mark the group of a container an interrupted run left behind.

    The container namespace holds a group this tool staged -- its mark names the
    staging family -- and no previous reference is waiting in a retiring name, so
    the interrupted run had already deleted it. Completing the takeover (the group
    name and mark the contract promises) is then the only way back to a state a
    later run can work with: the staged content itself was verified before its
    takeover started, so the group name and the mark are what the interruption can
    have cost it.

    This runs only while no retiring copy exists and the namespace holds no node
    the tool does not own: a completion mark is never written next to an unresolved
    previous reference, and one marked group of an unknown arrangement is not an
    authorisation to own a namespace. Returns whether the group was finished.
    """
    incomplete = list(state["incomplete"])
    if len(incomplete) != 1:
        return False
    node = incomplete[0]
    if _short_node_name(node) != container:
        found = cmds.ls(container + ":" + container, long=True,
                        type="transform") or []
        node = found[0] if found else cmds.rename(
            node, container + ":" + container)
    _mark_owned(node, container)
    return True


def _recover_previous_runs(container):
    """Undo or finish what an interrupted run of this tool left behind.

    Only resources this tool can prove it created are touched, and a state the run
    cannot resolve is refused instead of guessed at.

    A namespace holding a previous reference in its retiring name is put back under
    the container's name -- but only while every one of its top-level nodes carries
    the container's mark, because a previous reference is restored or deleted whole,
    never in parts. One that also holds unmarked nodes is refused with its content
    named, so nobody can mistake an unknown arrangement for a finished takeover.

    A staging namespace of this tool's family is cleaned up node by node: the
    marked group is deleted, and a namespace that still holds unmarked nodes
    afterwards is left in place and reported instead of being emptied.

    The container name itself decides the rest. A half-swapped namespace -- one
    whose takeover was interrupted before the ownership mark was rewritten -- holds
    only this tool's staged import, so it is deleted whole and the retiring
    reference put back. That deletion refuses the run rather than working around a
    host refusal when the namespace also holds a node the tool does not own: neither
    putting the old reference back nor completing an unverified takeover is safe
    while the container's name is still taken. A finished takeover (its group
    carries the mark for the container's own name) is the reference a retiring copy
    was replaced by, so that copy is deleted; a host that refuses the deletion
    refuses the run instead of reporting a success with the copy still in the scene.

    Only when no previous reference is waiting in any retiring name does the run
    finish a half-swapped container by renaming and re-marking its group: the
    staged content was verified before its takeover started, so the group name and
    the mark are what an interruption can have cost it, and writing the completion
    mark is then the only way back to a state a later run can work with. The mark
    is never written next to a retiring copy that still exists.
    """
    recovery = {"stale_staging_namespaces": [], "restored_previous_reference": None,
                "retired_leftovers_removed": [],
                "interrupted_container_removed": False,
                "finished_interrupted_container": None,
                "skipped_namespaces": [], "partial_cleanups": [],
                "events": []}
    container_state = _container_state(container)
    for name in _existing_family_names(container + RETIRING_SUFFIX):
        state = _namespace_ownership(name, [_ownership_mark(container)])
        if not state["exists"]:
            continue
        if not state["ours_only"]:
            raise ReferenceRefused(
                CODE_CONTAINER_NOT_OWNED,
                "the namespace {0!r} holds a previous reference of this tool next "
                "to nodes it does not own ({1}), so it is neither restored nor "
                "deleted; rename or remove it and run again".format(
                    name, _node_summary(state["foreign"]
                                        or _namespace_top_nodes(name))))
        if container_state["group"] or container_state["root_group"]:
            # The container under that name is a finished takeover, so this retiring
            # copy is the reference that takeover replaced. It is deleted as a whole
            # namespace or the run refuses with it still named: reporting a success
            # while an unhandled copy of a previous reference sits in the scene is
            # what a retry must never do.
            removed, error = _remove_namespace(name)
            if not removed:
                raise ReferenceRefused(
                    CODE_RECOVERY_REFUSED,
                    "the retired reference {0!r} of the finished container could "
                    "not be deleted ({1}), so the run is refused instead of "
                    "reporting a success with the copy still in the scene; run again "
                    "once the name can be removed".format(name, error))
            recovery["retired_leftovers_removed"].append(name)
            recovery["events"].append(
                "RETIRED_LEFTOVER_REMOVED: the interrupted run's previous reference "
                "{0} was a copy of the verified container and has been "
                "deleted".format(name))
            continue
        if container_state["incomplete"]:
            # A half-swapped namespace: the takeover never wrote the mark that proves
            # it, so its top level holds nothing but this tool's staged import. A
            # namespace that also holds a node the tool does not own is refused with
            # its content named -- it is never emptied -- and a host that refuses the
            # removal refuses the run, because the previous reference stays in its
            # retiring name and the container's name stays taken by an unverified
            # takeover until it is dealt with.
            if container_state["foreign_nodes"]:
                raise ReferenceRefused(
                    CODE_CONTAINER_NOT_OWNED,
                    "the namespace {0!r} was left half swapped by an interrupted run "
                    "next to nodes this tool does not own ({1}), so the previous "
                    "reference {2!r} is neither restored nor deleted; rename or "
                    "remove the foreign nodes and run again".format(
                        container,
                        _node_summary(container_state["foreign_nodes"]), name))
            removed, error = _remove_namespace(container)
            recovery["interrupted_container_removed"] = bool(removed)
            if not removed:
                raise ReferenceRefused(
                    CODE_RECOVERY_REFUSED,
                    "the half swapped namespace {0!r} could not be deleted ({1}), so "
                    "the previous reference {2!r} stays in its retiring name and the "
                    "run is refused instead of completing a takeover that was never "
                    "verified; run again once the name can be removed".format(
                        container, error, name))
            recovery["events"].append(
                "INTERRUPTED_CONTAINER_REMOVED: the half swapped namespace {0} was "
                "left by an interrupted run and has been deleted".format(container))
        try:
            cmds.namespace(rename=(name, container))
        except Exception as error:  # noqa: BLE001 - reported, never raised
            raise ReferenceRefused(
                CODE_RECOVERY_REFUSED,
                "the previous reference {0!r} could not be put back under the "
                "container's name {1!r} ({2}); it stays in its retiring name until "
                "a run can restore it".format(name, container, error))
        recovery["restored_previous_reference"] = name
        recovery["events"].append(
            "RESTORED_PREVIOUS_REFERENCE: the previous reference {0} was put back "
            "under the container's name {1} after an interrupted run".format(
                name, container))
        container_state = _container_state(container)
    if (not recovery["restored_previous_reference"]
            and not container_state["group"] and not container_state["root_group"]
            and container_state["incomplete"]
            and not container_state["foreign_nodes"]):
        # The container namespace holds a group this tool staged but never renamed
        # and no previous reference is waiting in a retiring name, so the interrupted
        # run had already deleted it: finishing the takeover is what restores the
        # state the contract promises instead of refusing to run. A namespace that
        # also holds foreign nodes never reaches this point -- with a retiring copy
        # it is refused above, without one the container's own conflict check refuses
        # it before anything is imported.
        finished = _finish_interrupted_container(container, container_state)
        recovery["finished_interrupted_container"] = finished
        if finished:
            recovery["events"].append(
                "FINISHED_INTERRUPTED_TAKEOVER: the namespace {0} was left by an "
                "interrupted run and its group has been renamed and marked as the "
                "container".format(container))
            container_state = _container_state(container)
    for name in _existing_family_names(container + STAGING_SUFFIX):
        state = _namespace_ownership(name, [_ownership_mark(name)])
        if not state["exists"] or not state["marked"]:
            if state["exists"]:
                recovery["skipped_namespaces"].append(name)
            continue
        removed, left, errors = _remove_owned_nodes(name, [_ownership_mark(name)])
        if left or errors:
            recovery["partial_cleanups"].append({
                "namespace": name, "removed": removed, "left": left, "errors": errors})
            recovery["events"].append(
                "STALE_STAGING_PARTIAL: the staging namespace {0} held nodes this "
                "tool cannot prove are its own ({1}), so only its marked group was "
                "deleted and the rest was left exactly as it is".format(
                    name, _node_summary(left)))
            continue
        if not _namespace_top_nodes(name):
            removed_namespace, error = _remove_namespace(name)
            if removed_namespace:
                recovery["stale_staging_namespaces"].append(name)
            else:
                recovery["events"].append(
                    "STALE_STAGING_KEPT: the staging namespace {0} could not be "
                    "deleted: {1}".format(name, error))
        else:
            recovery["partial_cleanups"].append({
                "namespace": name, "removed": removed, "left": [], "errors": []})
    return recovery


# --------------------------------------------------------- the Maya readers

def _match_container_node(index, node_name):
    """``(path, matched_by, other_candidates)`` for one manifest node name.

    The engine writes the actor label, the mesh name, or an instance index; Maya
    may have appended a numeric deduplication suffix when the container was kept,
    so an exact short name wins and a suffixed one is reported as such. A missing
    node returns ``None`` so the caller reports it as a problem or falls back to
    matching by world position, never as a skipped comparison.
    """
    if not node_name:
        return None, None, []
    exact = index.get(node_name) or []
    if exact:
        return exact[0], "name", sorted(exact[1:])
    pattern = re.compile(re.escape(node_name) + r"(\d+)$")
    suffixed = []
    for short_name, paths in index.items():
        match = pattern.fullmatch(short_name)
        if match is not None:
            suffixed.append((int(match.group(1)), short_name, paths))
    if not suffixed:
        return None, None, []
    suffixed.sort()
    _, short_name, paths = suffixed[0]
    others = [path for _, _, group in suffixed[1:] for path in group]
    return paths[0], "name", sorted(others + paths[1:])


def _world_bounds(path):
    """``(min, max)`` of a node's world bounding box, in centimetres.

    The centre of that box is where the reference geometry actually sits, so the
    vector from the node's world position to it is the measurement that a mirrored
    arrival moves while position and size stay put.
    """
    shapes = cmds.listRelatives(path, shapes=True, fullPath=True) or []
    targets = shapes if shapes else [path]
    box = cmds.exactWorldBoundingBox(*targets)
    return ([float(box[0]), float(box[1]), float(box[2])],
            [float(box[3]), float(box[4]), float(box[5])])


def _local_bounds_size(path):
    """Local bounding-box size of the node's shapes, for the orientation check.

    Shapes are assumed to share the transform's space, which is what the FBX plugin
    builds; a shape's own ``boundingBoxMin/Max`` are in its object space.
    """
    shapes = cmds.listRelatives(path, shapes=True, fullPath=True) or []
    low = None
    high = None
    for shape in shapes:
        try:
            minimum = cmds.getAttr(shape + ".boundingBoxMin")
            maximum = cmds.getAttr(shape + ".boundingBoxMax")
        except (RuntimeError, TypeError, IndexError):
            continue
        if not isinstance(minimum, list) or not isinstance(maximum, list):
            continue
        if not minimum or not maximum:
            continue
        minimum = minimum[0]
        maximum = maximum[0]
        if len(minimum) != 3 or len(maximum) != 3:
            continue
        values_low = [float(value) for value in minimum]
        values_high = [float(value) for value in maximum]
        low = values_low if low is None else [min(low[axis], values_low[axis])
                                              for axis in range(3)]
        high = values_high if high is None else [max(high[axis], values_high[axis])
                                                 for axis in range(3)]
    if low is None or high is None:
        return None
    return [high[axis] - low[axis] for axis in range(3)]


def _world_surface_centroid(path, cap=CENTROID_FACE_CAP):
    """``(centroid, faces_used, triangles_used, sampled)`` of a node's shapes.

    The area-weighted centroid of the mesh's triangles in world space:
    ``sum(area * triangle_centre) / sum(area)``, each face fan-triangulated from
    its first vertex. This is the one measurement of the geometry a mirrored
    placement moves that the exported file can reproduce -- the exporter welds
    duplicated render vertices away, so a *vertex* average compares two different
    vertex sets -- and it moves even for a mesh whose bounding box is centred on
    its pivot, such as a cone. Reading every face of a level mesh would be
    unbounded, so at most ``cap`` faces are accumulated: a mesh at or below the cap
    is read whole, a larger one is sampled evenly and reports ``sampled``.
    """
    selection = om.MSelectionList()
    meshes = []
    for shape in cmds.listRelatives(path, shapes=True, fullPath=True) or []:
        try:
            if cmds.nodeType(shape) != "mesh":
                continue
            selection.add(shape)
            meshes.append(om.MFnMesh(selection.getDagPath(0)))
            selection.clear()
        except Exception:  # noqa: BLE001 - a shape without faces is not a mesh
            selection.clear()
            continue
    if not meshes:
        return None, 0, 0, False
    face_total = sum(mesh.numPolygons for mesh in meshes)
    if face_total <= 0:
        return None, 0, 0, False
    stride = max(1, -(-face_total // cap))  # ceil division: at most ``cap`` faces
    weighted = [0.0, 0.0, 0.0]
    total_area = 0.0
    faces_used = 0
    triangles_used = 0
    for mesh in meshes:
        points = mesh.getPoints(om.MSpace.kWorld)
        for face in range(0, mesh.numPolygons, stride):
            corners = mesh.getPolygonVertices(face)
            if len(corners) < 3:
                continue
            faces_used += 1
            first = points[corners[0]]
            for step in range(1, len(corners) - 1):
                second = points[corners[step]]
                third = points[corners[step + 1]]
                edge_one = om.MVector(second - first)
                edge_two = om.MVector(third - first)
                area = 0.5 * (edge_one ^ edge_two).length()
                if area <= 0.0:
                    continue
                triangles_used += 1
                for axis in range(3):
                    weighted[axis] += area * (first[axis] + second[axis]
                                              + third[axis]) / 3.0
                total_area += area
    if total_area <= 0.0 or triangles_used <= 0:
        return None, faces_used, triangles_used, stride > 1
    return ([value / total_area for value in weighted], faces_used, triangles_used,
            stride > 1)


def _identification_size(local_size, world_matrix):
    """The extents that decide whether a mesh's axes can be identified.

    The shape's local bounding-box extents are scaled by the node's own axis scale
    factors -- the lengths of the world matrix's axis rows -- because a non-uniform
    scale separates axes that a symmetric mesh leaves ambiguous. A cube whose world
    matrix scales it ``(2, 0.5, 0.25)`` produces images of three different lengths,
    so an axis permutation or a sign flip changes the geometry and must be reported;
    the same cube at uniform scale does not.
    """
    if not local_size or len(local_size) != 3 or not world_matrix:
        return None
    rows = matrix4_linear(world_matrix)
    return [float(local_size[axis]) * vector_length(rows[axis]) for axis in range(3)]


def _file_node_image_path(node):
    """The image path a ``file`` node names, or ``None``."""
    try:
        image = cmds.getAttr(node + ".fileTextureName")
    except Exception:  # noqa: BLE001 - a missing attribute is just not loaded
        return None
    return str(image) if image else None


def _file_node_image_exists(node):
    """Whether a ``file`` node's image path exists on disk."""
    image = _file_node_image_path(node)
    if not image:
        return False
    try:
        return os.path.exists(image)
    except (OSError, ValueError):
        return False


def stale_reference(anchor, container=REFERENCE_CONTAINER):
    """Why the scene reference cannot enter linkage at ``anchor``, or None.

    ``anchor`` is the linkage target's current placement (``world.anchor``
    shaped). No tool-owned container means there is nothing to check. A
    container whose recorded anchor differs, or that records none (imported
    by an earlier version), was placed for another frame: it is kept, but it
    is not the current reference.
    """
    require_maya()
    state = _container_state(container)
    group = state["group"] or state["root_group"]
    if not group:
        return None
    try:
        if not cmds.attributeQuery(
                REFERENCE_ANCHOR_ATTRIBUTE, node=group, exists=True):
            return ("the reference {0!r} records no anchor; it was imported by "
                    "an earlier version".format(container))
        recorded = json.loads(cmds.getAttr(group + "." + REFERENCE_ANCHOR_ATTRIBUTE))
    except Exception as error:  # noqa: BLE001 - an unreadable record is stale
        return "the reference {0!r} anchor cannot be read: {1}".format(
            container, error)
    return anchor_difference(recorded, anchor)


class ReferenceContainer(object):
    """The tool-owned scene-reference container: remove or recover it.

    ``remove`` deletes only the container this tool created, and ``recover``
    finishes or discards what an interrupted run left behind. Both refuse a name
    they cannot prove they own instead of deleting scene content, and both report
    the scene state they left behind.
    """

    def __init__(self, container=REFERENCE_CONTAINER, space_revision=None, budget=None):
        self.container = str(container)
        self.space_revision = space_revision
        self.budget = _budget(budget)

    def remove(self):
        """Delete the tool-owned container, and nothing else."""
        require_maya()
        report = _new_handoff_report("remove")
        report["container"] = {"name": self.container}
        report["space_revision"] = self.space_revision
        state = _container_state(self.container)
        if state["conflict"]:
            raise ReferenceRefused(CODE_CONTAINER_NOT_OWNED, state["conflict"])
        if not (state["group"] or state["root_group"]):
            report["removed"] = False
            report["namespace_removed"] = False
            report["nodes_removed"] = 0
            report["detail"] = ("no tool-owned reference container named {0!r} is "
                                "in the scene".format(self.container))
            report["residual"] = _residual_state(self.container)
            report["accepted"] = True
            return report
        nodes = _namespace_members(self.container) + list(
            _root_nodes(self.container))
        namespace_removed, error = _remove_namespace(self.container)
        root_removed = False
        if state["root_group"] and not namespace_removed:
            try:
                if cmds.objExists(state["root_group"]):
                    cmds.delete(state["root_group"])
                    root_removed = not cmds.objExists(state["root_group"])
            except Exception as delete_error:  # noqa: BLE001 - reported
                error = "{0}; {1}".format(error, delete_error)
        removed = bool(namespace_removed or root_removed)
        report["removed"] = removed
        report["namespace_removed"] = namespace_removed
        report["nodes_removed"] = len(nodes)
        report["residual"] = _residual_state(self.container)
        if not removed:
            detail = error or ("the reference container {0!r} could not be "
                               "deleted".format(self.container))
            report["detail"] = detail
            report["problems"].append("CONTAINER_REMOVAL_FAILED: {0}".format(detail))
            return report
        report["detail"] = ("removed the reference container {0!r} with {1} "
                            "node(s)".format(self.container, len(nodes)))
        report["accepted"] = True
        return report

    def recover(self):
        """Finish or discard what an interrupted run of this tool left behind."""
        require_maya()
        report = _new_handoff_report("recover")
        report["container"] = {"name": self.container}
        report["space_revision"] = self.space_revision
        recovery = _recover_previous_runs(self.container)
        report.update(recovery)
        state = _container_state(self.container)
        report["container"].update({
            "namespace": state["namespace"],
            "group_path": state["group"] or state["root_group"],
            "after_run": {"namespace": state["namespace"],
                          "group_path": state["group"] or state["root_group"],
                          "incomplete_groups": state["incomplete"],
                          "foreign_nodes": state["foreign_nodes"]},
        })
        report["residual"] = {"namespace": self.container,
                              "namespace_present": state["namespace"],
                              "nodes": state["foreign_nodes"] + state["incomplete"]}
        report["accepted"] = True
        report["detail"] = _recovery_detail(recovery)
        return report


def _recovery_detail(recovery):
    """One status line for a recovery run, naming what it did."""
    parts = []
    if recovery.get("restored_previous_reference"):
        parts.append("put the reference {0} back".format(
            recovery["restored_previous_reference"]))
    if recovery.get("finished_interrupted_container"):
        parts.append("finished the interrupted container")
    if recovery.get("interrupted_container_removed"):
        parts.append("removed the half swapped container")
    for name in recovery.get("retired_leftovers_removed") or []:
        parts.append("deleted the retired copy {0}".format(name))
    for name in recovery.get("stale_staging_namespaces") or []:
        parts.append("swept the staging namespace {0}".format(name))
    if not parts:
        return "no interrupted reference update was found"
    return "; ".join(parts)
