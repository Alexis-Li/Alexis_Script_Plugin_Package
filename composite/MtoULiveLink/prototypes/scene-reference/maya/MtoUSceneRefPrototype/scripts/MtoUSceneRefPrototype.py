"""Maya importer and verifier for the Issue 53 scene reference handoff.

The Unreal side exports the static geometry of one level scope into an ASCII FBX
plus a ``mtou-scene-ref-manifest/1`` measurement record. This module imports that
file into one container, puts the reference geometry on a single gray material
and compares the measured Maya world matrices and bounding boxes against the
manifest, so the two hosts are checked against each other rather than trusted.
The frozen contract is
``composite/MtoULiveLink/prototypes/scene-reference/transfer.md``; this module
implements its "Maya report schema", "Texture rule", "Object naming" and
"Commands" sections and never changes the contract.

Usage::

    <mayapy> MtoUSceneRefPrototype.py --fbx <file> --manifest <file>
        --report <file> [--container MtoU_UE_SceneRef] [--allow-textures]
        [--keep-existing] [--json]

Exit codes: ``0`` when every check passed, ``1`` when a check failed (including a
handoff that records textures without ``--allow-textures``), ``2`` for usage and
contract errors (an unreadable or non-FBX handoff, a manifest that is not
``mtou-scene-ref-manifest/1``, a scene or manifest unit that is not centimetres,
a manifest world that is not Z up, an unusable container name).

Safety contract
---------------
* The scene is never saved. Playback range, frame rate, current time and every
  existing key are left exactly as they were; the importer never calls
  ``save``, ``playbackOptions``, ``currentTime`` or ``setKeyframe``.
* Only the container namespace and its group are created, replaced or deleted.
  Nothing outside the container is renamed, reparented, reassigned or deleted,
  so a production scene keeps its own objects and shading networks.
* Replacing the container happens only when the container already exists and
  ``--keep-existing`` was not passed; the deletion is scoped to that namespace
  and that group.
* A handoff that records material textures is not imported unless
  ``--allow-textures`` was passed, and ``file`` nodes that survive in the
  container are always reported.
* ``main()`` never raises: a failure is written into the report JSON with the
  phase it happened in and the run exits non-zero.

Maya's API is reached through ``maya.cmds`` only, imported lazily so the module
can be imported and inspected without a running Maya.
"""

import argparse
import ctypes
import importlib.util
import json
import os
import re
import sys
import time
import traceback
from pathlib import Path

DEFAULT_CONTAINER = "MtoU_UE_SceneRef"

EXIT_OK = 0
EXIT_CHECK_FAILED = 1
EXIT_USAGE = 2

CATEGORY_CONTAINER_NAME = "CONTAINER_NAME_INVALID"
CATEGORY_UNSUPPORTED_SCENE_UNIT = "UNSUPPORTED_SCENE_LINEAR_UNIT"
CATEGORY_UNSUPPORTED_MANIFEST_UNIT = "UNSUPPORTED_MANIFEST_LINEAR_UNIT"
CATEGORY_UNSUPPORTED_UP_AXIS = "UNSUPPORTED_MANIFEST_UP_AXIS"
CATEGORY_FBX_UNREADABLE = "FBX_UNREADABLE"
CATEGORY_FBX_NOT_FBX = "FBX_NOT_FBX"
CATEGORY_FBX_PLUGIN = "FBX_PLUGIN_UNAVAILABLE"
CATEGORY_TEXTURES_PRESENT = "TEXTURE_REFERENCES_PRESENT"
CATEGORY_IMPORT_FAILED = "FBX_IMPORT_FAILED"
CATEGORY_GRAY_MATERIAL = "GRAY_MATERIAL_CONFLICT"

#: Most faces accumulated per object for the world surface centroid check. Above
#: it the faces are sampled evenly, so a huge handoff cannot turn the check into
#: a hang; ``centroid_faces_used``, ``centroid_triangles_used`` and
#: ``centroid_sampled`` say what happened.
CENTROID_FACE_CAP = 20000

#: Maya time units that the importer accepts; it never changes them.
_MANAGED_TIME_UNITS = frozenset((
    "game", "film", "pal", "ntsc", "show", "palf", "ntscf", "millisec", "sec",
    "min", "hour",
))

_cmds_module = None
_mapping_module = None


def commands():
    """``maya.cmds``, imported on first use.

    The name is not ``cmds`` on purpose: ``maya.standalone.initialize`` injects a
    ``cmds`` module into the globals of its caller, which would shadow this
    accessor whenever the importer initializes maya itself.
    """
    global _cmds_module
    if _cmds_module is None:
        import maya.cmds as maya_cmds
        _cmds_module = maya_cmds
    return _cmds_module


