"""Mayapy host checks for the scene reference importer.

Runs in a disposable scene and never saves anything. The checks cover, against
handoffs this host authored itself:

* the import of an ASCII level FBX into a fresh container, the container
  namespace and group, the gray material assignment and zero texture nodes;
* a repeat run replacing only its own container while a production cube, its
  material and its keys are untouched, and the scene's playback range, frame
  rate and current time stay where the run found them;
* the transform comparison against a manifest synthesized from known scene
  values, including the least-squares fit, and one deliberately wrong value that
  must be reported as a failure;
* matching by world position when a node name does not survive, and a missing
  node reported as a problem;
* the refusal paths: a handoff that records a texture, a file that is not FBX, a
  missing file, a manifest with another schema, and a scene or manifest that is
  not in centimetres;
* the contract's report shape and the command line entry point in its own
  process.

    <mayapy> maya_host_scene_ref_tests.py --result host_result.json [--keep-scratch]
"""

import argparse
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import traceback
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SCRIPTS = ROOT / "scripts"
for entry in (str(HERE), str(SCRIPTS)):
    if entry not in sys.path:
        sys.path.insert(0, entry)

#: Reference geometry: name, translation, rotation, cube dimensions in cm. The
#: four positions are not coplanar, so the least-squares fit is determined.
TARGETS = (
    ("SM_Pillar", (100.0, 200.0, -300.0), (0.0, 0.0, 0.0), (100.0, 200.0, 50.0)),
    ("SM_Rock", (-450.0, 25.0, 900.0), (0.0, 45.0, 0.0), (50.0, 50.0, 50.0)),
    ("SM_Wall", (1000.0, 0.0, 10.0), (0.0, 30.0, 0.0), (400.0, 20.0, 300.0)),
    ("SM_Beam", (-800.0, 1000.0, -1200.0), (15.0, 0.0, 0.0), (60.0, 300.0, 40.0)),
)

#: The axis map the camera-sync fixture is built with, so the importer has to
#: recover exactly this candidate from the measured Maya values.
EXPECTED_CANDIDATE = "maya_x=ue_y, maya_y=ue_z, maya_z=-ue_x"

#: The map the real engine handoff round-trips through, measured on 2026-09-28:
#: an Unreal world point (x, y, z) centimetres arrives at (x, z, y).
ENGINE_CANDIDATE = "maya_x=ue_x, maya_y=ue_z, maya_z=ue_y"

#: The camera route's map, which the handoff must not be mistaken for.
CAMERA_CANDIDATE = "maya_x=ue_y, maya_y=ue_z, maya_z=-ue_x"

#: Reference geometry for the engine-map case: three placed objects plus one
#: instanced child the engine names after its instance index.
ENGINE_TARGETS = (
    ("SM_Pillar", (1000.0, 300.0, -200.0), (0.0, 25.0, 0.0), (100.0, 200.0, 50.0)),
    ("SM_Wall", (-600.0, 50.0, 400.0), (0.0, -15.0, 0.0), (400.0, 20.0, 300.0)),
    ("SM_Beam", (200.0, -400.0, 900.0), (35.0, 0.0, 0.0), (60.0, 300.0, 40.0)),
)
ENGINE_INSTANCE_PARENT = "ISM_Cluster"
ENGINE_INSTANCE_CHILD = "SM_Inst_Child"
ENGINE_INSTANCE_POSITION = (250.0, 100.0, -50.0)

#: A mesh whose local bounding box sits off its pivot on every axis, with equal
#: extents so the orientation check skips it. A mirrored arrival keeps its
#: position and box size and moves the box centre, so this object is the one
#: that can see a mirror at all.
ENGINE_WEDGE = "SM_Wedge_Asymmetric"
ENGINE_WEDGE_VERTEX_OFFSET = (30.0, 50.0, 20.0)
ENGINE_WEDGE_PLACEMENT = ((900.0, -300.0, 250.0), (20.0, 35.0, 10.0), (1.5, 0.75, 2.0))

CONTRACT_REPORT_KEYS = {
    "schema": str, "ok": bool, "phase": str, "maya": dict, "container": dict,
    "counts": dict, "gray_material": dict, "transform_check": dict,
    "objects": list, "texture_scan": dict, "timing": dict, "memory": dict,
    "problems": list,
}
CONTRACT_MAYA_KEYS = {"version", "api", "linear_unit", "up_axis"}
CONTRACT_CONTAINER_KEYS = {"namespace", "group", "replaced"}
CONTRACT_COUNT_KEYS = {"manifest_objects", "container_nodes", "file_texture_nodes",
                       "image_nodes_loaded"}
CONTRACT_GRAY_KEYS = {"name", "type", "color"}
CONTRACT_CHECK_KEYS = {"candidate", "candidates", "best", "matched",
                       "max_position_error_cm", "max_size_error_cm"}
CONTRACT_OBJECT_KEYS = {"node_name", "path", "matched_id", "world_matrix",
                        "world_bounds_size_cm", "position_error_cm", "size_error_cm",
                        "found"}
