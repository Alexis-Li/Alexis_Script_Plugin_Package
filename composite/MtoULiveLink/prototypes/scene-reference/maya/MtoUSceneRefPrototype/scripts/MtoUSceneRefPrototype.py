"""Maya importer and verifier for the Issue 53 scene reference handoff.

The Unreal side exports the static geometry of one level scope into an ASCII FBX
plus a ``mtou-scene-ref-manifest/1`` measurement record. This module imports that
file into its own container, keeps the material assignment the file carries while
showing the geometry in one uniform gray, optionally moves it into the world the
camera route uses, and compares the measured Maya world matrices and bounding
boxes against the manifest, so the two hosts are checked against each other
rather than trusted. The frozen contract is
``composite/MtoULiveLink/prototypes/scene-reference/transfer.md``; this module
implements its "Maya report schema", "Media rule", "World", "Object naming" and
"Commands" sections and never changes the contract.

Usage::

    <mayapy> MtoUSceneRefPrototype.py --fbx <file> --manifest <file>
        --report <file> [--container MtoU_UE_SceneRef]
        [--target-world engine|camera] [--shading display|material|keep]
        [--allow-image-data] [--dry-run] [--json]

Exit codes: ``0`` when every check passed, ``1`` when a check failed (including a
handoff that delivers image data without ``--allow-image-data``), ``2`` for usage
and contract errors (an unreadable or non-FBX handoff, a manifest that is not
``mtou-scene-ref-manifest/1``, a scene or manifest unit that is not centimetres,
a manifest world that is not Z up, an unusable container name, an unknown world
or shading mode).

Safety contract
---------------
* The scene is never saved. Playback range, frame rate, current time and every
  existing key are left exactly as they were; the importer never calls
  ``save``, ``playbackOptions``, ``currentTime`` or ``setKeyframe``.
* The handoff is imported into a staging namespace first. Only after the
  comparison passed is the previous reference deleted and the staged one renamed
  into the container's name; a run that fails keeps the previous reference
  exactly as it was, and the staging namespace is deleted again. Nothing outside
  the container namespace is renamed, reparented, reassigned or deleted, so a
  production scene keeps its own objects and shading networks.
* A handoff that delivers image data -- image files in the directory the
  exporter owns, or media embedded in the FBX -- is refused unless
  ``--allow-image-data`` was passed; a material assignment and a recorded
  texture path are not image data and are imported, reported and never treated
  as a failure.
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

#: The namespace a run stages its import in before it replaces the reference. A
#: run takes the first free name of this family, so a namespace or node of that
#: name that belongs to the scene is never emptied, reused or deleted.
STAGING_SUFFIX = "_Incoming"

#: The name family a takeover moves the previous reference to, so the staged
#: namespace can take the container's name while the old one stays restorable.
RETIRING_SUFFIX = "_Retiring"

#: How many names of one family a run looks for before it refuses to run.
NAME_ATTEMPTS = 32

#: Ownership mark written onto the container group this tool creates and onto the
#: staging group of a run in flight. A container is replaceable only while the
#: mark names this tool, so a namespace or node of the same name that belongs to
#: the scene is refused instead of deleted.
OWNERSHIP_ATTRIBUTE = "mtouSceneRefContainer"
OWNERSHIP_TOKEN = "mtou-scene-ref-container/1"

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

EXIT_OK = 0
EXIT_CHECK_FAILED = 1
EXIT_USAGE = 2

CATEGORY_CONTAINER_NAME = "CONTAINER_NAME_INVALID"
CATEGORY_UNSUPPORTED_SCENE_UNIT = "UNSUPPORTED_SCENE_LINEAR_UNIT"
CATEGORY_UNSUPPORTED_MANIFEST_UNIT = "UNSUPPORTED_MANIFEST_LINEAR_UNIT"
CATEGORY_UNSUPPORTED_UP_AXIS = "UNSUPPORTED_MANIFEST_UP_AXIS"
CATEGORY_UNKNOWN_WORLD = "UNKNOWN_WORLD_TARGET"
CATEGORY_UNKNOWN_SHADING = "UNKNOWN_SHADING_MODE"
CATEGORY_FBX_UNREADABLE = "FBX_UNREADABLE"
CATEGORY_FBX_NOT_FBX = "FBX_NOT_FBX"
CATEGORY_FBX_PLUGIN = "FBX_PLUGIN_UNAVAILABLE"
CATEGORY_IMAGE_DATA_PRESENT = "IMAGE_DATA_PRESENT"
CATEGORY_IMPORT_FAILED = "FBX_IMPORT_FAILED"
CATEGORY_GRAY_MATERIAL = "GRAY_MATERIAL_CONFLICT"
CATEGORY_SWAP_FAILED = "STAGED_SWAP_FAILED"
CATEGORY_CONTAINER_NOT_OWNED = "CONTAINER_NOT_OWNED"
CATEGORY_RECOVERY_REFUSED = "RECOVERY_REFUSED"

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
                 target_world=None, shading=SHADING_DISPLAY,
                 allow_image_data=False, dry_run=False):
        self.fbx = str(fbx)
        self.manifest_path = str(manifest)
        self.container = str(container)
        self.target_world = (target_world or mapping().WORLD_ENGINE).strip().lower()
        self.shading = str(shading).strip().lower()
        self.allow_image_data = bool(allow_image_data)
        self.dry_run = bool(dry_run)
        self.manifest = None
        self.summary = {}
        self.scan = {}
        self.staging = None
        self.active_namespace = None
        self.staged = False
        self.takeover = None
        self.retired = None
        self.previous_state = None
        self.world_conversion = None
        self.frame_factor_reported = False
        self.report = {
            "schema": mapping().REPORT_SCHEMA,
            "ok": False,
            "phase": "start",
            "maya": {},
            "container": {"namespace": self.container, "group": self.container,
                          "group_path": None, "staging_namespace": None,
                          "staging_group_path": None, "previous_existed": False,
                          "previous_container_group": None,
                          "previous_reference_uuid": None,
                          "ownership_attribute": OWNERSHIP_ATTRIBUTE,
                          "ownership_token": OWNERSHIP_TOKEN,
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
                       "residual": None, "cleanup_errors": [],
                       "rollback_error": None},
            "counts": {"manifest_objects": 0, "container_nodes": 0,
                       "file_texture_nodes": 0, "image_nodes_loaded": 0},
            "media": {"handoff_directory": None,
                      "image_files_in_handoff_directory": [],
                      "embedded_media_records": 0, "content_records": 0,
                      "file_nodes_created": 0, "image_nodes_loaded": 0,
                      "image_paths_present": [], "allowed": False},
            "world": {"target": self.target_world, "target_map": None,
                      "target_note": None, "conversion_applied": False,
                      "conversion_matrix": None, "conversion_determinant": None,
                      "conversion_rotation_deg": None,
                      "conversion_seconds": None, "objects_converted": 0,
                      "manifest_conventions": None, "engine_check": None},
            "materials": {"mode": self.shading, "gray_material": None,
                          "display": None, "preserved_assignment": [],
                          "imported_material_nodes": [], "file_nodes": []},
            "texture_scan": {},
            "transform_check": {},
            "objects": [],
            # Present for the contract's shape; a run that never imported leaves
            # them null instead of claiming a duration it did not measure.
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
        """Run every phase and return the report; never raises."""
        started = time.time()
        exit_code = EXIT_OK
        try:
            self._read_manifest()
            self._read_scene()
            self._scan_handoff()
            self._stage_handoff()
            self._check_import_world()
            self._apply_world_conversion()
            self._apply_shading()
            self._verify()
            self._finalise()
            self.report["phase"] = "done"
        except SceneRefRefused as refusal:
            exit_code = refusal.exit_code
            self.report["phase"] = "refused"
            self.report["problems"].append(
                "REFUSED {0}: {1}".format(refusal.category, refusal.detail))
            self._cleanup_after_failure("refused")
        except mapping().ManifestError as refusal:
            exit_code = EXIT_USAGE
            self.report["phase"] = "refused"
            self.report["problems"].append(
                "REFUSED {0}: {1}".format(refusal.category, refusal.detail))
            self._cleanup_after_failure("refused")
        except Exception:  # noqa: BLE001 - the report carries the failure
            exit_code = EXIT_CHECK_FAILED
            self.report["phase"] = "failed"
            self.report["error"] = traceback.format_exc()
            self.report["problems"].append(
                "INTERNAL_ERROR: the run failed, see the report's error field")
            self._cleanup_after_failure("failed")
        finally:
            self.report["timing"]["total_seconds"] = round(time.time() - started, 3)
            rss, source = process_memory_mb()
            self.report["memory"] = {"process_rss_mb": rss,
                                     "process_rss_available": rss is not None,
                                     "source": source}
            # The recovery report is written from the scene as it is now, never
            # from what this run intended: whether the previous reference is still
            # there is read back, not assumed.
            self._record_container_state()
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
        self._check_manifest_conventions(summary.get("conventions"))
        self.report["manifest"] = summary
        self.report["counts"]["manifest_objects"] = summary["objects_exported"]
        self.report["counts"]["manifest_objects_total"] = summary["objects_total"]
        categories = {}
        for entry in mapping().exported_objects(manifest):
            key = entry.get("category") or "unknown"
            categories[key] = categories.get(key, 0) + 1
        self.report["counts"]["manifest_categories"] = categories
        coverage_warning = mapping().scope_coverage_warning(summary)
        if coverage_warning:
            self.report["warnings"].append(coverage_warning)
        if not summary["objects_exported"]:
            self.report["warnings"].append(
                "NO_EXPORTED_OBJECTS: the manifest declares no exported objects, "
                "so there is nothing to compare")
        self.report["phase"] = "manifest"

    def _check_manifest_conventions(self, conventions):
        """Compare the exporter's declared axis convention with this module's.

        The declared map is what the exporter believes its FBX carries; the
        comparison below measures the map the file really arrived in, so a
        disagreement is reported instead of silently trusted either way.
        """
        if not conventions:
            self.report["warnings"].append(
                "CONVENTIONS_ABSENT: the manifest declares no conventions block, "
                "so the exporter's own statement of the axis convention is not "
                "available; the measured candidates still decide the comparison")
            return
        declared = conventions.get("engine_to_maya_point_map")
        expected = mapping().ENGINE_HANDOFF_CANDIDATE
        if declared != expected:
            self.report["warnings"].append(
                "CONVENTIONS_DISAGREEMENT: the manifest declares the handoff map "
                "{0!r} while this host measures against {1!r}".format(
                    declared, expected))

    def _read_scene(self):
        """Record the Maya session and refuse a scene the comparison cannot use."""
        maya_cmds = commands()
        if not _container_name_valid(self.container):
            raise SceneRefRefused(
                CATEGORY_CONTAINER_NAME,
                "container name {0!r} is not a plain Maya node name".format(self.container))
        if self.shading not in SHADING_MODES:
            raise SceneRefRefused(
                CATEGORY_UNKNOWN_SHADING,
                "shading mode {0!r} is not one of {1}".format(
                    self.shading, ", ".join(SHADING_MODES)))
        if self.target_world not in [name for name, _candidate, _note
                                     in mapping().WORLD_TARGETS]:
            raise SceneRefRefused(
                CATEGORY_UNKNOWN_WORLD,
                "world target {0!r} is not one of {1}".format(
                    self.target_world,
                    ", ".join(name for name, _candidate, _note
                              in mapping().WORLD_TARGETS)))
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
        """Scan the FBX text and refuse a handoff that delivers image data.

        A recorded texture path is a reference, not a deliverable: the file may
        carry it, the geometry may keep its material, and the run only reports
        it. What a static reference handoff may not deliver is image data, and
        that is checked in two places: the files in the directory the exporter
        owns, and media embedded in the FBX itself.
        """
        scan = mapping().scan_fbx_file(self.fbx)
        self.scan = scan
        self.report["texture_scan"] = scan
        image_files = mapping().image_files_beside(self.fbx)
        media = {
            "handoff_directory": os.path.dirname(os.path.abspath(self.fbx)),
            "image_files_in_handoff_directory": image_files,
            "embedded_media_records": scan["embedded_media_records"],
            "embedded_media": scan["embedded_media"],
            "content_records": scan["content_records"],
            "texture_records": scan["texture_records"],
            "texture_references": scan["texture_references"],
            "camera_records": scan["camera_records"],
            "light_records": scan["light_records"],
            "media_heuristic": scan["media_heuristic"],
            "file_nodes_created": 0,
            "image_nodes_loaded": 0,
            "image_paths_present": [],
            "allowed": bool(self.allow_image_data),
        }
        self.report["media"] = media
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
        if scan["binary"]:
            self.report["warnings"].append(
                "BINARY_SCAN: the handoff is a binary FBX, so the media scan is a "
                "heuristic string table pass rather than a parsed document")
        self._compare_texture_scan(scan)
        if image_files:
            message = ("IMAGE_FILES_IN_HANDOFF: the directory the exporter owns "
                       "holds image files {0}; a static reference handoff must not "
                       "deliver copied or baked images".format(image_files))
            if self.allow_image_data:
                self.report["warnings"].append(message)
            else:
                raise SceneRefRefused(CATEGORY_IMAGE_DATA_PRESENT, message,
                                      exit_code=EXIT_CHECK_FAILED)
        if scan["embedded_media_records"]:
            detail = [(record.get("kind"), record.get("line"),
                       record.get("payload_characters"))
                      for record in scan["embedded_media"]]
            message = ("EMBEDDED_MEDIA_PRESENT: the handoff embeds media data in "
                       "{0} record(s) {1}; the reference geometry must not carry "
                       "image data".format(scan["embedded_media_records"], detail))
            if self.allow_image_data:
                self.report["warnings"].append(message)
            else:
                raise SceneRefRefused(CATEGORY_IMAGE_DATA_PRESENT, message,
                                      exit_code=EXIT_CHECK_FAILED)
        if scan["content_records"] > scan["embedded_media_records"]:
            self.report["warnings"].append(
                "CONTENT_WITHOUT_MEDIA: {0} Content record(s) carry no payload; "
                "they are reported but deliver no image data".format(
                    scan["content_records"] - scan["embedded_media_records"]))
        if scan["camera_records"] or scan["light_records"]:
            self.report["warnings"].append(
                "NON_GEOMETRY_RECORDS: the handoff carries {0} camera and {1} "
                "light node record(s); a static reference scope is reported to "
                "deliver none".format(scan["camera_records"], scan["light_records"]))
        self.report["texture_scan"]["allowed"] = bool(self.allow_image_data)
        self.report["phase"] = "scan"

    def _compare_texture_scan(self, scan):
        """Record where this scan and the exporter's own counts disagree."""
        output = self.summary.get("output") or {}
        for key in ("texture_records", "texture_references", "video_references",
                    "camera_records", "light_records", "embedded_media_records",
                    "content_records"):
            declared = output.get(key)
            if declared is None:
                continue
            if int(declared) != int(scan.get(key, 0)):
                self.report["warnings"].append(
                    "MEDIA_COUNT_DISAGREEMENT: the manifest reports {0}={1}, this "
                    "Maya scan found {2}".format(key, declared, scan.get(key, 0)))
        declared_images = output.get("image_files") or []
        if declared_images:
            self.report["warnings"].append(
                "EXPORTER_IMAGE_FILES: the exporter listed produced image files "
                "{0}; this run checks the directory itself".format(declared_images))
        declared_present = output.get("texture_reference_files_present") or []
        if declared_present:
            self.report["warnings"].append(
                "TEXTURE_PATHS_PRESENT: the recorded texture paths {0} exist on "
                "the exporter's machine; a reference to an existing file is not "
                "an exported image".format(declared_present))
        declared_names = output.get("node_names")
        if isinstance(declared_names, list) and declared_names:
            missing = [entry.get("node_name")
                       for entry in mapping().exported_objects(self.manifest)
                       if entry.get("node_name") not in declared_names]
            if missing:
                self.report["warnings"].append(
                    "NODE_NAMES_DISAGREEMENT: the exporter's output.node_names does "
                    "not hold the manifest node names {0}".format(missing))

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
        maya_cmds = commands()
        namespace_before = maya_cmds.namespaceInfo(currentNamespace=True)
        state_before = self._scene_state()
        recovery = self._recover_previous_runs()
        state = self._container_state(self.container)
        if state["conflict"]:
            raise SceneRefRefused(CATEGORY_CONTAINER_NOT_OWNED, state["conflict"],
                                  exit_code=EXIT_CHECK_FAILED)
        self.previous_state = state
        staging = self._free_name(self.container + STAGING_SUFFIX)
        self.staging = staging
        # The container block is written before anything is imported, so a run that fails
        # during the import still reports the previous reference it found and whether it is
        # still there afterwards, instead of leaving those fields at their defaults.
        previous_group = state["group"] or state["root_group"]
        previous_uuid = None
        if previous_group:
            uuids = maya_cmds.ls(previous_group, uuid=True) or []
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
            "previous_container_group": state["group"] or state["root_group"],
            "stale_staging_removed": bool(recovery["stale_staging_namespaces"]),
            "stale_staging_namespaces": recovery["stale_staging_namespaces"],
            "recovery": recovery,
            "previous_state": state["report"],
        })
        for removed in recovery["stale_staging_namespaces"]:
            self.report["warnings"].append(
                "STALE_STAGING_REMOVED: the staging namespace {0} was left behind by "
                "an interrupted run of this tool and has been deleted".format(removed))
        for entry in recovery["events"]:
            self.report["warnings"].append(entry)

        # The run owns the staging namespace from here on, including a namespace
        # that fails halfway through creation, so every later failure cleans up.
        self.staged = True
        maya_cmds.namespace(add=staging)
        maya_cmds.namespace(set=staging)
        try:
            maya_cmds.createNode("transform", name=staging)
        finally:
            maya_cmds.namespace(set=namespace_before)
        group = self._container_group(staging)
        self._mark_owned(group, staging)

        started = time.time()
        maya_cmds.namespace(set=staging)
        try:
            imported = maya_cmds.file(self.fbx, i=True, ns=staging, type="FBX",
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
        self.active_namespace = staging
        roots = self._imported_roots(group, staging)
        for node in roots:
            maya_cmds.parent(node, group, relative=True)
        self.report["timing"]["staging_seconds"] = round(time.time() - started, 3)
        self.report["timing"]["import_seconds"] = self.report["timing"]["staging_seconds"]
        self.report["fbx"] = {
            "file": self.fbx,
            "bytes": self.scan.get("bytes"),
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
        self.report["counts"]["container_nodes"] = len(self._namespace_members(staging))
        group_uuid = maya_cmds.ls(group, uuid=True) or []
        self.report["container"].update({
            "staging_group_path": group,
            "staging_group_uuid": group_uuid[0] if group_uuid else None,
        })
        self.report["phase"] = "staging"

    def _check_import_world(self):
        """Measure the world the file arrived in, before any conversion.

        The engine's handoff convention is a property of the file, not an
        assumption: when the caller asks for another world, this comparison is
        what proves the file really arrived in the handoff's world before the
        conversion was applied.
        """
        if self.target_world == mapping().WORLD_ENGINE:
            return
        comparison = self._compare_against_manifest()
        self.report["world"]["engine_check"] = comparison["transform_check"]
        if not comparison["transform_check"].get("matched"):
            self.report["warnings"].append(
                "IMPORT_CONVENTION_UNEXPECTED: the file did not arrive in the "
                "engine handoff's world: the comparison names {0}".format(
                    comparison["transform_check"].get("candidate")))

    def _apply_world_conversion(self):
        """Move the staged geometry into the world the caller asked for."""
        world = self.report["world"]
        world["target"] = self.target_world
        world["target_map"] = mapping().world_target_map(self.target_world)
        world["target_note"] = mapping().world_target_note(self.target_world)
        world["manifest_conventions"] = self.summary.get("conventions")
        if self.target_world == mapping().WORLD_ENGINE:
            world["conversion_applied"] = False
            self.report["phase"] = "world"
            return
        matrix, determinant = mapping().world_conversion_matrix(
            mapping().WORLD_ENGINE, self.target_world)
        self.world_conversion = matrix
        started = time.time()
        group = self._container_group(self.active_namespace)
        # Every root's world matrix is moved once; the children follow their
        # parent, so a level costs one query and one write per root, not per node.
        roots = commands().listRelatives(group, children=True, fullPath=True,
                                         type="transform") or []
        for node in roots:
            values = [float(value) for value in
                      commands().xform(node, query=True, worldSpace=True, matrix=True)]
            commands().xform(node, worldSpace=True,
                             matrix=mapping().carry_world_matrix(matrix, values))
        world.update({
            "conversion_applied": True,
            "conversion_matrix": mapping().matrix3_values(matrix),
            "conversion_determinant": determinant,
            "conversion_rotation_deg": mapping().matrix3_rotation_angle_degrees(matrix),
            "conversion_seconds": round(time.time() - started, 3),
            "objects_converted": len(roots),
        })
        self.report["phase"] = "world"

    def _apply_shading(self):
        """Show the reference in one gray, without pretending it carries no material.

        The material assignment the FBX carries is kept: the model may have
        material balls, and a texture path in a material is a reference, not a
        delivered image. What the artist needs to see is uniform gray, and that
        is a display treatment: the viewport override colors every imported shape
        gray and hides its textures, while the shading networks stay untouched
        and are reported.
        """
        maya_cmds = commands()
        namespace = self.active_namespace
        shapes = self._container_nodes_of_type("mesh", namespace)
        assignment = []
        for shape in shapes:
            assignment.append({
                "shape": shape,
                "shading_groups": list(maya_cmds.listConnections(shape,
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
                maya_cmds.setAttr(shape + ".overrideEnabled", True)
                maya_cmds.setAttr(shape + ".overrideShading", True)
                maya_cmds.setAttr(shape + ".overrideTexturing", True)
                maya_cmds.setAttr(shape + ".overrideColorRGB", *GRAY_DISPLAY_COLOR,
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
        for shape in shapes:
            before = maya_cmds.listConnections(shape, type="shadingEngine") or []
            maya_cmds.sets(shape, edit=True, forceElement=shading_group)
            after = maya_cmds.listConnections(shape, type="shadingEngine") or []
            if before != after:
                replaced += 1
        return {
            "name": shader,
            "type": str(maya_cmds.nodeType(shader)),
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
        a reported problem.
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
            [entry for entry in comparison["objects"] if entry.get("matched_by") == "name"])
        self.report["counts"]["matched_by_transform_count"] = len(transform_matched)
        self.report["timing"]["verify_seconds"] = round(time.time() - started, 3)
        self.report["phase"] = "verify"
        self._record_scene_after()

    def _compare_against_manifest(self):
        """Measure the staged container and compare it against the manifest."""
        index = self._container_index(self.active_namespace)
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
        comparison = mapping().compare_measurements(
            mapping().AXIS_CANDIDATES, measurements,
            frame=self._frame_factor(), conversion=self.world_conversion)
        if self._match_pending_by_transform(entries, comparison):
            measurements = [self._measure_manifest_object(entry) for entry in entries]
            comparison = mapping().compare_measurements(
                mapping().AXIS_CANDIDATES, measurements,
                frame=self._frame_factor(), conversion=self.world_conversion)
        return comparison

    def _frame_factor(self):
        """The node frame factor of this handoff, or ``None`` when unmeasured.

        The factor is a property of the export axis option the manifest
        declares. A manifest that carries no conventions block, or one that
        names an option whose factor was never measured, reports the orientation
        comparison as unavailable instead of comparing against a guess.
        """
        conventions = self.summary.get("conventions")
        if not conventions:
            return None
        option = conventions.get("export_axis_option")
        if option in mapping().MEASURED_ASYMMETRIC_FRAME_OPTIONS:
            return mapping().ENGINE_HANDOFF_LOCAL_FRAME
        if not self.frame_factor_reported:
            self.frame_factor_reported = True
            self.report["warnings"].append(
                "ORIENTATION_UNAVAILABLE: the manifest declares the export axis "
                "option {0!r}, whose node frame factor was never measured; the "
                "orientation comparison is reported as unavailable".format(option))
        return None

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
        from any of them (a host refusal, an error inside the read back) is caught
        here, the container this run created is deleted, the retired reference is
        renamed back, and the run reports the rollback instead of leaving a
        half-committed container behind.
        """
        update = self.report["update"]
        if self.dry_run or self.report["problems"]:
            self._discard_staging("dry_run" if self.dry_run else "check_failed")
            return
        started = time.time()
        maya_cmds = commands()
        try:
            self.retired = self._retire_container()
        except Exception as error:  # noqa: BLE001 - nothing was created yet
            raise SceneRefRefused(
                CATEGORY_SWAP_FAILED,
                "the previous reference could not be renamed aside: {0}".format(error),
                exit_code=EXIT_CHECK_FAILED)
        update["retired"] = self.retired
        self.takeover = {"namespace_renamed": False, "group_renamed": False,
                         "paths_checked": False, "marked": False}
        failure = None
        try:
            maya_cmds.namespace(rename=(self.staging, self.container))
            self.takeover["namespace_renamed"] = True
            maya_cmds.rename(self.container + ":" + self.staging,
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
            self._mark_owned(self._container_group(self.container), self.container)
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
                update["retired_removal"] = self._remove_retired()
            self.report["counts"]["container_nodes"] = len(
                self._namespace_members(self.container))
            self.report["phase"] = "finalise"
            return
        # The previous reference is still in its retiring name: put it back and delete
        # the container this run created, then report what happened.
        rollback = self._rollback_swap()
        update.update({
            "mode": "staged_swap_rolled_back",
            "swapped": False,
            "discarded": False,
            "rolled_back": True,
            "rollback_reason": failure,
            "rollback": rollback,
        })
        raise SceneRefRefused(CATEGORY_SWAP_FAILED, failure,
                              exit_code=EXIT_CHECK_FAILED)

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
        both, recording every cleanup error and whatever our nodes still occupy
        instead of letting a host refusal escape the public API.
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
                    "rollback_reason": update.get("rollback_reason") or
                                       "the run failed with the takeover in flight "
                                       "({0})".format(self._takeover_stage()),
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
        self.report["problems"].append("CLEANUP_ERROR: {0}".format(entry.splitlines()[0]))

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
        nodes = self._namespace_members(self.staging)
        # This run created this namespace -- its name was free when the run took it --
        # so everything in it appeared while the run held it and the namespace can go
        # as a whole, a partially imported node included.
        removed, error = self._remove_namespace(self.staging)
        self.staged = False
        self.active_namespace = None
        update["discard_attempted"] = True
        if error:
            update.setdefault("discard_errors", []).append(error)
        update["residual"] = self._residual_state(self.staging)
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
            state["namespace_present"] = bool(commands().namespace(exists=namespace))
            state["nodes"] = self._namespace_members(namespace)
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
            found = commands().ls(self.container + ":" + short, long=True) or []
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
        group = self._container_group(self.active_namespace)
        used = {entry["path"] for entry in entries if entry["path"]}
        available = [node for node in self._container_nodes_of_type("transform",
                                                                   self.active_namespace)
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

    # ------------------------------------------------- ownership and takeover

    def _container_state(self, name):
        """What the scene holds under ``name``, and whether it may be replaced.

        The answer comes from the nodes themselves, never from the name alone: a
        container is replaceable only while the group inside it -- or, for a build
        that wrote no group yet, the root level node of that name -- carries the
        ownership mark this tool writes with ``name``. Everything else found under
        that name is reported as foreign, so the caller can refuse instead of
        deleting scene content.
        """
        maya_cmds = commands()
        mark = _ownership_mark(name)
        state = {"name": name,
                 "namespace": bool(maya_cmds.namespace(exists=name)),
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
        if state["foreign_nodes"]:
            state["conflict"] = (
                "the name {0!r} is used by nodes this tool does not own and never "
                "deletes: {1}; rename or remove them, or run with another "
                "--container".format(name, _node_summary(state["foreign_nodes"])))
        elif state["incomplete"]:
            state["conflict"] = (
                "the namespace {0!r} holds a group without this tool's ownership "
                "mark naming it: {1}; the run does not touch it".format(
                    name, _node_summary(state["incomplete"])))
        elif state["namespace"] and not state["group"]:
            state["conflict"] = (
                "the namespace {0!r} exists but holds no container group with this "
                "tool's ownership mark ({1}), so it cannot be told apart from scene "
                "content; rename or remove it, or run with another --container".format(
                    name, OWNERSHIP_ATTRIBUTE))
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

    def _mark_owned(self, node, name):
        """Write this tool's ownership mark for ``name`` onto a group it created."""
        maya_cmds = commands()
        if not maya_cmds.attributeQuery(OWNERSHIP_ATTRIBUTE, node=node, exists=True):
            maya_cmds.addAttr(node, longName=OWNERSHIP_ATTRIBUTE, dataType="string")
        maya_cmds.setAttr(node + "." + OWNERSHIP_ATTRIBUTE,
                          _ownership_mark(name), type="string")

    def _free_name(self, base):
        """The first free name of ``base``, ``base_1``, ... no namespace or node uses.

        A run only ever takes a free name, so a namespace or node of the same name
        that belongs to the scene is never emptied, reused or deleted.
        """
        maya_cmds = commands()
        for index in range(NAME_ATTEMPTS):
            candidate = base if index == 0 else "{0}_{1}".format(base, index)
            if not _container_name_valid(candidate):
                continue
            if maya_cmds.namespace(exists=candidate):
                continue
            if _root_nodes(candidate) or maya_cmds.objExists(candidate):
                continue
            return candidate
        raise SceneRefRefused(
            CATEGORY_CONTAINER_NAME,
            "no free name of the {0!r} family is left for this run".format(base))

    def _existing_family_names(self, base):
        """Namespace names of one family that exist: ``base``, ``base_1``, ..."""
        names = []
        for name in commands().namespaceInfo(listOnlyNamespaces=True, recurse=True) or []:
            text = str(name)
            if text == base or re.fullmatch(re.escape(base) + r"_\d+", text):
                names.append(text)
        return sorted(names)

    def _owns_marked_group(self, namespace, mark):
        """Whether ``namespace`` holds a top level group carrying ``mark``.

        Used only where *this run* created the namespace itself -- the staging
        namespace it took a free name for, and the container name its own staging
        namespace was renamed into -- so a matching mark is proof that the group is
        the one it put there. A namespace the run found in the scene never reaches a
        wholesale removal through this check.
        """
        return bool([path for path in _namespace_top_nodes(namespace)
                     if _ownership_token(path) == mark])

    def _namespace_ownership(self, name, accepted_marks):
        """Whether every node at the top of one namespace is provably this tool's.

        A run marks the group it creates and nothing else, so the only nodes it can
        prove are its own are marked top-level nodes plus the placement nodes the FBX
        plugin adds beside the geometry. A namespace that also holds unmarked
        top-level nodes is *not* entirely ours: cleanup then deletes only the marked
        nodes and reports the rest, because the tool cannot tell a leftover of its own
        interrupted import from an object the scene put there.
        """
        marks = [mark for mark in accepted_marks if mark]
        state = {"namespace": name, "exists": bool(commands().namespace(exists=name)),
                 "marked": [], "foreign": []}
        for path in _namespace_top_nodes(name):
            token = _ownership_token(path)
            if token in marks:
                state["marked"].append(path)
            elif _node_type_in(path, TOLERATED_CONTAINER_TOP_TYPES):
                continue
            else:
                state["foreign"].append(path)
        state["ours_only"] = bool(state["marked"]) and not state["foreign"]
        return state

    def _remove_owned_nodes(self, namespace, accepted_marks):
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
            try:
                commands().delete(path)
            except Exception as error:  # noqa: BLE001 - reported, never raised
                errors.append("the marked node {0} could not be deleted: {1}".format(
                    path, error))
                left.append(path)
                continue
            if commands().objExists(path):
                errors.append("the marked node {0} is still there after the host "
                              "accepted its deletion".format(path))
                left.append(path)
            else:
                removed.append(path)
        return removed, left, errors

    def _recover_previous_runs(self):
        """Undo or finish what an interrupted run of this tool left behind.

        Only resources this tool can prove it created are touched, and a state the
        run cannot resolve is refused instead of guessed at.

        A namespace holding a previous reference in its retiring name is put back
        under the container's name -- but only while every one of its top-level nodes
        carries the container's mark, because a previous reference is restored or
        deleted whole, never in parts. One that also holds unmarked nodes is refused
        with its content named, so nobody can mistake an unknown arrangement for a
        finished takeover.

        A staging namespace of this tool's family is cleaned up node by node: the
        marked group is deleted, and a namespace that still holds unmarked nodes
        afterwards is left in place and reported instead of being emptied.

        The container name itself decides the rest. A half-swapped namespace -- one
        whose takeover was interrupted before the ownership mark was rewritten --
        holds only this tool's staged import, so it is deleted whole and the
        retiring reference put back. That deletion refuses the run rather than
        working around a host refusal when the namespace also holds a node the tool
        does not own: neither putting the old reference back nor completing an
        unverified takeover is safe while the container's name is still taken. A
        finished takeover (its group carries the mark for the container's own name)
        is the reference a retiring copy was replaced by, so that copy is deleted;
        a host that refuses the deletion refuses the run instead of reporting a
        success with the copy still in the scene.

        Only when no previous reference is waiting in any retiring name does the
        run finish a half-swapped container by renaming and re-marking its group:
        the staged content was verified before its takeover started, so the group
        name and the mark are what an interruption can have cost it, and writing
        the completion mark is then the only way back to a state a later run can
        work with. The mark is never written next to a retiring copy that still
        exists.
        """
        maya_cmds = commands()
        recovery = {"stale_staging_namespaces": [], "restored_previous_reference": None,
                    "retired_leftovers_removed": [],
                    "interrupted_container_removed": False,
                    "finished_interrupted_container": None,
                    "skipped_namespaces": [], "partial_cleanups": [],
                    "events": []}
        container_state = self._container_state(self.container)
        for name in self._existing_family_names(self.container + RETIRING_SUFFIX):
            state = self._namespace_ownership(name, [_ownership_mark(self.container)])
            if not state["exists"]:
                continue
            if not state["ours_only"]:
                raise SceneRefRefused(
                    CATEGORY_CONTAINER_NOT_OWNED,
                    "the namespace {0!r} holds a previous reference of this tool next to "
                    "nodes it does not own ({1}), so it is neither restored nor "
                    "deleted; rename or remove it and run again".format(
                        name, _node_summary(state["foreign"] or
                                            _namespace_top_nodes(name))),
                    exit_code=EXIT_CHECK_FAILED)
            if container_state["group"] or container_state["root_group"]:
                # The container under that name is a finished takeover, so this
                # retiring copy is the reference that takeover replaced. It is
                # deleted as a whole namespace or the run refuses with it still
                # named: reporting a success while an unhandled copy of a previous
                # reference sits in the scene is what a retry must never do.
                removed, error = self._remove_namespace(name)
                if not removed:
                    raise SceneRefRefused(
                        CATEGORY_RECOVERY_REFUSED,
                        "the retired reference {0!r} of the finished container could "
                        "not be deleted ({1}), so the run is refused instead of "
                        "reporting a success with the copy still in the scene; run "
                        "again once the name can be removed".format(name, error),
                        exit_code=EXIT_CHECK_FAILED)
                recovery["retired_leftovers_removed"].append(name)
                recovery["events"].append(
                    "RETIRED_LEFTOVER_REMOVED: the interrupted run's previous "
                    "reference {0} was a copy of the verified container and has "
                    "been deleted".format(name))
                continue
            if container_state["incomplete"]:
                # A half-swapped namespace: the takeover never wrote the mark that
                # proves it, so its top level holds nothing but this tool's staged
                # import. A namespace that also holds a node the tool does not own
                # is refused with its content named -- it is never emptied -- and a
                # host that refuses the removal refuses the run, because the
                # previous reference stays in its retiring name and the container's
                # name stays taken by an unverified takeover until it is dealt with.
                if container_state["foreign_nodes"]:
                    raise SceneRefRefused(
                        CATEGORY_CONTAINER_NOT_OWNED,
                        "the namespace {0!r} was left half swapped by an interrupted "
                        "run next to nodes this tool does not own ({1}), so the "
                        "previous reference {2!r} is neither restored nor deleted; "
                        "rename or remove the foreign nodes and run again".format(
                            self.container,
                            _node_summary(container_state["foreign_nodes"]), name),
                        exit_code=EXIT_CHECK_FAILED)
                removed, error = self._remove_namespace(self.container)
                recovery["interrupted_container_removed"] = bool(removed)
                if not removed:
                    raise SceneRefRefused(
                        CATEGORY_RECOVERY_REFUSED,
                        "the half swapped namespace {0!r} could not be deleted ({1}), "
                        "so the previous reference {2!r} stays in its retiring name "
                        "and the run is refused instead of completing a takeover "
                        "that was never verified; run again once the name can be "
                        "removed".format(self.container, error, name),
                        exit_code=EXIT_CHECK_FAILED)
                recovery["events"].append(
                    "INTERRUPTED_CONTAINER_REMOVED: the half swapped namespace {0} "
                    "was left by an interrupted run and has been deleted".format(
                        self.container))
            try:
                maya_cmds.namespace(rename=(name, self.container))
            except Exception as error:  # noqa: BLE001 - reported, never raised
                raise SceneRefRefused(
                    CATEGORY_RECOVERY_REFUSED,
                    "the previous reference {0!r} could not be put back under the "
                    "container's name {1!r} ({2}); it stays in its retiring name "
                    "until a run can restore it".format(name, self.container, error),
                    exit_code=EXIT_CHECK_FAILED)
            recovery["restored_previous_reference"] = name
            recovery["events"].append(
                "RESTORED_PREVIOUS_REFERENCE: the previous reference {0} was put "
                "back under the container's name {1} after an interrupted "
                "run".format(name, self.container))
            container_state = self._container_state(self.container)
        if (not recovery["restored_previous_reference"]
                and not container_state["group"] and not container_state["root_group"]
                and container_state["incomplete"]
                and not container_state["foreign_nodes"]):
            # The container namespace holds a group this tool staged but never renamed
            # and no previous reference is waiting in a retiring name, so the
            # interrupted run had already deleted it: finishing the takeover is what
            # restores the state the contract promises instead of refusing to run.
            # A namespace that also holds foreign nodes never reaches this point --
            # with a retiring copy it is refused above, without one the container's
            # own conflict check refuses it before anything is imported.
            finished = self._finish_interrupted_container(container_state)
            recovery["finished_interrupted_container"] = finished
            if finished:
                recovery["events"].append(
                    "FINISHED_INTERRUPTED_TAKEOVER: the namespace {0} was left by an "
                    "interrupted run and its group has been renamed and marked as the "
                    "container".format(self.container))
                container_state = self._container_state(self.container)
        for name in self._existing_family_names(self.container + STAGING_SUFFIX):
            state = self._namespace_ownership(name, [_ownership_mark(name)])
            if not state["exists"] or not state["marked"]:
                continue
            removed, left, errors = self._remove_owned_nodes(
                name, [_ownership_mark(name)])
            if left or errors:
                recovery["partial_cleanups"].append({
                    "namespace": name, "removed": removed, "left": left,
                    "errors": errors})
                recovery["events"].append(
                    "STALE_STAGING_PARTIAL: the staging namespace {0} held nodes this "
                    "tool cannot prove are its own ({1}), so only its marked group was "
                    "deleted and the rest was left exactly as it is".format(
                        name, _node_summary(left)))
                continue
            if not _namespace_top_nodes(name):
                removed_namespace, error = self._remove_namespace(name)
                if removed_namespace:
                    recovery["stale_staging_namespaces"].append(name)
                else:
                    recovery["events"].append(
                        "STALE_STAGING_KEPT: the staging namespace {0} could not be "
                        "deleted: {1}".format(name, error))
            else:
                recovery["partial_cleanups"].append({
                    "namespace": name, "removed": removed, "left": [],
                    "errors": []})
        return recovery

    def _finish_interrupted_container(self, state):
        """Rename and re-mark the group of a container an interrupted run left behind.

        The container namespace holds a group this tool staged -- its mark names the
        staging family -- and no previous reference is waiting in a retiring name,
        so the interrupted run had already deleted it. Completing the takeover (the
        group name and mark the contract promises) is then the only way back to a
        state a later run can work with: the staged content itself was verified
        before its takeover started, so the group name and the mark are what the
        interruption can have cost it.

        This runs only while no retiring copy exists and the namespace holds no node
        the tool does not own: a completion mark is never written next to an
        unresolved previous reference, and one marked group of an unknown
        arrangement is not an authorisation to own a namespace. Returns whether the
        group was finished.
        """
        incomplete = list(state["incomplete"])
        if len(incomplete) != 1:
            return False
        node = incomplete[0]
        if _short_node_name(node) != self.container:
            found = commands().ls(self.container + ":" + self.container, long=True,
                                  type="transform") or []
            node = found[0] if found else commands().rename(
                node, self.container + ":" + self.container)
        self._mark_owned(node, self.container)
        return True

    def _retire_container(self):
        """Move the previous, owned reference to a free retiring name.

        Renaming, never deleting: the previous reference keeps every node and its
        UUID, so a takeover that fails anywhere before it is complete puts it back
        under the container's name without changing what scene references point at.
        """
        state = self.previous_state or self._container_state(self.container)
        previous_group = state["group"] or state["root_group"]
        if not previous_group:
            return None
        maya_cmds = commands()
        retired = {"previous_group": previous_group, "retired_namespace": None,
                   "retired_root_group": None, "previous_uuid": None}
        uuid_values = maya_cmds.ls(previous_group, uuid=True) or []
        retired["previous_uuid"] = uuid_values[0] if uuid_values else None
        if state["group"]:
            name = self._free_name(self.container + RETIRING_SUFFIX)
            maya_cmds.namespace(rename=(self.container, name))
            retired["retired_namespace"] = name
        if state["root_group"]:
            name = self._free_name(self.container + RETIRING_SUFFIX)
            maya_cmds.rename(state["root_group"], name)
            retired["retired_root_group"] = name
        return retired

    def _remove_retired(self):
        """Delete the retired previous reference once the takeover is complete."""
        retired = self.retired
        if not retired:
            return {"removed": False, "namespace_removed": False,
                    "root_group_removed": False, "errors": []}
        maya_cmds = commands()
        errors = []
        namespace_removed = False
        if retired.get("retired_namespace"):
            namespace_removed, error = self._remove_namespace(
                retired["retired_namespace"])
            if error:
                errors.append(error)
        root_group_removed = False
        if retired.get("retired_root_group"):
            try:
                if maya_cmds.objExists(retired["retired_root_group"]):
                    maya_cmds.delete(retired["retired_root_group"])
                    root_group_removed = True
            except Exception as error:  # noqa: BLE001 - reported, the run continues
                errors.append("the retired group {0} could not be deleted: {1}".format(
                    retired["retired_root_group"], error))
        return {"removed": bool(namespace_removed or root_group_removed),
                "namespace_removed": namespace_removed,
                "root_group_removed": root_group_removed, "errors": errors}

    def _restore_retired(self):
        """Put the retired previous reference back under the container's name.

        A restore that the host refuses is reported, never raised: the run has to keep the
        scene's real state on the record, and the previous reference is still in its
        retiring name in that case.
        """
        retired = self.retired
        if not retired:
            return {"restored": False, "note": "no previous reference was retired"}
        maya_cmds = commands()
        restored = {"restored": False, "namespace": False, "root_group": False,
                    "note": None, "errors": []}
        if retired.get("retired_namespace"):
            if maya_cmds.namespace(exists=self.container):
                restored["errors"].append(
                    "the container name {0!r} is still taken, so the retired "
                    "namespace {1!r} was left in place".format(
                        self.container, retired["retired_namespace"]))
                restored["note"] = ("the previous reference stays in its retiring "
                                    "name and is recoverable by the next run")
                return restored
            try:
                maya_cmds.namespace(rename=(retired["retired_namespace"], self.container))
                restored["namespace"] = True
            except Exception as error:  # noqa: BLE001 - reported, never raised
                restored["errors"].append(
                    "the retired namespace {0} could not be renamed back to {1}: "
                    "{2}".format(retired["retired_namespace"], self.container, error))
        if retired.get("retired_root_group"):
            try:
                maya_cmds.rename(retired["retired_root_group"], self.container)
                restored["root_group"] = True
            except Exception as error:  # noqa: BLE001 - reported, never raised
                restored["errors"].append(
                    "the retired group {0} could not be renamed back to {1}: {2}".format(
                        retired["retired_root_group"], self.container, error))
        restored["restored"] = bool(restored["namespace"] or restored["root_group"])
        if not restored["restored"]:
            restored["note"] = ("the previous reference could not be put back under the "
                                "container's name" if restored["errors"]
                                else "the previous reference was already removed")
        return restored

    def _remove_created_container(self, accepted_marks):
        """Delete a container namespace this run created out of its staging import.

        Only a namespace that holds a marked group this run wrote is deleted: the mark it
        staged under (the staging name), or the mark it rewrote once the group had taken the
        container's name. A namespace with no such mark is left untouched and reported,
        because this run cannot prove it created it.
        """
        maya_cmds = commands()
        marks = [mark for mark in accepted_marks if mark]
        result = {"namespace_removed": False, "nodes_removed": [],
                  "skipped_reason": None, "accepted_marks": marks}
        if not maya_cmds.namespace(exists=self.container):
            return result
        if not any(self._owns_marked_group(self.container, mark) for mark in marks):
            result["skipped_reason"] = (
                "the namespace {0!r} holds no group this run staged (accepted marks "
                "{1}), so it is left untouched".format(self.container, marks))
            return result
        result["nodes_removed"] = self._namespace_members(self.container)
        removed, error = self._remove_namespace(self.container)
        result["namespace_removed"] = bool(removed)
        if error:
            result["skipped_reason"] = error
        return result

    def _rollback_swap(self):
        """Put the previous reference back after a takeover that did not complete.

        The staged content is deleted first -- it is this run's own, and its group
        carries the mark of the staging name -- so the retiring name is free when
        the previous reference is renamed back under the container's name.
        """
        rollback = {"container_namespace_removed": False, "container_nodes_removed": [],
                    "staging_namespace_removed": False, "container_skip_reason": None,
                    "errors": [], "previous_reference_restored": False, "restore": None}
        if self.staged and self.staging and commands().namespace(exists=self.staging):
            removed, error = self._remove_namespace(self.staging)
            rollback["staging_namespace_removed"] = bool(removed)
            if error:
                rollback["errors"].append(error)
        marks = [_ownership_mark(self.container)]
        if self.staging:
            marks.append(_ownership_mark(self.staging))
        removal = self._remove_created_container(marks)
        rollback["container_namespace_removed"] = removal["namespace_removed"]
        rollback["container_nodes_removed"] = removal["nodes_removed"]
        rollback["container_skip_reason"] = removal["skipped_reason"]
        if removal["skipped_reason"] and not removal["namespace_removed"]:
            rollback["errors"].append(removal["skipped_reason"])
        self.staged = False
        self.takeover = None
        self.active_namespace = None
        restore = self._restore_retired()
        rollback["restore"] = restore
        rollback["previous_reference_restored"] = bool(restore.get("restored"))
        rollback["errors"].extend(restore.get("errors") or [])
        rollback["residual"] = self._residual_state(self.container)
        return rollback

    def _record_container_state(self):
        """Write the container fields from the scene as it is, not from intent.

        ``kept_existing`` answers a question about the scene: is the very node this
        run found still there, read back by UUID? A completed swap deletes it, a
        failed one keeps or restores it, and the report says which happened even
        when the failure was not the one the run expected.
        """
        try:
            maya_cmds = commands()
            container = self.report.get("container") or {}
            previous_uuid = container.get("previous_reference_uuid")
            found = maya_cmds.ls(previous_uuid, long=True) if previous_uuid else None
            state = self._container_state(self.container)
            staging_present = bool(self.staging and maya_cmds.namespace(exists=self.staging))
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

    # -------------------------------------------------------------- helpers

    def _remove_namespace(self, namespace):
        """Delete a namespace and everything in it; never raises.

        Returns ``(removed, error)``. A host that refuses the removal is a cleanup
        failure the report carries: every caller records it, so a run always returns
        its report and names the resources it could not delete instead of raising
        out of the API.
        """
        maya_cmds = commands()
        if not namespace or not maya_cmds.namespace(exists=namespace):
            return False, None
        try:
            maya_cmds.namespace(removeNamespace=namespace,
                                deleteNamespaceContent=True)
        except Exception as error:  # noqa: BLE001 - reported by the caller
            return False, "the namespace {0!r} could not be deleted: {1}".format(
                namespace, error)
        if maya_cmds.namespace(exists=namespace):
            return False, ("the namespace {0!r} is still there after the host "
                           "accepted its removal".format(namespace))
        return True, None

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

    def _namespace_members(self, namespace):
        return list(commands().namespaceInfo(namespace, listNamespace=True) or [])

    def _container_nodes_of_type(self, node_type, namespace=None):
        """Nodes of ``node_type`` inside one namespace."""
        namespace = namespace or self.active_namespace or self.container
        return list(commands().ls(namespace + ":*", long=True, type=node_type) or [])

    def _container_group(self, namespace):
        """Long name of the container transform inside its namespace."""
        nodes = commands().ls(namespace + ":" + namespace, long=True,
                              type="transform") or []
        if not nodes:
            raise SceneRefRefused(
                CATEGORY_SWAP_FAILED,
                "the container group {0}:{0} was not created".format(namespace),
                exit_code=EXIT_CHECK_FAILED)
        return nodes[0]

    def _imported_roots(self, group, namespace):
        """Imported container transforms that are still at the top of the scene."""
        maya_cmds = commands()
        roots = []
        for node in self._container_nodes_of_type("transform", namespace):
            if node == group:
                continue
            parent = maya_cmds.listRelatives(node, parent=True, fullPath=True)
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
        for node in self._namespace_members(namespace):
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


def _ownership_mark(name):
    """The ownership value this tool writes for the namespace ``name``."""
    return "{0} {1}".format(OWNERSHIP_TOKEN, name)


def _ownership_token(node):
    """The ownership value recorded on ``node``, or ``None`` when it carries none.

    The mark is an ordinary string attribute, so it is read back from the node
    itself: a node without it may belong to the scene, and the run never deletes
    such a node. An attribute that cannot be read counts as no mark.
    """
    try:
        if not commands().attributeQuery(OWNERSHIP_ATTRIBUTE, node=node,
                                         exists=True):
            return None
        value = commands().getAttr(node + "." + OWNERSHIP_ATTRIBUTE)
    except Exception:  # noqa: BLE001 - an unreadable mark is no mark
        return None
    return str(value) if value is not None else None


def _namespace_top_nodes(namespace):
    """Long paths of the DAG nodes sitting at the top of one namespace.

    A node at the top of a namespace has a one step long path; anything deeper
    hangs under one of those nodes, so only the top level answers whether the
    namespace holds content of its own.
    """
    nodes = commands().ls(namespace + ":*", long=True, type="transform") or []
    return [path for path in nodes if str(path).count("|") == 1]


def _root_nodes(name):
    """Long paths of the nodes carrying ``name`` at the top of the scene."""
    nodes = commands().ls(name, type="transform", long=True) or []
    return [path for path in nodes if str(path).count("|") == 1 and ":" not in str(path)]


def _node_type_in(path, types):
    """Whether the node's type is one of ``types``."""
    try:
        return str(commands().nodeType(path)) in types
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


def _identification_size(local_size, world_matrix):
    """The extents that decide whether a mesh's axes can be identified.

    The shape's local bounding-box extents are scaled by the node's own axis
    scale factors -- the lengths of the world matrix's axis rows -- because a
    non-uniform scale separates axes that a symmetric mesh leaves ambiguous. A
    cube whose world matrix scales it ``(2, 0.5, 0.25)`` produces images of
    three different lengths, so an axis permutation or a sign flip changes the
    geometry and must be reported; the same cube at uniform scale does not.
    """
    if not local_size or len(local_size) != 3 or not world_matrix:
        return None
    rows = mapping().matrix4_linear(world_matrix)
    return [float(local_size[axis]) * mapping().vector_length(rows[axis])
            for axis in range(3)]


def _file_node_image_path(node):
    """The image path a ``file`` node names, or ``None``."""
    try:
        image = commands().getAttr(node + ".fileTextureName")
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


def run(fbx, manifest, container=DEFAULT_CONTAINER, target_world=None,
        shading=SHADING_DISPLAY, allow_image_data=False, dry_run=False):
    """Import and verify one handoff; returns ``(report, exit_code)``."""
    return SceneRefImporter(fbx, manifest, container=container,
                            target_world=target_world, shading=shading,
                            allow_image_data=allow_image_data,
                            dry_run=dry_run).run()


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
    world = report.get("world") or {}
    media = report.get("media") or {}
    update = report.get("update") or {}
    lines = [
        "phase: {0}  ok: {1}".format(report.get("phase"), report.get("ok")),
        "manifest objects: {0} exported of {1}".format(
            counts.get("manifest_objects"), counts.get("manifest_objects_total")),
        "world: {0} (map {1}, conversion applied {2})".format(
            world.get("target"), world.get("target_map"),
            world.get("conversion_applied")),
        "update: {0} (swapped {1}, discarded {2})".format(
            update.get("mode"), update.get("swapped"), update.get("discarded")),
        "media: {0} embedded record(s), {1} image file(s) in the handoff directory, "
        "{2} file node(s), {3} resolving to an existing image".format(
            media.get("embedded_media_records"),
            len(media.get("image_files_in_handoff_directory") or []),
            media.get("file_nodes_created"), media.get("image_nodes_loaded")),
        "container: {0} nodes".format(counts.get("container_nodes")),
    ]
    texture_scan = report.get("texture_scan") or {}
    lines.append("texture records: {0} Texture(s) ({1} naming a file) {2}".format(
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
    parser.add_argument("--target-world", default="engine",
                        help="the world to place the geometry in: engine (the "
                             "handoff's own world, shared with the animation "
                             "route) or camera (the camera sync route's world)")
    parser.add_argument("--shading", default=SHADING_DISPLAY,
                        help="display (uniform gray viewport override, materials "
                             "kept), material (assign the gray lambert) or keep")
    parser.add_argument("--allow-image-data", action="store_true",
                        help="import even when the handoff directory holds image "
                             "files or the FBX embeds media data")
    parser.add_argument("--dry-run", action="store_true",
                        help="stage and verify the handoff but keep the existing "
                             "reference as it is")
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
                                target_world=args.target_world, shading=args.shading,
                                allow_image_data=args.allow_image_data,
                                dry_run=args.dry_run)
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