def mapping():
    """The pure mapping module, loaded from this script's directory."""
    global _mapping_module
    if _mapping_module is None:
        path = Path(__file__).resolve().parent / "mtou_scene_ref_mapping.py"
        spec = importlib.util.spec_from_file_location(
            "mtou_scene_ref_mapping_loaded", str(path))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _mapping_module = module
    return _mapping_module


def ensure_maya(name="python"):
    """Make ``maya.cmds`` usable, initializing Maya on a plain CPython host.

    ``mayapy`` imports ``maya.cmds`` as a stub without commands, so a missing
    attribute is what tells this function to initialize the standalone server.
    Returns whether this call initialized standalone Maya.
    """
    try:
        maya_cmds = commands()
    except ImportError:
        maya_cmds = None
    if maya_cmds is not None:
        try:
            maya_cmds.about(version=True)
            return False
        except Exception:  # noqa: BLE001 - not initialized yet
            pass
    import maya.standalone
    maya.standalone.initialize(name=name)
    return True


class SceneRefRefused(RuntimeError):
    """A run the importer stops instead of guessing. ``category`` is stable."""

    def __init__(self, category, detail, exit_code=EXIT_USAGE):
        RuntimeError.__init__(self, "{0}: {1}".format(category, detail))
        self.category = category
        self.detail = detail
        self.exit_code = exit_code