CONTRACT_SCAN_KEYS = {"source", "texture_references", "files"}
CONTRACT_TIMING_KEYS = {"import_seconds", "verify_seconds"}
CONTRACT_MEMORY_KEYS = {"process_rss_mb"}


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, str(path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Checks(object):
    """Collects assertions instead of aborting on the first failure."""

    def __init__(self):
        self.results = []
        self.failures = []
        self.evidence = {}

    def check(self, label, condition, detail=""):
        entry = {"check": label, "ok": bool(condition), "detail": detail}
        self.results.append(entry)
        if not condition:
            self.failures.append("{0}: {1}".format(label, detail))
        return bool(condition)

    def close(self, label, first, second, tolerance=1e-9):
        try:
            ok = abs(float(first) - float(second)) <= tolerance
        except (TypeError, ValueError):
            ok = False
        return self.check(label, ok, "{0!r} vs {1!r} (tolerance {2})".format(
            first, second, tolerance))

    def record(self, key, value):
        self.evidence[key] = value


def scratch_directory():
    """A disposable directory under the repository's scratch folder."""
    repo = HERE.parents[6]
    for candidate in (repo.parent / ".tmp", repo / ".tmp"):
        if candidate.is_dir() and os.access(str(candidate), os.W_OK):
            return Path(tempfile.mkdtemp(prefix="mtou_scene_ref_host_", dir=str(candidate)))
    return Path(tempfile.mkdtemp(prefix="mtou_scene_ref_host_"))


def write_png(path):
    """A real 2x2 grey PNG, so an imported texture node has a file on disk."""
    def chunk(tag, payload):
        body = tag + payload
        return (len(payload).to_bytes(4, "big") + body
                + zlib.crc32(body).to_bytes(4, "big"))

    header = (b"\x89PNG\r\n\x1a\n"
              + chunk(b"IHDR", (2).to_bytes(4, "big") + (2).to_bytes(4, "big")
                      + bytes((8, 2, 0, 0, 0)))
              + chunk(b"IDAT", zlib.compress(b"\x00\x40\x40\x40\x40\x40\x40"
                                             b"\x00\x40\x40\x40\x40\x40\x40"))
              + chunk(b"IEND", b""))
    path.write_bytes(header)
    return path


def export_fbx(cmds, mel, path):
    """Export the whole scene as an ASCII FBX, the way the exporter would."""
    mel.eval("FBXResetExport")
    mel.eval("FBXExportInAscii -v true")
    mel.eval("FBXExportShapes -v false")
    mel.eval("FBXExportSkins -v false")
    mel.eval("FBXExportCameras -v false")
    mel.eval("FBXExportLights -v false")
    mel.eval("FBXExportEmbeddedTextures -v false")
    mel.eval("FBXExportConstraints -v false")
    mel.eval('FBXExport -f "{0}"'.format(str(path).replace("\\", "/")))
    return path


def author_reference(cmds, mel, directory):
    """Create the reference geometry and export the clean and textured handoffs."""
    cmds.file(new=True, force=True)
    cmds.currentUnit(linear="cm", time="film")
    for name, translation, rotation, size in TARGETS:
        node = cmds.polyCube(name=name, width=size[0], height=size[1], depth=size[2])[0]
        cmds.move(translation[0], translation[1], translation[2], node, absolute=True,
                  worldSpace=True)
        if any(rotation):
            cmds.rotate(rotation[0], rotation[1], rotation[2], node, absolute=True,
                        worldSpace=True)
    clean = export_fbx(cmds, mel, directory / "SceneRef_Clean.fbx")

    texture = write_png(directory / "reference_texture.png")
    shader = cmds.shadingNode("lambert", asShader=True, name="M_SceneRef_Textured")
    file_node = cmds.shadingNode("file", asTexture=True, name="sceneRefFile")
    cmds.setAttr(file_node + ".fileTextureName", str(texture), type="string")
    cmds.connectAttr(file_node + ".outColor", shader + ".color", force=True)
    shading_group = cmds.sets(renderable=True, noSurfaceShader=True, empty=True,
                              name="M_SceneRef_TexturedSG")
    cmds.connectAttr(shader + ".outColor", shading_group + ".surfaceShader", force=True)
    shape = cmds.listRelatives("SM_Pillar", shapes=True, fullPath=True)[0]
    cmds.sets(shape, edit=True, forceElement=shading_group)
    textured = export_fbx(cmds, mel, directory / "SceneRef_Textured.fbx")
    return clean, textured, texture


def author_engine_reference(cmds, mel, directory, mapping):
    """Author the reference geometry the way the engine's handoff presents it.

    Three placed objects plus one instanced child. The child is authored with a
    readable name and renamed to the instance index ``0`` inside the exported
    ASCII FBX text, which is what the engine writes and what this host's FBX
    plugin escapes to ``FBXASC048`` on import, so the manifest can only match it
    by world position. Returns the FBX path and the authored measurements.
    """
    cmds.file(new=True, force=True)
    cmds.currentUnit(linear="cm", time="film")
    measured = []
    for name, translation, rotation, size in ENGINE_TARGETS:
        node = cmds.polyCube(name=name, width=size[0], height=size[1], depth=size[2])[0]
        cmds.move(translation[0], translation[1], translation[2], node, absolute=True,
                  worldSpace=True)
        if any(rotation):
            cmds.rotate(rotation[0], rotation[1], rotation[2], node, absolute=True,
                        worldSpace=True)
        measured.append({"node_name": name, "maya_matrix": _world_matrix(cmds, name),
                         "maya_size": _world_size(cmds, name),
                         "maya_offset": _world_offset(cmds, name),
                         "maya_centroid": _surface_centroid(cmds, name),
                         "local_size": _local_size(cmds, name)})
    translation, rotation, scale = ENGINE_WEDGE_PLACEMENT
    wedge = cmds.polyCube(name=ENGINE_WEDGE, width=100.0, height=100.0, depth=100.0)[0]
    wedge_shape = cmds.listRelatives(wedge, shapes=True, fullPath=True)[0]
    cmds.move(ENGINE_WEDGE_VERTEX_OFFSET[0], ENGINE_WEDGE_VERTEX_OFFSET[1],
              ENGINE_WEDGE_VERTEX_OFFSET[2], wedge_shape + ".vtx[*]", relative=True,
              objectSpace=True)
    cmds.move(translation[0], translation[1], translation[2], wedge, absolute=True,
              worldSpace=True)
    cmds.rotate(rotation[0], rotation[1], rotation[2], wedge, absolute=True,
                worldSpace=True)
    cmds.scale(scale[0], scale[1], scale[2], wedge, absolute=True)
    measured.append({"node_name": ENGINE_WEDGE, "maya_matrix": _world_matrix(cmds, wedge),
                     "maya_size": _world_size(cmds, wedge),
                     "maya_offset": _world_offset(cmds, wedge),
                     "maya_centroid": _surface_centroid(cmds, wedge),
                     "local_size": _local_size(cmds, wedge)})

    parent = cmds.createNode("transform", name=ENGINE_INSTANCE_PARENT)
    child = cmds.polyCube(name=ENGINE_INSTANCE_CHILD, width=50.0, height=50.0,
                          depth=50.0)[0]
    cmds.move(ENGINE_INSTANCE_POSITION[0], ENGINE_INSTANCE_POSITION[1],
              ENGINE_INSTANCE_POSITION[2], child, absolute=True, worldSpace=True)
    cmds.parent(child, parent, relative=True)
    measured.append({"node_name": "0", "maya_matrix": _world_matrix(cmds, child),
                     "maya_size": _world_size(cmds, child),
                     "maya_offset": _world_offset(cmds, child),
                     "maya_centroid": _surface_centroid(cmds, child),
                     "local_size": _local_size(cmds, child)})

    fbx = export_fbx(cmds, mel, directory / "SceneRef_EngineMap.fbx")
    text = fbx.read_text(encoding="utf-8", errors="replace")
    tagged = '"Model::{0}"'.format(ENGINE_INSTANCE_CHILD)
    if tagged not in text:
        raise AssertionError("{0} is not in the exported FBX".format(tagged))
    fbx.write_text(text.replace(tagged, '"Model::0"'), encoding="utf-8")
    return fbx, measured


def _world_matrix(cmds, name):
    return [float(value) for value in
            cmds.xform(name, query=True, worldSpace=True, matrix=True)]


def _world_size(cmds, name):
    """World bounding-box size of a node and its shapes, in centimetres."""
    box = cmds.exactWorldBoundingBox(name)
    return [box[3] - box[0], box[4] - box[1], box[5] - box[2]]


def _surface_centroid(cmds, name):
    """Area-weighted world space centroid of the node's shape triangles.

    Deliberately an independent implementation of the importer's reader: the
    faces come from ``polyInfo`` and the positions from ``xform``, so the fixture
    value is not produced by the code under test.
    """
    weighted = [0.0, 0.0, 0.0]
    total_area = 0.0
    for shape in cmds.listRelatives(name, shapes=True, fullPath=True) or []:
        info = cmds.polyInfo(shape, faceToVertex=True) or []
        world = cmds.xform(shape + ".vtx[*]", query=True, translation=True,
                           worldSpace=True) or []
        positions = [(world[index * 3], world[index * 3 + 1], world[index * 3 + 2])
                     for index in range(len(world) // 3)]
        for line in info:
            corners = [int(token) for token in line.split(":")[1].split()]
            for step in range(1, len(corners) - 1):
                first, second, third = (positions[corners[0]],
                                        positions[corners[step]],
                                        positions[corners[step + 1]])
                edges = [tuple(second[axis] - first[axis] for axis in range(3)),
                         tuple(third[axis] - first[axis] for axis in range(3))]
                cross = (edges[0][1] * edges[1][2] - edges[0][2] * edges[1][1],
                         edges[0][2] * edges[1][0] - edges[0][0] * edges[1][2],
                         edges[0][0] * edges[1][1] - edges[0][1] * edges[1][0])
                area = 0.5 * sum(value * value for value in cross) ** 0.5
                if area <= 0.0:
                    continue
                for axis in range(3):
                    weighted[axis] += area * (first[axis] + second[axis]
                                              + third[axis]) / 3.0
                total_area += area
    if total_area <= 0.0:
        return None
    return [value / total_area for value in weighted]


def _world_offset(cmds, name):
    """Vector from the node's world position to its world box centre."""
    box = cmds.exactWorldBoundingBox(name)
    position = _world_matrix(cmds, name)[12:15]
    return [(box[axis] + box[axis + 3]) / 2.0 - position[axis] for axis in range(3)]


def _local_size(cmds, name):
    """Local bounding-box size of the node's shapes, in centimetres."""
    shape = cmds.listRelatives(name, shapes=True, fullPath=True)[0]
    low = cmds.getAttr(shape + ".boundingBoxMin")[0]
    high = cmds.getAttr(shape + ".boundingBoxMax")[0]
    return [high[axis] - low[axis] for axis in range(3)]


def engine_matrix(mapping):
    """The map the engine's FBX level export round-trips through."""
    for candidate in mapping.AXIS_CANDIDATES:
        if candidate["name"] == ENGINE_CANDIDATE:
            return mapping.matrix3(candidate["matrix"])
    raise AssertionError("the engine handoff map is not among the candidates")


def camera_matrix(mapping):
    """The camera route's Unreal to Maya map."""
    for candidate in mapping.AXIS_CANDIDATES:
        if candidate["name"] == CAMERA_CANDIDATE:
            return mapping.matrix3(candidate["matrix"])
    raise AssertionError("the camera route map is not among the candidates")


def build_engine_manifest(measured, scan, directory, mapping):
    """The manifest the engine would write for the engine-map handoff."""
    engine = engine_matrix(mapping)
    manifest = build_manifest([], scan, directory, mapping)
    manifest["objects"] = []
    for entry in measured:
        unreal = mapping.uncarry_world_matrix(engine, entry["maya_matrix"])
        inverse = mapping.matrix3_inverse(engine)
        size = mapping.expected_axis_sizes(inverse, entry["maya_size"])
        position = mapping.matrix4_translation(unreal)
        offset = manifest_offset(entry, mapping, inverse)
        centroid = mapping.matrix3_apply(inverse, entry["maya_centroid"])
        manifest["objects"].append({
            "id": "static_mesh_actor:{0}:component:StaticMeshComponent0:instance:none".format(
                entry["node_name"]),
            "node_name": entry["node_name"],
            "actor": entry["node_name"],
            "actor_class": "StaticMeshActor",
            "level": "/Game/SceneRef/Map",
            "category": "static_mesh",
            "component": "StaticMeshComponent0",
            "mesh": "/Engine/BasicShapes/Cube.Cube",
            "instance_index": None,
            "exported": True,
            "note": "",
            "materials": ["/Engine/BasicShapes/BasicShapeMaterial"],
            "world_location_cm": {"x": unreal[12], "y": unreal[13], "z": unreal[14]},
            "world_rotation_deg": {"roll": 0.0, "pitch": 0.0, "yaw": 0.0},
            "world_scale": {"x": 1.0, "y": 1.0, "z": 1.0},
            "world_matrix": unreal,
            "world_surface_centroid_cm": centroid_point(centroid),
            "world_bounds_cm": bounds_from(position, offset, size),
        })
    manifest["scale"] = {"actors": len(manifest["objects"]),
                         "components": len(manifest["objects"]),
                         "objects": len(manifest["objects"]), "triangles": 0,
                         "vertices": 0}
    manifest["output"] = {
        "directory": str(directory),
        "geometry_file": "SceneRef_EngineMap.fbx",
        "geometry_bytes": scan["bytes"],
        "files": [{"name": "SceneRef_EngineMap.fbx", "bytes": scan["bytes"]}],
        "image_files": [],
        "texture_records": scan["texture_records"],
        "texture_references": scan["texture_references"],
        "video_references": scan["video_references"],
        "texture_reference_files": scan["files"],
        "texture_reference_files_present": [],
        "node_names": [entry["node_name"] for entry in measured],
    }
    return manifest


def manifest_offset(entry, mapping, inverse):
    """The object's position to bounds-centre vector in Unreal terms."""
    box = entry["maya_matrix"]
    position = mapping.matrix4_translation(box)
    centre = [position[axis] + entry["maya_offset"][axis] for axis in range(3)]
    return mapping.matrix3_apply(inverse, [centre[axis] - position[axis]
                                           for axis in range(3)])


def centroid_point(centroid):
    """A manifest ``world_surface_centroid_cm`` block from a vector."""
    return {axis: centroid[index] for index, axis in enumerate(("x", "y", "z"))}


def mirror_object_centroid(entry, axis="z"):
    """Mirror one manifest object's vertex centroid about its world position."""
    position = [entry["world_location_cm"][name] for name in ("x", "y", "z")]
    centroid = [entry["world_surface_centroid_cm"][name] for name in ("x", "y", "z")]
    index = ("x", "y", "z").index(axis)
    centroid[index] = 2.0 * position[index] - centroid[index]
    entry["world_surface_centroid_cm"] = centroid_point(centroid)
    return entry


def bounds_from(position, offset, size):
    """Manifest ``world_bounds_cm`` for a position, an offset and a size."""
    centre = [position[axis] + offset[axis] for axis in range(3)]
    return {
        "min": {axis: centre[index] - size[index] / 2.0
                for index, axis in enumerate(("x", "y", "z"))},
        "max": {axis: centre[index] + size[index] / 2.0
                for index, axis in enumerate(("x", "y", "z"))},
        "size": {axis: size[index] for index, axis in enumerate(("x", "y", "z"))},
    }


def mirror_object_offset(entry, axis="x"):
    """Mirror one manifest object's bounds centre about its position.

    This is what a mirrored arrival looks like in the manifest: the position and
    the box size stay, the box centre moves to the other side of the pivot.
    """
    bounds = entry["world_bounds_cm"]
    position = [entry["world_location_cm"][name] for name in ("x", "y", "z")]
    size = [bounds["size"][name] for name in ("x", "y", "z")]
    offset = [(bounds["min"][name] + bounds["max"][name]) / 2.0 - position[index]
              for index, name in enumerate(("x", "y", "z"))]
    offset[("x", "y", "z").index(axis)] = -offset[("x", "y", "z").index(axis)]
    entry["world_bounds_cm"] = bounds_from(position, offset, size)
    return entry


def measure_targets(cmds, mapping):
    """Scene values of the reference geometry, in Maya and Unreal terms.

    The camera-sync scenario is authored here, so the manifest carries the
    measured Maya values back through the camera route's map.
    """
    candidate = camera_matrix(mapping)
    inverse = mapping.matrix3_inverse(candidate)
    measured = []
    for name, _, _, _ in TARGETS:
        matrix = _world_matrix(cmds, name)
        size = _world_size(cmds, name)
        local = _local_size(cmds, name)
        measured.append({
            "node_name": name,
            "maya_matrix": matrix,
            "maya_size": size,
            "maya_offset": _world_offset(cmds, name),
            "maya_centroid": _surface_centroid(cmds, name),
            "local_size": local,
            "unreal_matrix": mapping.uncarry_world_matrix(candidate, matrix),
            "unreal_size": mapping.expected_axis_sizes(inverse, size),
        })
    return measured


def build_manifest(measured, scan, directory, mapping):
    """A manifest for the handoff, as the exporter would write it.

    The Unreal values are the measured Maya values carried back through the
    documented axis map, so the importer has to recover that map.
    """
    objects = []
    inverse = mapping.matrix3_inverse(mapping.matrix3(
        [candidate["matrix"] for candidate in mapping.AXIS_CANDIDATES
         if candidate["name"] == CAMERA_CANDIDATE][0]))
    for index, entry in enumerate(measured):
        position = mapping.matrix4_translation(entry["unreal_matrix"])
        offset = mapping.matrix3_apply(inverse, entry["maya_offset"])
        centroid = mapping.matrix3_apply(inverse, entry["maya_centroid"])
        objects.append({
            "id": "static_mesh_actor:{0}:component:StaticMeshComponent0:instance:none".format(
                entry["node_name"]),
            "node_name": entry["node_name"],
            "actor": entry["node_name"],
            "actor_class": "StaticMeshActor",
            "level": "/Game/SceneRef/Map",
            "category": "static_mesh",
            "component": "StaticMeshComponent0",
            "mesh": "/Engine/BasicShapes/Cube.Cube",
            "instance_index": None,
            "exported": True,
            "note": "",
            "materials": ["/Engine/BasicShapes/BasicShapeMaterial"],
            "world_location_cm": {"x": position[0], "y": position[1], "z": position[2]},
            "world_rotation_deg": {"roll": 0.0, "pitch": 0.0, "yaw": float(index * 10)},
            "world_scale": {"x": 1.0, "y": 1.0, "z": 1.0},
            "world_matrix": entry["unreal_matrix"],
            "world_surface_centroid_cm": centroid_point(centroid),
            "world_bounds_cm": bounds_from(position, offset, entry["unreal_size"]),
        })
    return {
        "schema": "mtou-scene-ref-manifest/1",
        "generated_utc": "2026-09-28T12:00:00Z",
        "engine_version": "5.7.4-host-check",
        "world": {"package": "/Game/SceneRef/Map", "name": "Map", "up_axis": "Z",
                  "linear_unit": "cm", "world_partition": False},
        "scope": {"kind": "level_range", "persistent_level": "/Game/SceneRef/Map",
                  "requested_sublevels": ["/Game/SceneRef/Sub"],
                  "loaded_sublevels": ["/Game/SceneRef/Sub"],
                  "unloaded_sublevels": [{"package": "/Game/SceneRef/Other",
                                          "streaming_state": "not_in_world",
                                          "visible": False}],
                  "excluded_sublevels": ["/Game/SceneRef/Third"]},
        "scale": {"actors": len(objects), "components": len(objects),
                  "objects": len(objects), "triangles": 4 * 12, "vertices": 4 * 8},
        "objects": objects,
        "unsupported": [{"actor": "LI_House", "class": "LevelInstance",
                         "reason": "level instance is not baked"}],
        "skipped": [{"actor": "SkyLight", "class": "SkyLight",
                     "reason": "not_static_geometry"}],
        "output": {
            "directory": str(directory),
            "geometry_file": "SceneRef_Clean.fbx",
            "geometry_bytes": scan["bytes"],
            "files": [{"name": "SceneRef_Clean.fbx", "bytes": scan["bytes"]}],
            "image_files": [],
            "texture_records": scan["texture_records"],
            "texture_references": scan["texture_references"],
            "video_references": scan["video_references"],
            "texture_reference_files": scan["files"],
            "texture_reference_files_present": [],
            "node_names": [entry["node_name"] for entry in measured],
        },
        "timing": {"scope_seconds": 0.1, "export_seconds": 1.2,
                   "inspect_seconds": 0.2},
        "memory": {"used_physical_mb": 1234.5},
        "warnings": [],
    }


def move_object(entry, mapping, delta):
    """Move one manifest object by ``delta``: matrix, location and bounds.

    The bounds move with the object, so the manifest stays internally consistent
    and the run reports the placement mismatch alone.
    """
    position = mapping.matrix4_translation(entry["world_matrix"])
    entry["world_matrix"] = mapping.matrix4_from_linear_and_translation(
        mapping.matrix4_linear(entry["world_matrix"]),
        tuple(position[axis] + delta[axis] for axis in range(3)))
    entry["world_location_cm"] = {"x": entry["world_matrix"][12],
                                  "y": entry["world_matrix"][13],
                                  "z": entry["world_matrix"][14]}
    bounds = entry["world_bounds_cm"]
    for index, axis in enumerate(("x", "y", "z")):
        bounds["min"][axis] += delta[index]
        bounds["max"][axis] += delta[index]
    if entry.get("world_surface_centroid_cm"):
        for index, axis in enumerate(("x", "y", "z")):
            entry["world_surface_centroid_cm"][axis] += delta[index]
    return entry


def write_json(path, payload):
    path.write_text(json.dumps(payload, indent=2, ensure_ascii=True), encoding="utf-8")
    return path


def make_production_scene(cmds):
    """A fresh scene with one production object, its material and a keyed witness."""
    cmds.file(new=True, force=True)
    cmds.currentUnit(linear="cm", time="film")
    cmds.playbackOptions(minTime=1.0, maxTime=120.0)
    cmds.currentTime(42.0)
    cube = cmds.polyCube(name="ProdCube", width=40.0, height=40.0, depth=40.0)[0]
    shader = cmds.shadingNode("lambert", asShader=True, name="ProdShader")
    cmds.setAttr(shader + ".color", 1.0, 0.0, 0.0, type="double3")
    shading_group = cmds.sets(renderable=True, noSurfaceShader=True, empty=True,
                              name="ProdShaderSG")
    cmds.connectAttr(shader + ".outColor", shading_group + ".surfaceShader", force=True)
    cmds.sets(cmds.listRelatives(cube, shapes=True)[0], edit=True,
              forceElement=shading_group)
    witness = cmds.createNode("transform", name="ProdWitness")
    cmds.setKeyframe(witness, attribute="translateX", time=1.0, value=1.0)
    cmds.setKeyframe(witness, attribute="translateX", time=120.0, value=5.0)
    return cube


def production_state(cmds):
    """Everything a run must leave exactly as it found it."""
    return {
        "prod": cmds.objExists("ProdCube"),
        "shader": cmds.objExists("ProdShader"),
        "shading_group": cmds.listConnections("ProdCubeShape", type="shadingEngine"),
        "shader_color": [round(float(value), 4) for value in
                         cmds.getAttr("ProdShader.color")[0]],
        "witness_keys": int(cmds.keyframe("ProdWitness", query=True, keyframeCount=True) or 0),
        "playback": [float(cmds.playbackOptions(query=True, minTime=True)),
                     float(cmds.playbackOptions(query=True, maxTime=True))],
        "current_time": float(cmds.currentTime(query=True)),
        "time_unit": str(cmds.currentUnit(query=True, time=True)),
        "linear_unit": str(cmds.currentUnit(query=True, linear=True)),
        "roots": sorted(node for node in (cmds.ls(assemblies=True) or [])
                        if node not in ("persp", "top", "front", "side")
                        and not node.startswith("MtoU_UE_SceneRef")),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--result", required=True,
                        help="path for the JSON evidence file")
    parser.add_argument("--keep-scratch", action="store_true",
                        help="keep the fixture directory instead of deleting it")
    args = parser.parse_args(argv)

    report = {"ok": False, "phase": "loading", "checks": [], "failures": [],
              "evidence": {}}
    checks = Checks()
    scratch = scratch_directory()
    started = time.time()
    try:
        import maya.standalone
        maya.standalone.initialize(name="python")
        import maya.cmds as cmds
        import maya.mel as mel

        proto = load_module("MtoUSceneRefPrototype",
                            SCRIPTS / "MtoUSceneRefPrototype.py")
        mapping = proto.mapping()
        cmds.loadPlugin("fbxmaya", quiet=True)

        report.update(maya_version=cmds.about(version=True),
                      maya_api=cmds.about(apiVersion=True),
                      fbx_plugin=cmds.pluginInfo("fbxmaya", query=True, version=True),
                      scratch=str(scratch))

        # ---------------------------------------------------- fixtures
        report["phase"] = "fixtures"
        clean_fbx, textured_fbx, texture_file = author_reference(cmds, mel, scratch)
        measured = measure_targets(cmds, mapping)
        clean_scan = mapping.scan_fbx_file(str(clean_fbx))
        textured_scan = mapping.scan_fbx_file(str(textured_fbx))
        checks.record("clean_scan", {key: clean_scan[key] for key in
                                     ("format", "texture_records", "texture_references",
                                      "video_references", "files")})
        checks.record("textured_scan", {key: textured_scan[key] for key in
                                        ("format", "texture_records", "texture_references",
                                         "video_references", "files")})
        checks.check("the clean fixture records no texture",
                     clean_scan["texture_records"] == 0, str(clean_scan["files"]))
        checks.check("the textured fixture records a texture",
                     textured_scan["texture_records"] >= 1,
                     "records {0} files {1}".format(textured_scan["texture_records"],
                                                    textured_scan["files"]))
        checks.check("the fixtures are ASCII FBX files",
                     clean_scan["format"] == "ascii" and textured_scan["format"] == "ascii",
                     "{0}/{1}".format(clean_scan["format"], textured_scan["format"]))
        checks.check("the reference geometry has measurable and symmetric meshes",
                     len([entry for entry in measured
                          if len(set(round(value, 3)
                                 for value in entry["local_size"])) == 3]) == 3 and
                     [entry for entry in measured if entry["node_name"] == "SM_Rock"][0]
                     ["local_size"] == [50.0, 50.0, 50.0],
                     str([(entry["node_name"], entry["local_size"])
                          for entry in measured]))
        checks.check("the synthesized manifest places the objects away from the origin",
                     all(any(abs(value) > 1.0
                             for value in mapping.matrix4_translation(entry["unreal_matrix"]))
                         for entry in measured),
                     str([mapping.matrix4_translation(entry["unreal_matrix"])
                          for entry in measured]))
        checks.check("the manifest carries Maya placements through the axis map",
                     any(abs(first - second) > 1.0
                         for entry in measured
                         for first, second in zip(
                             mapping.matrix4_translation(entry["maya_matrix"]),
                             mapping.matrix4_translation(entry["unreal_matrix"]))),
                     str([(mapping.matrix4_translation(entry["maya_matrix"]),
                           mapping.matrix4_translation(entry["unreal_matrix"]))
                          for entry in measured[:1]]))

        manifest = build_manifest(measured, clean_scan, scratch, mapping)
        manifest_path = write_json(scratch / "SceneRef.manifest.json", manifest)

        wrong = json.loads(json.dumps(manifest))
        move_object(wrong["objects"][0], mapping, (25.0, 0.0, 0.0))
        wrong_path = write_json(scratch / "SceneRef.wrong.json", wrong)

        unmatched = json.loads(json.dumps(manifest))
        unmatched["objects"][1]["node_name"] = "SM_Ghost"
        move_object(unmatched["objects"][1], mapping, (9000.0, 0.0, 0.0))
        unmatched_path = write_json(scratch / "SceneRef.unmatched.json", unmatched)

        by_transform = json.loads(json.dumps(manifest))
        by_transform["objects"][1]["node_name"] = "SM_Rock_Renamed?"
        by_transform_path = write_json(scratch / "SceneRef.transform.json", by_transform)

        wrong_schema = json.loads(json.dumps(manifest))
        wrong_schema["schema"] = "mtou-scene-ref-manifest/9"
        wrong_schema_path = write_json(scratch / "SceneRef.schema.json", wrong_schema)

        bad_unit = json.loads(json.dumps(manifest))
        bad_unit["world"]["linear_unit"] = "m"
        bad_unit_path = write_json(scratch / "SceneRef.unit.json", bad_unit)

        bad_axis = json.loads(json.dumps(manifest))
        bad_axis["world"]["up_axis"] = "Y"
        bad_axis_path = write_json(scratch / "SceneRef.axis.json", bad_axis)

        disagreeing = json.loads(json.dumps(manifest))
        disagreeing["output"]["texture_records"] = 3
        disagreeing["output"]["image_files"] = ["handoff_texture.png"]
        disagreeing_path = write_json(scratch / "SceneRef.disagree.json", disagreeing)

        not_fbx = scratch / "not_an_fbx.fbx"
        not_fbx.write_text("this file is not an FBX document\n", encoding="utf-8")
        invalid_json_path = scratch / "invalid.json"
        invalid_json_path.write_text("{not json", encoding="utf-8")

        # ---------------------------------------------------- import run
        report["phase"] = "import"
        make_production_scene(cmds)
        before = production_state(cmds)
        checks.record("production_before", before)
        result_path = scratch / "SceneRef.report.json"
        run_report, code = proto.run(str(clean_fbx), str(manifest_path))
        proto.write_report(run_report, result_path)
        checks.record("import_run", {
            "exit": code, "phase": run_report["phase"], "ok": run_report["ok"],
            "candidate": run_report["transform_check"].get("candidate"),
            "max_position_error_cm": run_report["transform_check"].get("max_position_error_cm"),
            "counts": run_report["counts"], "problems": run_report["problems"],
            "warnings": run_report["warnings"]})

        check = checks.check
        check("the import run succeeded", code == 0 and run_report["ok"] is True,
              "exit {0} phase {1} problems {2}".format(code, run_report["phase"],
                                                       run_report["problems"]))
        check("the run reported the done phase", run_report["phase"] == "done",
              str(run_report["phase"]))
        check("no problems were reported", run_report["problems"] == [],
              str(run_report["problems"]))
        check("no warnings were reported", run_report["warnings"] == [],
              str(run_report["warnings"]))
        check("no report file error", "error" not in run_report,
              str(run_report.get("error"))[:400])

        container = run_report["container"]
        check("the container namespace and group exist",
              cmds.namespace(exists=proto.DEFAULT_CONTAINER) and
              cmds.objExists(proto.DEFAULT_CONTAINER + ":" + proto.DEFAULT_CONTAINER),
              json.dumps(container))
        check("the container group is inside its own namespace",
              container["group_path"].startswith("|" + proto.DEFAULT_CONTAINER + ":"),
              str(container["group_path"]))
        check("the container was created, not replaced",
              container["replaced"] is False and container["namespace_created"] is True,
              json.dumps(container))
        check("the report named both container parts",
              container["namespace"] == proto.DEFAULT_CONTAINER and
              container["group"] == proto.DEFAULT_CONTAINER, json.dumps(container))

        counts = run_report["counts"]
        check("every exported manifest object was compared",
              counts["manifest_objects"] == len(TARGETS) and
              counts.get("matched_objects") == len(TARGETS), json.dumps(counts))
        check("the mesh count matches the handoff",
              counts.get("meshes_assigned") == len(TARGETS), json.dumps(counts))
        check("every imported mesh was reassigned",
              counts.get("shading_groups_replaced") == len(TARGETS), json.dumps(counts))
        check("the handoff imported no texture node",
              counts["file_texture_nodes"] == 0 and counts["image_nodes_loaded"] == 0,
              json.dumps(counts))
        check("the container holds the imported nodes",
              counts["container_nodes"] >= len(TARGETS) * 2, json.dumps(counts))

        container_meshes = [node for node in cmds.ls(proto.DEFAULT_CONTAINER + ":*",
                                                     long=True, type="mesh")]
        assigned = {shape: (cmds.listConnections(shape, type="shadingEngine") or [])
                    for shape in container_meshes}
        checks.record("container_assignment", assigned)
        check("every container mesh is on the gray shading group",
              bool(assigned) and all(groups == ["MtoU_UE_SceneRef_GraySG"]
                                     for groups in assigned.values()),
              json.dumps(assigned))
        check("no file node survives in the container",
              not cmds.ls(proto.DEFAULT_CONTAINER + ":*", type="file"), "file nodes found")

        gray = run_report["gray_material"]
        check("the gray material is a lambert",
              gray["type"] == "lambert" and cmds.nodeType(gray["name"]) == "lambert",
              json.dumps(gray))
        check("the gray material is mid gray",
              all(abs(value - 0.5) < 1e-6 for value in gray["color"]), json.dumps(gray))
        check("the gray material is reported unreused",
              gray["reused"] is False and gray["shading_group"].endswith("SG"),
              json.dumps(gray))

        # transform comparison
        transform_check = run_report["transform_check"]
        checks.record("transform_check", {
            key: transform_check.get(key) for key in
            ("candidate", "best", "matched", "max_position_error_cm", "max_size_error_cm",
             "max_orientation_error_deg", "objects_compared", "objects_missing")})
        checks.record("fit", transform_check.get("fit"))
        check("the comparison matched", transform_check["matched"] is True,
              json.dumps(checks.evidence["transform_check"]))
        check("the candidate is the documented axis map",
              transform_check["candidate"] == EXPECTED_CANDIDATE,
              str(transform_check["candidate"]))
        check("every candidate was scored",
              len(transform_check["candidates"]) == len(mapping.AXIS_CANDIDATES),
              str(len(transform_check["candidates"])))
        check("the position error is inside tolerance",
              transform_check["max_position_error_cm"] <= 1e-6,
              str(transform_check["max_position_error_cm"]))
        check("the size error is inside tolerance",
              transform_check["max_size_error_cm"] <= 1e-6,
              str(transform_check["max_size_error_cm"]))
        check("the orientation error is zero for the measurable meshes",
              transform_check["max_orientation_error_deg"] == 0.0,
              str(transform_check["max_orientation_error_deg"]))
        scores = {candidate["name"]: candidate
                  for candidate in transform_check["candidates"]}
        checks.record("candidate_scores", {
            name: {key: score[key] for key in
                   ("max_position_error_cm", "max_size_error_cm",
                    "max_orientation_error_deg")}
            for name, score in scores.items()})
        for other in mapping.AXIS_CANDIDATES:
            if other["name"] == EXPECTED_CANDIDATE:
                continue
            score = scores[other["name"]]
            check("the candidate {0!r} is rejected".format(other["name"]),
                  score["max_position_error_cm"] is None or
                  score["max_position_error_cm"] > 1.0,
                  json.dumps(checks.evidence["candidate_scores"][other["name"]]))
        fit = transform_check["fit"]
        check("the least-squares fit is determined", fit["available"] is True,
              str(fit.get("reason")))
        check("the fitted map is a signed permutation",
              fit["signed_permutation"] is True, json.dumps(fit))
        check("the fitted determinant is a reflection",
              fit["determinant"] is not None and
              abs(fit["determinant"] + 1.0) <= 1e-9, str(fit["determinant"]))
        check("the fit recovers the documented permutation",
              fit["permutation"] == [1, 2, 0] and fit["signs"] == [1, 1, -1],
              json.dumps({"permutation": fit["permutation"], "signs": fit["signs"]}))
        check("the fit residual is at machine precision",
              fit["max_position_error_cm"] is not None and
              fit["max_position_error_cm"] <= 1e-6, str(fit["max_position_error_cm"]))
        check("the fit used every matched object", fit["points"] == len(TARGETS),
              str(fit["points"]))

        objects = run_report["objects"]
        check("every object is reported", len(objects) == len(TARGETS), str(len(objects)))
        check("every object was matched by name",
              all(entry["matched_by"] == "name" for entry in objects),
              json.dumps([entry["matched_by"] for entry in objects]))
        check("every object was found",
              all(entry["found"] for entry in objects),
              json.dumps([entry["node_name"] for entry in objects if not entry["found"]]))
        check("every object carries its measured world matrix",
              all(entry["world_matrix"] and len(entry["world_matrix"]) == 16
                  for entry in objects), "world matrices missing")
        check("a symmetric mesh reports its orientation as skipped",
              not [entry for entry in objects
                   if entry["node_name"] == "SM_Rock"][0]["orientation_checked"],
              "SM_Rock is a cube")
        check("rotated meshes report their orientation checked",
              all(entry["orientation_checked"] for entry in objects
                  if entry["node_name"] in ("SM_Wall", "SM_Beam")),
              json.dumps([(entry["node_name"], entry["orientation_checked"])
                          for entry in objects]))
        authored = {entry["node_name"]: mapping.matrix4_translation(entry["maya_matrix"])
                    for entry in measured}
        for entry in objects:
            expected_position = authored[entry["node_name"]]
            measured_position = entry["measured_position_cm"]
            check("the measured position of {0} is the authored one".format(
                entry["node_name"]),
                  measured_position is not None and
                  all(abs(measured_position[axis] - expected_position[index]) <= 1e-6
                      for index, axis in enumerate(("x", "y", "z"))),
                  "{0} vs {1}".format(measured_position, expected_position))
        checks.record("objects", [{key: entry[key] for key in
                                   ("node_name", "found", "matched_by",
                                    "position_error_cm", "size_error_cm",
                                    "orientation_checked", "orientation_error_deg")}
                                  for entry in objects])

        # contract shape
        for key, kind in CONTRACT_REPORT_KEYS.items():
            check("report holds {0} as {1}".format(key, kind.__name__),
                  isinstance(run_report.get(key), kind),
                  "{0!r}".format(run_report.get(key))[:120])
        check("the report holds the contract schema",
              run_report["schema"] == mapping.REPORT_SCHEMA, str(run_report["schema"]))
        check("the Maya block holds the contract keys",
              CONTRACT_MAYA_KEYS <= set(run_report["maya"]), json.dumps(run_report["maya"]))
        check("the container block holds the contract keys",
              CONTRACT_CONTAINER_KEYS <= set(container), json.dumps(container))
        check("the counts block holds the contract keys",
              CONTRACT_COUNT_KEYS <= set(counts), json.dumps(counts))
        check("the gray material holds the contract keys",
              CONTRACT_GRAY_KEYS <= set(gray), json.dumps(gray))
        check("the transform check holds the contract keys",
              CONTRACT_CHECK_KEYS <= set(transform_check), json.dumps(
                  sorted(transform_check.keys())))
        check("every object holds the contract keys",
              all(CONTRACT_OBJECT_KEYS <= set(entry) for entry in objects),
              json.dumps(sorted(objects[0].keys())))
        check("the texture scan holds the contract keys",
              CONTRACT_SCAN_KEYS <= set(run_report["texture_scan"]),
              json.dumps(sorted(run_report["texture_scan"].keys())))
        check("the timing block holds the contract keys",
              CONTRACT_TIMING_KEYS <= set(run_report["timing"]),
              json.dumps(run_report["timing"]))
        check("the memory block holds the contract key",
              CONTRACT_MEMORY_KEYS <= set(run_report["memory"]),
              json.dumps(run_report["memory"]))
        check("the import timing was measured",
              run_report["timing"]["import_seconds"] >= 0.0 and
              run_report["timing"]["verify_seconds"] >= 0.0,
              json.dumps(run_report["timing"]))
        check("the process memory was read",
              run_report["memory"]["process_rss_available"] is True and
              run_report["memory"]["process_rss_mb"] > 0.0,
              json.dumps(run_report["memory"]))
        check("the texture scan names the handoff",
              run_report["texture_scan"]["source"].endswith("SceneRef_Clean.fbx"),
              str(run_report["texture_scan"]["source"]))
        check("the run reported the scope before importing",
              run_report["manifest"]["scope"]["unloaded_sublevels"][0]["streaming_state"]
              == "not_in_world" and
              run_report["manifest"]["objects_exported"] == len(TARGETS),
              json.dumps(run_report["manifest"]["scope"]))
        check("the run reported the geometry scale",
              run_report["manifest"]["scale"]["triangles"] == 48,
              json.dumps(run_report["manifest"]["scale"]))

        after = production_state(cmds)
        checks.record("production_after", after)
        check("the production object and its material are untouched",
              after["prod"] and after["shader"] and
              after["shading_group"] == ["ProdShaderSG"] and
              after["shader_color"] == [1.0, 0.0, 0.0], json.dumps(after))
        check("the scene time and range are untouched",
              after["playback"] == before["playback"] and
              after["current_time"] == before["current_time"] and
              after["time_unit"] == before["time_unit"] and
              after["linear_unit"] == before["linear_unit"], json.dumps(after))
        check("the witness keys are untouched", after["witness_keys"] == 2,
              str(after["witness_keys"]))
        check("no other root object appeared",
              after["roots"] == before["roots"],
              "{0} vs {1}".format(after["roots"], before["roots"]))
        side_effects = run_report["scene"]["import_side_effects"]
        check("the importer reports and restores the host's own side effects",
              set(side_effects) <= {"current_time"} and
              run_report["scene"]["playback_range_unchanged"] is True and
              run_report["scene"]["current_time_unchanged"] is True and
              run_report["scene"]["time_unit_after"] == before["time_unit"],
              json.dumps({"side_effects": side_effects,
                          "range": run_report["scene"]["playback_range_unchanged"],
                          "time": run_report["scene"]["current_time_unchanged"]}))
        check("the fb2 plugin did move the current time before the restore",
              "current_time" in side_effects and
              side_effects["current_time"]["after"] == 1.0,
              json.dumps(side_effects))

        # ---------------------------------------------------- repeat run
        report["phase"] = "repeat"
        first_nodes = run_report["counts"]["container_nodes"]
        repeat_report, repeat_code = proto.run(str(clean_fbx), str(manifest_path))
        checks.record("repeat_run", {
            "exit": repeat_code, "ok": repeat_report["ok"],
            "replaced": repeat_report["container"]["replaced"],
            "container_nodes": repeat_report["counts"]["container_nodes"],
            "previous_nodes": first_nodes,
            "matched": repeat_report["counts"].get("matched_objects"),
            "warnings": repeat_report["warnings"], "problems": repeat_report["problems"]})
        check("the repeat run succeeded",
              repeat_code == 0 and repeat_report["ok"] is True,
              "exit {0} problems {1}".format(repeat_code, repeat_report["problems"]))
        check("the repeat run replaced only its container",
              repeat_report["container"]["replaced"] is True, json.dumps(
                  repeat_report["container"]))
        check("the container holds the same nodes after the repeat run",
              repeat_report["counts"]["container_nodes"] == first_nodes,
              "{0} vs {1}".format(repeat_report["counts"]["container_nodes"], first_nodes))
        check("the repeat run compared every object again",
              repeat_report["counts"].get("matched_objects") == len(TARGETS),
              json.dumps(repeat_report["counts"]))
        check("the repeat run left the production object alone",
              production_state(cmds)["shading_group"] == ["ProdShaderSG"] and
              production_state(cmds)["roots"] == before["roots"],
              json.dumps(production_state(cmds)))
        check("the removal was scoped to the namespace",
              repeat_report["container_removal"]["namespace_removed"] is True and
              repeat_report["container_removal"]["namespace_existed"] is True,
              json.dumps(repeat_report["container_removal"]))

        keep_report, keep_code = proto.run(str(clean_fbx), str(manifest_path),
                                           keep_existing=True)
        check("keep-existing keeps the container",
              keep_code == 0 and keep_report["ok"] is True and
              keep_report["container"]["replaced"] is False and
              keep_report["container"]["kept_existing"] is True,
              json.dumps(keep_report["container"]))
        check("keep-existing still compares every object",
              keep_report["counts"].get("matched_objects") == len(TARGETS),
              json.dumps(keep_report["counts"]))

        # ---------------------------------------------------- mismatches
        report["phase"] = "mismatch"
        wrong_report, wrong_code = proto.run(str(clean_fbx), str(wrong_path))
        checks.record("wrong_value_run", {
            "exit": wrong_code, "ok": wrong_report["ok"],
            "problems": wrong_report["problems"],
            "position_error_cm": wrong_report["objects"][0]["position_error_cm"]})
        check("a wrong manifest value fails the run",
              wrong_code == 1 and wrong_report["ok"] is False, json.dumps(
                  {"exit": wrong_code, "ok": wrong_report["ok"]}))
        check("the wrong value is reported with both positions",
              any("POSITION_MISMATCH" in problem and "SM_Pillar" in problem and
                  "expected (100.0000, 200.0000, -325.0000)" in problem
                  for problem in wrong_report["problems"]),
              json.dumps(wrong_report["problems"]))
        check("the moved object is the only one reported",
              all(problem.startswith("POSITION_MISMATCH") or "SM_Pillar" in problem
                  for problem in wrong_report["problems"]) and
              len(wrong_report["problems"]) <= 3,
              json.dumps(wrong_report["problems"]))
        checks.close("the wrong value reports the 25 cm error",
                     wrong_report["objects"][0]["position_error_cm"], 25.0, tolerance=1e-6)
        check("the wrong value leaves the other objects matched",
              all(entry["position_error_cm"] <= 1e-6
                  for entry in wrong_report["objects"][1:]),
              json.dumps([entry["position_error_cm"] for entry in wrong_report["objects"]]))

        missing_report, missing_code = proto.run(str(clean_fbx), str(unmatched_path))
        checks.record("missing_node_run", {
            "exit": missing_code, "ok": missing_report["ok"],
            "problems": missing_report["problems"],
            "objects": [entry["node_name"] for entry in missing_report["objects"]
                        if not entry["found"]]})
        check("a node that is not in the scene is a problem",
              missing_code == 1 and missing_report["ok"] is False and
              any("MISSING_NODE" in problem for problem in missing_report["problems"]),
              json.dumps(missing_report["problems"]))
        check("the missing node is reported by name",
              any("SM_Ghost" in problem for problem in missing_report["problems"]),
              json.dumps(missing_report["problems"]))
        check("the missing node is not silently skipped",
              any(not entry["found"] and entry["node_name"] == "SM_Ghost"
                  for entry in missing_report["objects"]),
              json.dumps([(entry["node_name"], entry["found"])
                          for entry in missing_report["objects"]]))

        transform_report, transform_code = proto.run(str(clean_fbx),
                                                    str(by_transform_path))
        checks.record("transform_match_run", {
            "exit": transform_code, "ok": transform_report["ok"],
            "matched_by": [entry["matched_by"] for entry in transform_report["objects"]],
            "warnings": transform_report["warnings"]})
        check("a node name that did not survive falls back to world position",
              transform_code == 0 and transform_report["ok"] is True and
              [entry["matched_by"] for entry in transform_report["objects"]].count(
                  "transform") == 1,
              json.dumps(checks.evidence["transform_match_run"]))
        check("the transform match is reported",
              any("TRANSFORM_MATCH" in warning
                  for warning in transform_report["warnings"]),
              json.dumps(transform_report["warnings"]))
        check("the transform matched object carries its distance",
              all(entry["match_distance_cm"] is not None
                  for entry in transform_report["objects"]
                  if entry["matched_by"] == "transform"),
              json.dumps([entry["match_distance_cm"]
                          for entry in transform_report["objects"]]))

        disagree_report, disagree_code = proto.run(str(clean_fbx), str(disagreeing_path))
        check("a disagreeing exporter count is reported, not hidden",
              disagree_code == 0 and
              any("TEXTURE_COUNT_DISAGREEMENT" in warning
                  for warning in disagree_report["warnings"]) and
              any("TEXTURE_FILES_DISAGREEMENT" in warning
                  for warning in disagree_report["warnings"]),
              json.dumps(disagree_report["warnings"]))

        # ------------------------------------------------------ engine map
        # The real engine handoff does not round-trip through the camera route's
        # map: an Unreal point (x, y, z) arrives at (x, z, y). This case builds
        # that handoff, including an instanced child the engine names after its
        # instance index, and asserts the comparison measures the convention
        # instead of demanding the camera one.
        report["phase"] = "engine_map"
        engine_fbx, engine_measured = author_engine_reference(cmds, mel, scratch, mapping)
        engine_scan = mapping.scan_fbx_file(str(engine_fbx))
        checks.record("engine_scan", {key: engine_scan[key] for key in
                                      ("format", "texture_records", "files",
                                       "video_references")})
        check("the engine-map fixture records no texture",
              engine_scan["format"] == "ascii" and engine_scan["texture_records"] == 0,
              json.dumps(checks.evidence["engine_scan"]))
        engine_manifest = build_engine_manifest(engine_measured, engine_scan, scratch,
                                                mapping)
        engine_manifest_path = write_json(scratch / "SceneRef.engine.json", engine_manifest)

        make_production_scene(cmds)
        engine_report, engine_code = proto.run(str(engine_fbx), str(engine_manifest_path))
        engine_check = engine_report["transform_check"]
        checks.record("engine_map_run", {
            "exit": engine_code, "ok": engine_report["ok"], "phase": engine_report["phase"],
            "best": engine_check.get("best"), "decided_by": engine_check.get("decided_by"),
            "matched": engine_check.get("matched"),
            "max_position_error_cm": engine_check.get("max_position_error_cm"),
            "max_size_error_cm": engine_check.get("max_size_error_cm"),
            "objects_compared": engine_check.get("objects_compared"),
            "objects_missing": engine_check.get("objects_missing"),
            "camera_contract": engine_check.get("camera_contract"),
            "fit": {key: engine_check["fit"].get(key) for key in
                    ("available", "signed_permutation", "determinant", "permutation",
                     "signs", "max_position_error_cm", "points")},
            "counts": engine_report["counts"], "problems": engine_report["problems"],
            "warnings": engine_report["warnings"],
            "matched_by": [entry["matched_by"] for entry in engine_report["objects"]]})

        check("the engine-map handoff verifies",
              engine_code == 0 and engine_report["ok"] is True and
              engine_report["problems"] == [],
              "exit {0} problems {1}".format(engine_code, engine_report["problems"]))
        check("the measured engine map is the reported candidate",
              engine_check["best"] == ENGINE_CANDIDATE and
              engine_check["candidate"] == ENGINE_CANDIDATE,
              json.dumps({"best": engine_check["best"],
                          "candidate": engine_check["candidate"]}))
        check("the comparison says a named candidate decided it",
              engine_check["decided_by"] == "candidate" and
              engine_check["winner"]["name"] == ENGINE_CANDIDATE and
              engine_check["winner"]["source"] == "candidate",
              json.dumps(engine_check.get("winner")))
        check("the engine-map comparison matched",
              engine_check["matched"] is True and
              engine_check["max_position_error_cm"] <= 1e-6 and
              engine_check["max_size_error_cm"] <= 1e-6,
              json.dumps(checks.evidence["engine_map_run"]))
        check("every object of the engine-map handoff was matched",
              engine_check["objects_compared"] == len(engine_measured) and
              engine_check["objects_missing"] == 0 and
              all(entry["found"] for entry in engine_report["objects"]),
              json.dumps({"compared": engine_check["objects_compared"],
                          "missing": engine_check["objects_missing"]}))
        check("the numeric instance node was matched by world position",
              [entry["matched_by"] for entry in engine_report["objects"]
               if entry["node_name"] == "0"] == ["transform"],
              json.dumps(checks.evidence["engine_map_run"]["matched_by"]))
        check("the named objects were matched by name",
              all(entry["matched_by"] == "name" for entry in engine_report["objects"]
                  if entry["node_name"] != "0"),
              json.dumps(checks.evidence["engine_map_run"]["matched_by"]))
        check("the engine-map objects report no per-object problem",
              all(entry["problems"] == [] for entry in engine_report["objects"]) and
              all((entry["position_error_cm"] or 0.0) <= 1e-6 and
                  (entry["size_error_cm"] or 0.0) <= 1e-6
                  for entry in engine_report["objects"]),
              json.dumps([(entry["node_name"], entry["position_error_cm"],
                           entry["size_error_cm"]) for entry in engine_report["objects"]]))
        engine_wedge = [entry for entry in engine_report["objects"]
                        if entry["node_name"] == ENGINE_WEDGE][0]
        check("the asymmetric object's offset is measured and matches",
              engine_wedge["offset_error_cm"] is not None and
              engine_wedge["offset_error_cm"] <= 1e-6 and
              engine_wedge["orientation_checked"] is False,
              json.dumps({key: engine_wedge[key] for key in
                          ("offset_error_cm", "orientation_checked",
                           "measured_offset_cm", "expected_offset_cm")}))
        check("the camera-map objects report an agreeing surface centroid",
              all(entry["centroid_checked"] and entry["centroid_error_cm"] <= 1e-6
                  for entry in objects) and
              transform_check["max_centroid_error_cm"] <= 1e-6 and
              transform_check["tolerances"]["centroid_cm"] == 1.0,
              json.dumps({"max": transform_check.get("max_centroid_error_cm"),
                          "tolerance": transform_check["tolerances"].get("centroid_cm"),
                          "errors": [(entry["node_name"], entry["centroid_error_cm"])
                                     for entry in objects]}))
        check("the offset metric is reported with its own tolerance",
              engine_check["tolerances"]["offset_cm"] == 1.0 and
              engine_check["max_offset_error_cm"] is not None and
              engine_check["max_offset_error_cm"] <= 1e-6 and
              engine_check["winner"]["max_offset_error_cm"] is not None,
              json.dumps({"tolerance": engine_check["tolerances"].get("offset_cm"),
                          "max_offset_error_cm": engine_check["max_offset_error_cm"],
                          "winner": engine_check["winner"].get("max_offset_error_cm")}))
        check("the asymmetric object is not symmetric enough to skip the offset",
              abs(engine_wedge["measured_offset_cm"]["x"]) > 10.0 and
              abs(engine_wedge["measured_offset_cm"]["y"]) > 10.0 and
              abs(engine_wedge["measured_offset_cm"]["z"]) > 10.0,
              json.dumps(engine_wedge["measured_offset_cm"]))
        engine_wedge_centroid = [entry for entry in engine_report["objects"]
                                 if entry["node_name"] == ENGINE_WEDGE][0]
        check("the asymmetric object's surface centroid is measured and matches",
              engine_wedge_centroid["centroid_checked"] is True and
              engine_wedge_centroid["centroid_error_cm"] <= 1e-6 and
              engine_wedge_centroid["centroid_faces_used"] > 0 and
              engine_wedge_centroid["centroid_triangles_used"] >=
              engine_wedge_centroid["centroid_faces_used"],
              json.dumps({key: engine_wedge_centroid[key] for key in
                          ("centroid_checked", "centroid_error_cm",
                           "centroid_faces_used", "centroid_triangles_used", "centroid_sampled",
                           "measured_centroid_cm", "expected_centroid_cm")}))
        check("the centroid metric is reported with its own tolerance",
              engine_check["tolerances"]["centroid_cm"] == 1.0 and
              engine_check["max_centroid_error_cm"] is not None and
              engine_check["max_centroid_error_cm"] <= 1e-6 and
              engine_check["winner"]["max_centroid_error_cm"] is not None,
              json.dumps({"tolerance": engine_check["tolerances"].get("centroid_cm"),
                          "max_centroid_error_cm": engine_check.get("max_centroid_error_cm")}))
        engine_fit = engine_check["fit"]
        check("the fit of the engine-map handoff is a signed permutation",
              engine_fit["available"] is True and engine_fit["signed_permutation"] is True,
              json.dumps(engine_fit))
        check("the engine-map fit is the reflection the handoff uses",
              engine_fit["determinant"] is not None and
              abs(engine_fit["determinant"] + 1.0) <= 1e-9 and
              engine_fit["permutation"] == [0, 2, 1] and engine_fit["signs"] == [1, 1, 1],
              json.dumps({"determinant": engine_fit["determinant"],
                          "permutation": engine_fit["permutation"],
                          "signs": engine_fit["signs"]}))
        check("the engine-map fit residual is at machine precision",
              engine_fit["max_position_error_cm"] is not None and
              engine_fit["max_position_error_cm"] <= 1e-6 and
              engine_fit["points"] == len(engine_measured),
              json.dumps({"residual": engine_fit["max_position_error_cm"],
                          "points": engine_fit["points"]}))
        engine_camera = engine_check["camera_contract"]
        check("the camera route's map is reported as divergent",
              engine_camera["candidate"] == CAMERA_CANDIDATE and
              engine_camera["max_position_error_cm"] > 100.0 and
              engine_camera["max_size_error_cm"] > 10.0 and
              engine_camera["within_tolerance"] is False,
              json.dumps({key: value for key, value in engine_camera.items()
                          if key != "note"}))
        check("the divergence from the camera contract is explained",
              "reconcile" in engine_camera["note"] and
              "does not correct" in engine_camera["note"],
              engine_camera["note"])
        engine_scores = {entry["name"]: entry for entry in engine_check["candidates"]}
        check("the classic Z-up to Y-up candidate alone is not the match",
              engine_check["best"] != "maya_x=ue_x, maya_y=ue_z, maya_z=-ue_y" and
              engine_scores["maya_x=ue_x, maya_y=ue_z, maya_z=-ue_y"]
              ["max_position_error_cm"] > 1.0,
              json.dumps({key: engine_scores["maya_x=ue_x, maya_y=ue_z, maya_z=-ue_y"][key]
                          for key in ("name", "max_position_error_cm",
                                      "max_size_error_cm")}))
        check("the camera route's map alone is not the match either",
              engine_scores[CAMERA_CANDIDATE]["max_position_error_cm"] > 1.0,
              json.dumps({key: engine_scores[CAMERA_CANDIDATE][key] for key in
                          ("name", "max_position_error_cm", "max_size_error_cm")}))
        escaped = [node for node in cmds.ls(proto.DEFAULT_CONTAINER + ":*", long=True,
                                            type="transform")
                   if "FBXASC" in node]
        check("the instance node kept this host's escaped numeric name",
              len(escaped) == 1 and engine_report["counts"]["matched_by_transform_count"] == 1,
              json.dumps({"escaped": escaped,
                          "counts": engine_report["counts"]}))
        check("the engine-map handoff imported no texture node",
              engine_report["counts"]["file_texture_nodes"] == 0 and
              engine_report["counts"]["image_nodes_loaded"] == 0,
              json.dumps(engine_report["counts"]))
        check("the engine-map run left the production object alone",
              production_state(cmds)["shading_group"] == ["ProdShaderSG"],
              json.dumps(production_state(cmds)))

        # A mirrored arrival: same position, same box size, box centre on the
        # other side of the pivot. Nothing but the offset can see it, so the run
        # has to report it and refuse to call the handoff matched.
        report["phase"] = "offset"
        mirrored = json.loads(json.dumps(engine_manifest))
        mirrored_entry = [entry for entry in mirrored["objects"]
                          if entry["node_name"] == ENGINE_WEDGE][0]
        mirror_object_offset(mirrored_entry, axis="x")
        mirrored_path = write_json(scratch / "SceneRef.offset.json", mirrored)
        make_production_scene(cmds)
        mirrored_report, mirrored_code = proto.run(str(engine_fbx), str(mirrored_path))
        mirrored_check = mirrored_report["transform_check"]
        mirrored_object = [entry for entry in mirrored_report["objects"]
                           if entry["node_name"] == ENGINE_WEDGE][0]
        checks.record("offset_mirror_run", {
            "exit": mirrored_code, "ok": mirrored_report["ok"],
            "matched": mirrored_check.get("matched"),
            "best": mirrored_check.get("best"),
            "max_position_error_cm": mirrored_check.get("max_position_error_cm"),
            "max_size_error_cm": mirrored_check.get("max_size_error_cm"),
            "max_offset_error_cm": mirrored_check.get("max_offset_error_cm"),
            "object": {key: mirrored_object[key] for key in
                       ("node_name", "position_error_cm", "size_error_cm",
                        "offset_error_cm", "expected_offset_cm",
                        "measured_offset_cm", "orientation_checked")},
            "problems": mirrored_report["problems"]})
        check("a mirrored offset fails the run",
              mirrored_code == 1 and mirrored_report["ok"] is False and
              mirrored_check["matched"] is False,
              json.dumps({"exit": mirrored_code, "matched": mirrored_check["matched"]}))
        check("the mirrored offset is reported as OFFSET_MISMATCH",
              any("OFFSET_MISMATCH" in problem and ENGINE_WEDGE in problem
                  for problem in mirrored_report["problems"]),
              json.dumps(mirrored_report["problems"]))
        check("only the offset changed for the mirrored object",
              mirrored_object["offset_error_cm"] > 1.0 and
              mirrored_object["position_error_cm"] <= 1e-6 and
              mirrored_object["size_error_cm"] <= 1e-6 and
              mirrored_object["orientation_checked"] is False,
              json.dumps(checks.evidence["offset_mirror_run"]["object"]))
        check("the mirrored offset error is twice the offset it flipped",
              mirrored_object["offset_error_cm"] is not None and
              abs(mirrored_object["offset_error_cm"] -
                  2.0 * abs(mirrored_object["measured_offset_cm"]["x"])) <= 1e-6,
              json.dumps({"error": mirrored_object["offset_error_cm"],
                          "measured_x": mirrored_object["measured_offset_cm"]["x"]}))
        check("every other object still passes under the winner",
              all((entry["offset_error_cm"] or 0.0) <= 1e-6
                  for entry in mirrored_report["objects"]
                  if entry["node_name"] != ENGINE_WEDGE) and
              mirrored_check["max_offset_error_cm"] == mirrored_object["offset_error_cm"],
              json.dumps([(entry["node_name"], entry["offset_error_cm"])
                          for entry in mirrored_report["objects"]]))

        # A mirrored placement of an asymmetric mesh: position, size and offset
        # all agree, only the vertex centroid moves.
        report["phase"] = "centroid"
        mirrored_centroid = json.loads(json.dumps(engine_manifest))
        centroid_entry = [entry for entry in mirrored_centroid["objects"]
                          if entry["node_name"] == ENGINE_WEDGE][0]
        mirror_object_centroid(centroid_entry, axis="z")
        centroid_path = write_json(scratch / "SceneRef.centroid.json", mirrored_centroid)
        make_production_scene(cmds)
        centroid_report, centroid_code = proto.run(str(engine_fbx), str(centroid_path))
        centroid_check = centroid_report["transform_check"]
        centroid_object = [entry for entry in centroid_report["objects"]
                           if entry["node_name"] == ENGINE_WEDGE][0]
        checks.record("centroid_mirror_run", {
            "exit": centroid_code, "ok": centroid_report["ok"],
            "matched": centroid_check.get("matched"),
            "best": centroid_check.get("best"),
            "max_centroid_error_cm": centroid_check.get("max_centroid_error_cm"),
            "object": {key: centroid_object[key] for key in
                       ("node_name", "position_error_cm", "size_error_cm",
                        "offset_error_cm", "centroid_error_cm", "centroid_checked",
                        "expected_centroid_cm", "measured_centroid_cm",
                        "orientation_checked")},
            "problems": centroid_report["problems"]})
        check("a mirrored surface centroid fails the run",
              centroid_code == 1 and centroid_report["ok"] is False and
              centroid_check["matched"] is False,
              json.dumps({"exit": centroid_code, "matched": centroid_check["matched"]}))
        check("the mirrored centroid is reported as CENTROID_MISMATCH",
              any("CENTROID_MISMATCH" in problem and ENGINE_WEDGE in problem
                  for problem in centroid_report["problems"]),
              json.dumps(centroid_report["problems"]))
        check("only the centroid changed for the mirrored object",
              centroid_object["centroid_error_cm"] > 1.0 and
              centroid_object["position_error_cm"] <= 1e-6 and
              centroid_object["size_error_cm"] <= 1e-6 and
              (centroid_object["offset_error_cm"] or 0.0) <= 1e-6,
              json.dumps(checks.evidence["centroid_mirror_run"]["object"]))
        deltas = {axis: abs(centroid_object["expected_centroid_cm"][axis] -
                            centroid_object["measured_centroid_cm"][axis])
                  for axis in ("x", "y", "z")}
        moved_axis = max(deltas, key=lambda axis: deltas[axis])
        check("the mirrored centroid error is twice the centroid it flipped",
              centroid_object["centroid_error_cm"] is not None and
              abs(centroid_object["centroid_error_cm"] - deltas[moved_axis]) <= 1e-6 and
              abs(deltas[moved_axis] -
                  2.0 * abs(centroid_object["measured_centroid_cm"][moved_axis] -
                            centroid_object["measured_position_cm"][moved_axis])) <= 1e-6 and
              len([value for value in deltas.values() if value > 1e-6]) == 1,
              json.dumps({"error": centroid_object["centroid_error_cm"],
                          "deltas": deltas,
                          "expected": centroid_object["expected_centroid_cm"],
                          "measured": centroid_object["measured_centroid_cm"],
                          "position": centroid_object["measured_position_cm"]}))
        check("every other object still passes its centroid check",
              all((entry["centroid_error_cm"] or 0.0) <= 1e-6
                  for entry in centroid_report["objects"]
                  if entry["node_name"] != ENGINE_WEDGE),
              json.dumps([(entry["node_name"], entry["centroid_error_cm"])
                          for entry in centroid_report["objects"]]))

        # A handoff written before the centroid existed: the check reports itself
        # unavailable for that object instead of failing it.
        report["phase"] = "centroid_unavailable"
        older = json.loads(json.dumps(engine_manifest))
        del [entry for entry in older["objects"]
             if entry["node_name"] == ENGINE_WEDGE][0]["world_surface_centroid_cm"]
        older_path = write_json(scratch / "SceneRef.old.json", older)
        make_production_scene(cmds)
        older_report, older_code = proto.run(str(engine_fbx), str(older_path))
        older_object = [entry for entry in older_report["objects"]
                        if entry["node_name"] == ENGINE_WEDGE][0]
        checks.record("centroid_unavailable_run", {
            "exit": older_code, "ok": older_report["ok"],
            "matched": older_report["transform_check"].get("matched"),
            "warnings": older_report["warnings"],
            "object": {key: older_object[key] for key in
                       ("node_name", "centroid_checked", "centroid_error_cm",
                        "centroid_faces_used", "centroid_triangles_used",
                        "problems")}})
        check("a manifest without a centroid is not a failure",
              older_code == 0 and older_report["ok"] is True and
              older_report["problems"] == [] and
              older_object["centroid_checked"] is False and
              older_object["centroid_error_cm"] is None,
              json.dumps(checks.evidence["centroid_unavailable_run"]))
        check("the unavailable centroid check is reported",
              any("CENTROID_UNAVAILABLE" in warning and ENGINE_WEDGE in warning
                  for warning in older_report["warnings"]),
              json.dumps(older_report["warnings"]))
        check("the other objects still check their centroid",
              all(entry["centroid_checked"] for entry in older_report["objects"]
                  if entry["node_name"] != ENGINE_WEDGE),
              json.dumps([(entry["node_name"], entry["centroid_checked"])
                          for entry in older_report["objects"]]))

        # The face cap: a mesh above it is sampled evenly rather than read
        # whole, and the reading stays close to the exact centroid.
        report["phase"] = "centroid_cap"
        cmds.file(new=True, force=True)
        cmds.currentUnit(linear="cm", time="film")
        big = cmds.polySphere(name="BigSphere", radius=500.0,
                              subdivisionsAxis=170, subdivisionsHeight=160)[0]
        big_shape = cmds.listRelatives(big, shapes=True, fullPath=True)[0]
        face_count = int(cmds.polyEvaluate(big_shape, face=True))
        exact = _surface_centroid(cmds, big)
        sampled, faces_used, triangles_used, sampled_flag = \
            proto._world_surface_centroid(big)
        checks.record("centroid_cap", {
            "faces": face_count, "cap": proto.CENTROID_FACE_CAP,
            "faces_used": faces_used, "triangles_used": triangles_used,
            "sampled": sampled_flag, "exact": exact, "sampled_centroid": sampled,
            "distance_cm": None if sampled is None else
            mapping.vector_length(tuple(sampled[axis] - exact[axis]
                                        for axis in range(3)))})
        check("the fixture mesh is larger than the face cap",
              face_count > proto.CENTROID_FACE_CAP, str(face_count))
        check("the cap is applied and reported",
              sampled_flag is True and 0 < faces_used <= proto.CENTROID_FACE_CAP,
              json.dumps(checks.evidence["centroid_cap"]))
        check("the sampled centroid stays close to the exact one",
              sampled is not None and exact is not None and
              mapping.vector_length(tuple(sampled[axis] - exact[axis]
                                          for axis in range(3))) <= 5.0,
              json.dumps(checks.evidence["centroid_cap"]))
        check("triangles never exceed the faces that produced them",
              triangles_used <= faces_used * 2 and triangles_used > 0,
              json.dumps({"triangles": triangles_used, "faces": faces_used}))

        # ---------------------------------------------------- texture refusal
        report["phase"] = "textures"
        make_production_scene(cmds)
        texture_report, texture_code = proto.run(str(textured_fbx), str(manifest_path))
        checks.record("textured_refusal", {
            "exit": texture_code, "phase": texture_report["phase"],
            "ok": texture_report["ok"], "problems": texture_report["problems"],
            "container_nodes": texture_report["counts"]["container_nodes"]})
        check("a textured handoff is refused",
              texture_code == 1 and texture_report["phase"] == "refused" and
              texture_report["ok"] is False, json.dumps(checks.evidence["textured_refusal"]))
        check("the refusal names the texture rule",
              any("TEXTURE_REFERENCES_PRESENT" in problem
                  for problem in texture_report["problems"]),
              json.dumps(texture_report["problems"]))
        check("the refusal imported nothing",
              texture_report["counts"]["container_nodes"] == 0 and
              not cmds.namespace(exists=proto.DEFAULT_CONTAINER) and
              texture_report["timing"]["import_seconds"] is None and
              texture_report["timing"]["verify_seconds"] is None,
              json.dumps({"counts": texture_report["counts"],
                          "namespace": cmds.namespace(exists=proto.DEFAULT_CONTAINER)}))
        check("the refusal left the production object alone",
              production_state(cmds)["shading_group"] == ["ProdShaderSG"],
              json.dumps(production_state(cmds)))
        check("the refusal still reports the scan",
              texture_report["texture_scan"]["texture_records"] >= 1,
              json.dumps({key: texture_report["texture_scan"][key] for key in
                          ("texture_records", "texture_references", "files")}))

        allowed_report, allowed_code = proto.run(str(textured_fbx), str(manifest_path),
                                                 allow_textures=True)
        checks.record("textured_allowed", {
            "exit": allowed_code, "ok": allowed_report["ok"],
            "counts": allowed_report["counts"], "warnings": allowed_report["warnings"],
            "problems": allowed_report["problems"]})
        check("allow-textures imports the textured handoff",
              allowed_code == 0 and allowed_report["phase"] == "done",
              json.dumps(checks.evidence["textured_allowed"]))
        check("the surviving texture node is reported",
              allowed_report["counts"]["file_texture_nodes"] >= 1 and
              allowed_report["counts"]["image_nodes_loaded"] >= 1 and
              any("TEXTURE_NODES_PRESENT" in warning
                  for warning in allowed_report["warnings"]),
              json.dumps({"counts": allowed_report["counts"],
                          "warnings": allowed_report["warnings"]}))
        check("the surviving texture node is not silently deleted",
              bool(cmds.ls(proto.DEFAULT_CONTAINER + ":*", type="file")),
              "no file node in the container")
        check("an allowed texture is not a problem",
              not any("TEXTURE" in problem for problem in allowed_report["problems"]),
              json.dumps(allowed_report["problems"]))
        check("the meshes are still on the gray material",
              all((cmds.listConnections(shape, type="shadingEngine") or []) ==
                  ["MtoU_UE_SceneRef_GraySG"]
                  for shape in cmds.ls(proto.DEFAULT_CONTAINER + ":*", type="mesh")),
              "gray material not assigned")

        # ---------------------------------------------------- refusals
        report["phase"] = "refusals"
        refusals = (
            ("a file that is not FBX", not_fbx, manifest_path, {}, 2, "FBX_NOT_FBX"),
            ("a missing handoff", scratch / "missing.fbx", manifest_path, {}, 2,
             "FBX_UNREADABLE"),
            ("another manifest schema", clean_fbx, wrong_schema_path, {}, 2,
             "MANIFEST_SCHEMA_MISMATCH"),
            ("a manifest that is not JSON", clean_fbx, invalid_json_path, {}, 2,
             "MANIFEST_INVALID_JSON"),
            ("a manifest world in metres", clean_fbx, bad_unit_path, {}, 2,
             "UNSUPPORTED_MANIFEST_LINEAR_UNIT"),
            ("a manifest world that is Y up", clean_fbx, bad_axis_path, {}, 2,
             "UNSUPPORTED_MANIFEST_UP_AXIS"),
            ("an unusable container name", clean_fbx, manifest_path,
             {"container": "Bad Name"}, 2, "CONTAINER_NAME_INVALID"),
        )
        refusal_evidence = {}
        for label, fbx, manifest_file, options, expected_code, expected_category in refusals:
            make_production_scene(cmds)
            refusal_report, refusal_code = proto.run(str(fbx), str(manifest_file),
                                                    **options)
            problems = " ".join(refusal_report["problems"])
            refusal_evidence[label] = {"exit": refusal_code,
                                       "phase": refusal_report["phase"],
                                       "problems": refusal_report["problems"][:1]}
            check("refused: {0}".format(label),
                  refusal_code == expected_code and
                  refusal_report["phase"] == "refused" and
                  expected_category in problems,
                  json.dumps(refusal_evidence[label]))

        make_production_scene(cmds)
        cmds.currentUnit(linear="in")
        unit_report, unit_code = proto.run(str(clean_fbx), str(manifest_path))
        cmds.currentUnit(linear="cm")
        check("a scene that is not in centimetres is refused",
              unit_code == 2 and unit_report["phase"] == "refused" and
              "UNSUPPORTED_SCENE_LINEAR_UNIT" in " ".join(unit_report["problems"]),
              json.dumps({"exit": unit_code, "problems": unit_report["problems"][:1]}))
        checks.record("refusals", refusal_evidence)

        # ---------------------------------------------------- command line
        report["phase"] = "commandline"
        cli_report_path = scratch / "cli.report.json"
        environment = dict(os.environ)
        command = [sys.executable, str(SCRIPTS / "MtoUSceneRefPrototype.py"),
                   "--fbx", str(clean_fbx), "--manifest", str(manifest_path),
                   "--report", str(cli_report_path), "--json"]
        completed = subprocess.run(command, capture_output=True, timeout=600,
                                   env=environment)
        # Maya writes its own log lines in the console code page, so the streams
        # are decoded defensively: only the ASCII report text is asserted on.
        stdout = completed.stdout.decode("utf-8", "replace")
        stderr = completed.stderr.decode("utf-8", "replace")
        checks.record("cli", {"returncode": completed.returncode,
                              "stdout_bytes": len(completed.stdout),
                              "stderr_tail": stderr[-400:]})
        check("the command line run succeeded", completed.returncode == 0,
              "returncode {0} stderr {1}".format(completed.returncode, stderr[-400:]))
        check("the command line printed the report json",
              '"schema": "mtou-scene-ref-report/1"' in stdout, stdout[:200])
        cli_report = json.loads(cli_report_path.read_text(encoding="utf-8"))
        check("the command line wrote its report",
              cli_report["ok"] is True and cli_report["phase"] == "done" and
              cli_report["transform_check"]["matched"] is True,
              json.dumps({"ok": cli_report["ok"], "phase": cli_report["phase"]}))
        check("the command line compared every object",
              cli_report["counts"].get("matched_objects") == len(TARGETS),
              json.dumps(cli_report["counts"]))

        report["ok"] = not checks.failures
        report["phase"] = "done"
    except Exception:
        report["error"] = traceback.format_exc()
        report["phase"] = "failed"
    finally:
        report["checks"] = checks.results
        report["failures"] = checks.failures
        report["evidence"] = checks.evidence
        report["seconds"] = round(time.time() - started, 2)
        try:
            Path(args.result).write_text(
                json.dumps(report, indent=2, ensure_ascii=True, default=str),
                encoding="utf-8")
        except OSError as error:
            sys.stderr.write("cannot write result {0}: {1}\n".format(args.result, error))
        if not args.keep_scratch:
            shutil.rmtree(str(scratch), ignore_errors=True)
        try:
            import maya.standalone
            maya.standalone.uninitialize()
        except Exception:
            pass
    print("checks: {0}, failures: {1}".format(len(checks.results), len(checks.failures)))
    for failure in checks.failures:
        print("FAILED " + failure)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