def process_memory_mb():
    """Resident set size of this process in MB, or ``None`` when unreadable.

    Windows uses ``GetProcessMemoryInfo``, every other host reads
    ``/proc/self/statm``. A host that offers neither reports ``None`` instead of
    failing the run.
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
                    ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t),
                    ("PeakPagefileUsage", ctypes.c_size_t),
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
    except Exception as error:  # noqa: BLE001 - memory reporting must never fail a run
        return None, "unavailable: {0}".format(error)


class SceneRefImporter(object):
    """Imports one handoff into the container and verifies it against the manifest."""

    def __init__(self, fbx, manifest, container=DEFAULT_CONTAINER,
                 allow_textures=False, keep_existing=False):
        self.fbx = str(fbx)
        self.manifest_path = str(manifest)
        self.container = str(container)
        self.allow_textures = bool(allow_textures)
        self.keep_existing = bool(keep_existing)
        self.manifest = None
        self.summary = {}
        self.scan = {}
        self.report = {
            "schema": mapping().REPORT_SCHEMA,
            "ok": False,
            "phase": "start",
            "maya": {},
            "container": {"namespace": self.container, "group": self.container,
                          "replaced": False},
            "counts": {"manifest_objects": 0, "container_nodes": 0,
                       "file_texture_nodes": 0, "image_nodes_loaded": 0},
            "gray_material": {},
            "transform_check": {},
            "objects": [],
            "texture_scan": {},
            # Present for the contract's shape; a run that never imported leaves
            # them null instead of claiming a duration it did not measure.
            "timing": {"import_seconds": None, "verify_seconds": None},
            "memory": {},
            "problems": [],
            "warnings": [],
            "manifest": {},
            "fbx": {},
            "scene": {},
        }

    # ------------------------------------------------------------- phases

    def run(self):
        """Run every phase and return the report; never raises."""
        started = time.time()
        exit_code = EXIT_OK
        try:
            self._read_manifest()
            self._read_scene()
            self._scan_handoff()
            self._import_handoff()
            self._apply_gray_material()
            self._verify()
            self.report["phase"] = "done"
        except SceneRefRefused as refusal:
            exit_code = refusal.exit_code
            self.report["phase"] = "refused"
            self.report["problems"].append(
                "REFUSED {0}: {1}".format(refusal.category, refusal.detail))
        except mapping().ManifestError as refusal:
            exit_code = EXIT_USAGE
            self.report["phase"] = "refused"
            self.report["problems"].append(
                "REFUSED {0}: {1}".format(refusal.category, refusal.detail))
        except Exception:  # noqa: BLE001 - the report carries the failure
            exit_code = EXIT_CHECK_FAILED
            self.report["phase"] = "failed"
            self.report["error"] = traceback.format_exc()
            self.report["problems"].append(
                "INTERNAL_ERROR: the run failed, see the report's error field")
        finally:
            self.report["timing"]["total_seconds"] = round(time.time() - started, 3)
            rss, source = process_memory_mb()
            self.report["memory"] = {"process_rss_mb": rss,
                                     "process_rss_available": rss is not None,
                                     "source": source}
        self.report["ok"] = not self.report["problems"]
        if not self.report["ok"] and exit_code == EXIT_OK:
            exit_code = EXIT_CHECK_FAILED
        return self.report, exit_code

    def _read_manifest(self):
        """Read the manifest and report the scope before anything is imported."""
        manifest = mapping().load_manifest(self.manifest_path)
        summary = mapping().manifest_summary(manifest)
        self.manifest = manifest
        self.summary = summary
        world = summary["world"]
        if world.get("linear_unit") not in (None, "cm"):
            raise SceneRefRefused(
                CATEGORY_UNSUPPORTED_MANIFEST_UNIT,
                "manifest world linear unit is {0!r}; the prototype compares "
                "centimetres and does not rescale".format(world.get("linear_unit")))
        if world.get("up_axis") not in (None, "Z"):
            raise SceneRefRefused(
                CATEGORY_UNSUPPORTED_UP_AXIS,
                "manifest world up axis is {0!r}; the axis candidates assume "
                "Unreal's Z up".format(world.get("up_axis")))
        self.report["manifest"] = summary
        self.report["counts"]["manifest_objects"] = summary["objects_exported"]
        self.report["counts"]["manifest_objects_total"] = summary["objects_total"]
        if not summary["objects_exported"]:
            self.report["warnings"].append(
                "NO_EXPORTED_OBJECTS: the manifest declares no exported objects, "
                "so there is nothing to compare")
        self.report["phase"] = "manifest"

    def _read_scene(self):
        """Record the Maya session and refuse a scene the comparison cannot use."""
        maya_cmds = commands()
        if not _container_name_valid(self.container):
            raise SceneRefRefused(
                CATEGORY_CONTAINER_NAME,
                "container name {0!r} is not a plain Maya node name".format(self.container))
        try:
            maya_cmds.loadPlugin("fbxmaya", quiet=True)
        except Exception as error:  # noqa: BLE001 - reported as a refusal
            raise SceneRefRefused(CATEGORY_FBX_PLUGIN,
                                  "the bundled fbxmaya plugin did not load: {0}".format(error))
        try:
            plugin_version = maya_cmds.pluginInfo("fbxmaya", query=True, version=True)
        except Exception:  # noqa: BLE001
            plugin_version = None
        linear_unit = str(maya_cmds.currentUnit(query=True, linear=True))
        time_unit = str(maya_cmds.currentUnit(query=True, time=True))
        up_axis = str(maya_cmds.upAxis(query=True, axis=True)).lower()
        self.report["maya"] = {
            "version": str(maya_cmds.about(version=True)),
            "api": str(maya_cmds.about(apiVersion=True)),
            "batch": bool(maya_cmds.about(batch=True)),
            "linear_unit": linear_unit,
            "up_axis": up_axis,
            "time_unit": time_unit,
            "plugins": {"fbxmaya": plugin_version},
        }
        if linear_unit.strip().lower() != "cm":
            raise SceneRefRefused(
                CATEGORY_UNSUPPORTED_SCENE_UNIT,
                "the Maya scene linear unit is {0!r}; the manifest is in "
                "centimetres and the prototype does not rescale".format(linear_unit))
        if time_unit.strip().lower() not in _MANAGED_TIME_UNITS:
            self.report["warnings"].append(
                "UNKNOWN_TIME_UNIT: the scene time unit {0!r} is not one the "
                "importer recognises; it is never changed".format(time_unit))
        self.report["scene"] = {
            "linear_unit": linear_unit,
            "up_axis": up_axis,
            "time_unit": time_unit,
            "current_namespace": maya_cmds.namespaceInfo(currentNamespace=True),
            "playback_range": {
                "min": float(maya_cmds.playbackOptions(query=True, minTime=True)),
                "max": float(maya_cmds.playbackOptions(query=True, maxTime=True)),
            },
            "current_time": float(maya_cmds.currentTime(query=True)),
            "namespaces": list(maya_cmds.namespaceInfo(listOnlyNamespaces=True,
                                                       recurse=True) or []),
        }
        self.report["phase"] = "scene"

    def _scan_handoff(self):
        """Scan the FBX text and refuse a textured handoff when not allowed."""
        scan = mapping().scan_fbx_file(self.fbx)
        self.scan = scan
        self.report["texture_scan"] = scan
        if not scan["exists"] or not scan["readable"]:
            raise SceneRefRefused(
                CATEGORY_FBX_UNREADABLE,
                "handoff {0} is not a readable file: {1}".format(
                    self.fbx, scan["error"] or "unknown error"))
        if scan["format"] == "unknown":
            raise SceneRefRefused(
                CATEGORY_FBX_NOT_FBX,
                "handoff {0} is not an FBX file: its header {1!r} matches neither "
                "the ASCII nor the binary FBX magic".format(self.fbx, scan["magic"]))
        if scan["binary"] and not scan["texture_records"]:
            self.report["warnings"].append(
                "BINARY_SCAN: the handoff is a binary FBX, so the texture scan is "
                "a heuristic string table pass rather than a parsed document")
        self._compare_texture_scan(scan)
        if scan["texture_records"] and not self.allow_textures:
            raise SceneRefRefused(
                CATEGORY_TEXTURES_PRESENT,
                "the handoff records {0} Texture record(s) ({1} of them naming a "
                "file {2}); the contract forbids importing a textured handoff, "
                "pass --allow-textures to inspect it anyway".format(
                    scan["texture_records"], scan["texture_references"], scan["files"]),
                exit_code=EXIT_CHECK_FAILED)
        self.report["texture_scan"]["allowed"] = bool(self.allow_textures)
        self.report["phase"] = "scan"

    def _compare_texture_scan(self, scan):
        """Record where this scan and the exporter's own count disagree."""
        output = self.summary.get("output") or {}
        for key in ("texture_records", "texture_references", "video_references"):
            declared = output.get(key)
            if declared is None:
                continue
            if int(declared) != int(scan[key]):
                self.report["warnings"].append(
                    "TEXTURE_COUNT_DISAGREEMENT: the manifest reports {0}={1}, this "
                    "Maya scan found {2}".format(key, declared, scan[key]))
        declared_images = output.get("image_files") or []
        if declared_images and not scan["texture_records"]:
            self.report["warnings"].append(
                "TEXTURE_FILES_DISAGREEMENT: the exporter listed produced image "
                "files {0} but the FBX records no Texture record".format(declared_images))
        declared_present = output.get("texture_reference_files_present") or []
        if declared_present:
            self.report["warnings"].append(
                "TEXTURE_FILES_PRESENT: the exporter reports image files that exist "
                "on disk next to the handoff: {0}".format(declared_present))
        declared_names = output.get("node_names")
        if isinstance(declared_names, list) and declared_names:
            missing = [entry.get("node_name")
                       for entry in mapping().exported_objects(self.manifest)
                       if entry.get("node_name") not in declared_names]
            if missing:
                self.report["warnings"].append(
                    "NODE_NAMES_DISAGREEMENT: the exporter's output.node_names does "
                    "not hold the manifest node names {0}".format(missing))

    def _import_handoff(self):
        """Create or replace the container, import the FBX into it, parent the roots."""
        maya_cmds = commands()
        namespace_before = maya_cmds.namespaceInfo(currentNamespace=True)
        state_before = self._scene_state()
        existing_namespace = bool(maya_cmds.namespace(exists=self.container))
        existing_group = bool(self._container_group_names())
        replaced = False
        if (existing_namespace or existing_group) and not self.keep_existing:
            self._remove_container()
            replaced = True
        if not maya_cmds.namespace(exists=self.container):
            maya_cmds.namespace(add=self.container)
        if not maya_cmds.objExists(self.container + ":" + self.container):
            maya_cmds.namespace(set=self.container)
            try:
                maya_cmds.createNode("transform", name=self.container)
            finally:
                maya_cmds.namespace(set=namespace_before)
        group = self._container_group()

        started = time.time()
        maya_cmds.namespace(set=self.container)
        try:
            imported = maya_cmds.file(self.fbx, i=True, ns=self.container, type="FBX",
                                      ignoreVersion=True, mergeNamespacesOnClash=False,
                                      options="v=0;")
        except Exception as error:  # noqa: BLE001 - reported as a refusal
            raise SceneRefRefused(
                CATEGORY_IMPORT_FAILED,
                "the FBX plugin refused {0}: {1}".format(self.fbx, error),
                exit_code=EXIT_CHECK_FAILED)
        finally:
            maya_cmds.namespace(set=namespace_before)
            self.report["scene"]["import_side_effects"] = self._restore_scene_state(
                state_before)
        roots = self._imported_roots(group)
        for node in roots:
            maya_cmds.parent(node, group, relative=True)
        self.report["timing"]["import_seconds"] = round(time.time() - started, 3)
        self.report["fbx"] = {
            "file": self.fbx,
            "bytes": self.scan.get("bytes"),
            "format": self.scan.get("format"),
            "plugin": "fbxmaya",
            "plugin_version": self.report["maya"].get("plugins", {}).get("fbxmaya"),
            "import_options": "v=0;",
            "namespace_flag": self.container,
            "namespace_mechanism": ("current namespace during import; this host's "
                                    "fbxmaya ignores the ns flag for node placement"),
            "import_return": str(imported),
            "root_nodes_parented": roots,
        }
        self.report["container"] = {
            "namespace": self.container,
            "group": self.container,
            "group_path": group,
            "replaced": replaced,
            "kept_existing": bool(self.keep_existing and (existing_namespace or existing_group)),
            "namespace_created": not existing_namespace or replaced,
        }
        self.report["counts"]["container_nodes"] = len(self._namespace_members())
        self.report["phase"] = "import"

    def _apply_gray_material(self):
        """Put every imported mesh on one gray lambert and report what is left."""
        maya_cmds = commands()
        gray_name = mapping().DEFAULT_GRAY_MATERIAL
        existing = maya_cmds.ls(gray_name, type="lambert")
        reused = bool(existing)
        if existing:
            shader = existing[0]
        elif maya_cmds.objExists(gray_name):
            raise SceneRefRefused(
                CATEGORY_GRAY_MATERIAL,
                "a node named {0} exists and is not a lambert; the importer does "
                "not rename or replace scene objects it did not create".format(gray_name),
                exit_code=EXIT_CHECK_FAILED)
        else:
            shader = maya_cmds.shadingNode("lambert", asShader=True, name=gray_name)
        maya_cmds.setAttr(shader + ".color", 0.5, 0.5, 0.5, type="double3")
        color = [float(value) for value in maya_cmds.getAttr(shader + ".color")[0]]
        group_name = gray_name + "SG"
        if maya_cmds.objExists(group_name):
            shading_group = maya_cmds.ls(group_name, type="shadingEngine")[0]
        else:
            shading_group = maya_cmds.sets(renderable=True, noSurfaceShader=True,
                                           empty=True, name=group_name)
        if not maya_cmds.isConnected(shader + ".outColor",
                                     shading_group + ".surfaceShader"):
            maya_cmds.connectAttr(shader + ".outColor",
                                  shading_group + ".surfaceShader", force=True)

        replaced = 0
        assigned = []
        for shape in self._container_nodes_of_type("mesh"):
            before = maya_cmds.listConnections(shape, type="shadingEngine") or []
            maya_cmds.sets(shape, edit=True, forceElement=shading_group)
            after = maya_cmds.listConnections(shape, type="shadingEngine") or []
            if before != after:
                replaced += 1
            assigned.append({"shape": shape, "before": before, "after": after})
        leftovers = self._leftovers()
        file_nodes = sorted(self._container_nodes_of_type("file"))
        image_nodes_loaded = [node for node in file_nodes if _file_node_image_exists(node)]
        if file_nodes:
            message = ("TEXTURE_NODES_PRESENT: {0} file node(s) survive in the "
                       "container {1}: {2}".format(len(file_nodes), self.container,
                                                   file_nodes))
            if self.allow_textures:
                self.report["warnings"].append(message)
                self.report["texture_scan"]["allowed"] = True
            else:
                self.report["problems"].append(message)
        self.report["gray_material"] = {
            "name": shader,
            "type": str(maya_cmds.nodeType(shader)),
            "color": color,
            "shading_group": shading_group,
            "reused": reused,
            "meshes_assigned": len(assigned),
            "shading_groups_replaced": replaced,
            "leftover_file_nodes": file_nodes,
            "leftover_image_nodes_loaded": image_nodes_loaded,
            "leftover_imported_material_nodes": leftovers,
            "assignment": assigned,
        }
        self.report["counts"]["file_texture_nodes"] = len(file_nodes)
        self.report["counts"]["image_nodes_loaded"] = len(image_nodes_loaded)
        self.report["counts"]["meshes_assigned"] = len(assigned)
        self.report["counts"]["shading_groups_replaced"] = replaced
        self.report["phase"] = "material"

    def _verify(self):
        """Compare every exported manifest object with its Maya node.

        Every exported object is matched by name first. Objects the engine names
        after an instance index -- a purely numeric node name, which Maya is free
        to rename -- are then matched by world position under the best axis
        candidate, and the method used is reported per object as ``matched_by``
        (``name`` or ``transform``). A node that cannot be matched either way is
        a reported problem.
        """
        started = time.time()
        index = self._container_index()
        entries = []
        for entry in mapping().exported_objects(self.manifest):
            name, unreal_matrix, unreal_size, unreal_offset, unreal_centroid, \
                problems = mapping().manifest_object_requirement(entry)
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
        comparison = mapping().compare_measurements(mapping().AXIS_CANDIDATES, measurements)
        if self._match_pending_by_transform(entries, comparison):
            measurements = [self._measure_manifest_object(entry) for entry in entries]
            comparison = mapping().compare_measurements(mapping().AXIS_CANDIDATES,
                                                        measurements)
        self.report["objects"] = comparison["objects"]
        self.report["transform_check"] = comparison["transform_check"]
        self.report["problems"].extend(comparison["problems"])
        self.report["warnings"].extend(comparison["warnings"])
        matched_by_transform = [entry["node_name"] for entry in entries
                                if entry["matched_by"] == "transform"]
        if matched_by_transform:
            self.report["warnings"].append(
                "TRANSFORM_MATCH: {0} object(s) were matched by world position "
                "rather than by name: {1}".format(len(matched_by_transform),
                                                  matched_by_transform))
        self.report["counts"]["matched_objects"] = comparison["transform_check"]["objects_compared"]
        self.report["counts"]["matched_by_name"] = len(
            [entry for entry in entries if entry["matched_by"] == "name"])
        self.report["counts"]["matched_by_transform_count"] = len(matched_by_transform)
        self.report["timing"]["verify_seconds"] = round(time.time() - started, 3)
        self.report["phase"] = "verify"
        self._record_scene_after()

    def _measure_manifest_object(self, entry):
        """Maya readings for one manifest object, or an unfound placeholder."""
        measurement = {
            "node_name": entry["node_name"],
            "matched_id": entry["manifest"].get("id"),
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
            "match_distance_cm": entry["match_distance_cm"],
            "match_candidate": entry["match_candidate"],
        }
        if not entry["path"]:
            return measurement
        try:
            matrix = commands().xform(entry["path"], query=True, worldSpace=True, matrix=True)
        except Exception as error:  # noqa: BLE001 - reported, never skipped
            measurement["problems"].append(
                "NODE_UNREADABLE: {0} could not be read: {1}".format(
                    entry["path"], error))
            return measurement
        measurement["maya_matrix"] = [float(value) for value in matrix]
        try:
            bounds = _world_bounds(entry["path"])
            measurement["maya_size"] = [bounds[1][axis] - bounds[0][axis]
                                        for axis in range(3)]
            position = mapping().matrix4_translation(matrix)
            measurement["maya_offset"] = [
                (bounds[0][axis] + bounds[1][axis]) / 2.0 - position[axis]
                for axis in range(3)]
            measurement["local_size"] = _local_bounds_size(entry["path"])
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
                         "matrix": mapping().matrix3(winner["matrix"])})
        for candidate in transform_check["candidates"]:
            if winner.get("name") == candidate["name"]:
                continue
            maps.append({"name": candidate["name"],
                         "matrix": mapping().matrix3(candidate["matrix"])})
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
        group = self._container_group()
        used = {entry["path"] for entry in entries if entry["path"]}
        available = [node for node in self._container_nodes_of_type("transform")
                     if node not in used and node != group]
        if not available:
            return False
        positions = {}
        for node in available:
            matrix = commands().xform(node, query=True, worldSpace=True, matrix=True)
            positions[node] = mapping().matrix4_translation([float(value) for value in matrix])
        tolerance = mapping().TOLERANCES["position_cm"]
        maps = self._fallback_maps(comparison)
        moved = False
        for candidate in maps:
            matrix = candidate["matrix"]
            taken = set()
            assignments = []
            for entry in pending:
                expected = mapping().matrix3_apply(
                    matrix, mapping().matrix4_translation(entry["ue_matrix"]))
                best_node = None
                best_distance = None
                for node, position in positions.items():
                    if node in taken:
                        continue
                    distance = mapping().vector_length(
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
        maya_cmds = commands()
        scene = self.report.get("scene") or {}
        before_range = scene.get("playback_range") or {}
        before_time = scene.get("current_time")
        after_range = {
            "min": float(maya_cmds.playbackOptions(query=True, minTime=True)),
            "max": float(maya_cmds.playbackOptions(query=True, maxTime=True)),
        }
        after_time = float(maya_cmds.currentTime(query=True))
        scene["playback_range_after"] = after_range
        scene["current_time_after"] = after_time
        scene["namespaces_after"] = list(
            maya_cmds.namespaceInfo(listOnlyNamespaces=True, recurse=True) or [])
        scene["time_unit_after"] = str(maya_cmds.currentUnit(query=True, time=True))
        scene["linear_unit_after"] = str(maya_cmds.currentUnit(query=True, linear=True))
        scene["playback_range_unchanged"] = (
            before_range.get("min") == after_range["min"]
            and before_range.get("max") == after_range["max"])
        scene["current_time_unchanged"] = before_time == after_time
        self.report["scene"] = scene

    # -------------------------------------------------------------- helpers

    def _remove_container(self):
        """Delete only the container namespace and its group."""
        maya_cmds = commands()
        removal = {"namespace_removed": False, "group_removed": False,
                   "namespace_existed": bool(maya_cmds.namespace(exists=self.container)),
                   "group_existed": bool(self._container_group_names())}
        if removal["namespace_existed"]:
            maya_cmds.namespace(removeNamespace=self.container,
                                deleteNamespaceContent=True)
            removal["namespace_removed"] = True
        for node in self._container_group_names():
            if maya_cmds.objExists(node):
                maya_cmds.delete(node)
                removal["group_removed"] = True
        self.report["container_removal"] = removal

    def _container_group_names(self):
        """Existing container transforms: a stale root one, and the namespaced one."""
        maya_cmds = commands()
        names = list(maya_cmds.ls(self.container, type="transform", long=True) or [])
        names.extend(maya_cmds.ls(self.container + ":" + self.container,
                                  type="transform", long=True) or [])
        return names

    def _scene_state(self):
        """The session state the importer promises not to keep."""
        maya_cmds = commands()
        return {
            "current_time": float(maya_cmds.currentTime(query=True)),
            "playback_min": float(maya_cmds.playbackOptions(query=True, minTime=True)),
            "playback_max": float(maya_cmds.playbackOptions(query=True, maxTime=True)),
            "time_unit": str(maya_cmds.currentUnit(query=True, time=True)),
        }

    def _restore_scene_state(self, before):
        """Undo the frame rate, playback range and current time an import moved.

        ``cmds.file`` importing an FBX moves Maya's current time to the imported
        take and can adopt the file's frame rate, so the state captured before
        the import is put back and every difference is reported.
        """
        maya_cmds = commands()
        after = self._scene_state()
        changed = {key: {"before": before[key], "after": after[key]}
                   for key in before if before[key] != after[key]}
        if not changed:
            return changed
        if "time_unit" in changed:
            maya_cmds.currentUnit(time=before["time_unit"])
        if "playback_min" in changed or "playback_max" in changed:
            maya_cmds.playbackOptions(minTime=before["playback_min"],
                                      maxTime=before["playback_max"])
        if "current_time" in changed:
            maya_cmds.currentTime(before["current_time"])
        return changed

    def _namespace_members(self):
        return list(commands().namespaceInfo(self.container, listNamespace=True) or [])

    def _container_nodes_of_type(self, node_type):
        """Nodes of ``node_type`` inside the container namespace."""
        return list(commands().ls(self.container + ":*", long=True, type=node_type) or [])

    def _container_group(self):
        """Long name of the container transform inside the container namespace."""
        nodes = commands().ls(self.container + ":" + self.container, long=True,
                          type="transform") or []
        if not nodes:
            raise SceneRefRefused(
                CATEGORY_GRAY_MATERIAL,
                "the container group {0}:{0} was not created".format(self.container),
                exit_code=EXIT_CHECK_FAILED)
        return nodes[0]

    def _imported_roots(self, group):
        """Imported container transforms that are still at the top of the scene."""
        maya_cmds = commands()
        roots = []
        for node in self._container_nodes_of_type("transform"):
            if node == group:
                continue
            parent = maya_cmds.listRelatives(node, parent=True, fullPath=True)
            if not parent:
                roots.append(node)
        return roots

    def _container_index(self):
        """Container transforms by short node name, with their long paths."""
        maya_cmds = commands()
        index = {}
        for node in self._container_nodes_of_type("transform"):
            short = _short_node_name(node)
            index.setdefault(short, []).append(node)
        return index

    def _leftovers(self):
        """Imported shading nodes the container still holds after reassignment."""
        leftovers = {"materials": [], "file": [], "place2d": [], "other": []}
        for node in self._namespace_members():
            node_type = commands().nodeType(node)
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


def _container_name_valid(name):
    """Whether ``name`` is a plain Maya node name this prototype may own."""
    if not isinstance(name, str) or not name.strip():
        return False
    if any(character in name for character in (":", "|", " ", "\t", "\n")):
        return False
    return re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) is not None


def _short_node_name(path):
    """Short name of a (possibly namespaced, possibly long) node path."""
    return str(path).rsplit("|", 1)[-1].split(":", 1)[-1]


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
    vector from the node's world position to it is the measurement that a
    mirrored arrival moves while position and size stay put.
    """
    maya_cmds = commands()
    shapes = maya_cmds.listRelatives(path, shapes=True, fullPath=True) or []
    targets = shapes if shapes else [path]
    box = maya_cmds.exactWorldBoundingBox(*targets)
    return ([float(box[0]), float(box[1]), float(box[2])],
            [float(box[3]), float(box[4]), float(box[5])])


def _local_bounds_size(path):
    """Local bounding-box size of the node's shapes, for the orientation check.

    Shapes are assumed to share the transform's space, which is what the FBX
    plugin builds; a shape's own ``boundingBoxMin/Max`` are in its object space.
    """
    maya_cmds = commands()
    shapes = maya_cmds.listRelatives(path, shapes=True, fullPath=True) or []
    low = None
    high = None
    for shape in shapes:
        try:
            minimum = maya_cmds.getAttr(shape + ".boundingBoxMin")
            maximum = maya_cmds.getAttr(shape + ".boundingBoxMax")
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
    unbounded, so at most ``cap`` faces are accumulated: a mesh at or below the
    cap is read whole, a larger one is sampled evenly and reports ``sampled``.
    """
    import maya.api.OpenMaya as open_maya
    maya_cmds = commands()
    selection = open_maya.MSelectionList()
    meshes = []
    for shape in maya_cmds.listRelatives(path, shapes=True, fullPath=True) or []:
        try:
            if not maya_cmds.nodeType(shape) == "mesh":
                continue
            selection.add(shape)
            meshes.append(open_maya.MFnMesh(selection.getDagPath(0)))
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
        points = mesh.getPoints(open_maya.MSpace.kWorld)
        for face in range(0, mesh.numPolygons, stride):
            corners = mesh.getPolygonVertices(face)
            if len(corners) < 3:
                continue
            faces_used += 1
            first = points[corners[0]]
            for step in range(1, len(corners) - 1):
                second = points[corners[step]]
                third = points[corners[step + 1]]
                edge_one = open_maya.MVector(second - first)
                edge_two = open_maya.MVector(third - first)
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


def _file_node_image_exists(node):
    """Whether a ``file`` node's image path exists on disk."""
    try:
        image = commands().getAttr(node + ".fileTextureName")
    except Exception:  # noqa: BLE001 - a missing attribute is just not loaded
        return False
    if not image:
        return False
    try:
        return os.path.exists(image)
    except (OSError, ValueError):
        return False


def run(fbx, manifest, container=DEFAULT_CONTAINER, allow_textures=False,
        keep_existing=False):
    """Import and verify one handoff; returns ``(report, exit_code)``."""
    return SceneRefImporter(fbx, manifest, container=container,
                            allow_textures=allow_textures,
                            keep_existing=keep_existing).run()


def write_report(report, path):
    """Write the report JSON, creating the evidence directory when needed."""
    target = Path(path)
    if target.parent and not target.parent.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(report, indent=2, ensure_ascii=True, default=str),
                      encoding="utf-8")
    return target


def summarise(report):
    """Short human readable summary lines for stdout."""
    counts = report.get("counts") or {}
    lines = [
        "phase: {0}  ok: {1}".format(report.get("phase"), report.get("ok")),
        "manifest objects: {0} exported of {1}".format(
            counts.get("manifest_objects"), counts.get("manifest_objects_total")),
        "container: {0} nodes, {1} mesh(es) on {2}".format(
            counts.get("container_nodes"), counts.get("meshes_assigned"),
            (report.get("gray_material") or {}).get("name")),
    ]
    texture_scan = report.get("texture_scan") or {}
    lines.append("texture records: {0} Texture ({1} naming a file) {2}".format(
        texture_scan.get("texture_records"), texture_scan.get("texture_references"),
        texture_scan.get("files")))
    transform_check = report.get("transform_check") or {}
    lines.append("axis candidate: {0} (matched {1}, max position error {2} cm)".format(
        transform_check.get("candidate"), transform_check.get("matched"),
        transform_check.get("max_position_error_cm")))
    for warning in report.get("warnings") or []:
        lines.append("warning: {0}".format(warning))
    for problem in report.get("problems") or []:
        lines.append("problem: {0}".format(problem))
    return lines


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fbx", required=True, help="the exported level FBX file")
    parser.add_argument("--manifest", required=True,
                        help="the mtou-scene-ref-manifest/1 JSON file")
    parser.add_argument("--report", required=True, help="path for the JSON report")
    parser.add_argument("--container", default=DEFAULT_CONTAINER,
                        help="container namespace and group name")
    parser.add_argument("--allow-textures", action="store_true",
                        help="import even when the handoff records textures")
    parser.add_argument("--keep-existing", action="store_true",
                        help="keep an existing container instead of replacing it")
    parser.add_argument("--json", action="store_true",
                        help="print the whole report JSON to stdout")
    args = parser.parse_args(argv)

    report = {
        "schema": mapping().REPORT_SCHEMA,
        "ok": False,
        "phase": "start",
        "problems": ["REFUSED {0}: {1}".format("NOT_STARTED", "the run did not start")],
    }
    exit_code = EXIT_USAGE
    try:
        ensure_maya()
        report, exit_code = run(args.fbx, args.manifest, container=args.container,
                                allow_textures=args.allow_textures,
                                keep_existing=args.keep_existing)
    except Exception:  # noqa: BLE001 - never raise out of main
        report = dict(report)
        report["phase"] = "failed"
        report["error"] = traceback.format_exc()
        report["problems"] = ["INTERNAL_ERROR: the run failed before it could report"]
        exit_code = EXIT_CHECK_FAILED
    try:
        write_report(report, args.report)
    except OSError as error:
        sys.stderr.write("cannot write report {0}: {1}\n".format(args.report, error))
        return EXIT_USAGE
    if args.json:
        print(json.dumps(report, indent=2, ensure_ascii=True, default=str))
    else:
        for line in summarise(report):
            print(line)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
