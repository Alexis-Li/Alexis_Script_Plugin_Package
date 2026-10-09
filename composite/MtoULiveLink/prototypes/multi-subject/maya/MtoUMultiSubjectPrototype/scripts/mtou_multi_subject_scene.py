"""Maya fixtures, reference session scenes and subject guards.

This is the Maya half of the bounded two-subject prototype. It owns three
things and nothing else:

* the checked-in fixture recipe (``mtou_multi_subject_recipe.json``) and the
  code that materialises it into independent ``.ma`` rig files under a scratch
  directory outside the repository;
* session scenes that reference two of those rigs by namespace, plus the
  read-only path for a supplied real scene;
* the subject guards: explicit root resolution, ancestor/socket reporting,
  subject capture through the product's ``_capture_subject`` and per-subject
  sampling through the product's ``_sample_pose``/``convert_transform``.

Maya is reached through ``maya.cmds`` only, imported lazily, so the module can
be imported and inspected on a plain CPython host. Nothing here saves the
session scene, writes keys into a scene it did not create, or edits a
referenced file.
"""

import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path

#: Repository root, then the product script whose sampling contract this
#: prototype reuses (``convert_transform``, ``_sample_pose``, ``_capture_subject``).
_MODULE_PATH = Path(__file__).resolve()
REPOSITORY_ROOT = _MODULE_PATH.parents[7]
PRODUCT_SCRIPT = (REPOSITORY_ROOT / "composite" / "MtoULiveLink" / "maya"
                  / "MtoULiveLink" / "scripts" / "MtoULiveLink.py")
RECIPE_PATH = _MODULE_PATH.parent / "mtou_multi_subject_recipe.json"

SCRATCH_ENVIRONMENT = "MTOU_MULTI_SUBJECT_SCRATCH"
DEFAULT_SCRATCH_PARTS = ("mtou-issue54", "multi-subject")

CODE_RECIPE_INVALID = "RECIPE_INVALID"
CODE_MISSING_ROOT = "MISSING_ROOT"
CODE_AMBIGUOUS_ROOT = "AMBIGUOUS_ROOT"
CODE_NOT_A_JOINT = "NOT_A_JOINT"
CODE_DUPLICATE_ROOT = "DUPLICATE_ROOT"
CODE_ROLE_SWAP = "ROLE_SWAP"
CODE_WRONG_BONES = "WRONG_BONES"
CODE_WRONG_CURVES = "WRONG_CURVES"
CODE_CURVE_CROSS_TALK = "CURVE_CROSS_TALK"
CODE_ANIMATED_ANCESTOR = "ANIMATED_ANCESTOR"
CODE_SUBJECT_MESH_MISSING = "SUBJECT_MESH_MISSING"
CODE_SUBJECT_SKELETON_INCOMPLETE = "SUBJECT_SKELETON_INCOMPLETE"
CODE_PRODUCT_CAPTURE_FAILED = "PRODUCT_CAPTURE_FAILED"
CODE_SAMPLING_FAILED = "SAMPLING_FAILED"
CODE_UNSUPPORTED_SCENE_LINEAR_UNIT = "UNSUPPORTED_SCENE_LINEAR_UNIT"
CODE_SCENE_FPS_MISMATCH = "SCENE_FPS_MISMATCH"
CODE_FIXTURE_MISSING = "FIXTURE_MISSING"
CODE_SCENE_OPEN_FAILED = "SCENE_OPEN_FAILED"
CODE_REFERENCE_COUNT = "REFERENCE_COUNT"
CODE_PRODUCT_UNAVAILABLE = "PRODUCT_UNAVAILABLE"
CODE_CURVE_OVERRIDE = "CURVE_OVERRIDE"

_cmds_module = None
_product_module = None


class SceneRefused(RuntimeError):
    """A configuration or scene this prototype refuses instead of guessing."""

    def __init__(self, code, message="", details=""):
        self.code = code
        self.details = details or message or code
        super(SceneRefused, self).__init__(
            "{0}: {1}".format(code, self.details))


def ensure_maya():
    """Make ``maya.cmds`` usable, initializing Maya on a plain CPython host.

    ``import maya.cmds`` succeeds in ``mayapy`` before the interpreter is
    initialized, but the module has no commands yet; the probe below is the
    same one a GUI session passes without any initialization.
    """
    global _cmds_module
    if _cmds_module is not None:
        return _cmds_module
    try:
        import maya.cmds as cmds
    except ImportError:
        cmds = None
    if cmds is None or not hasattr(cmds, "file"):
        import maya.standalone
        maya.standalone.initialize(name="python")
        import maya.cmds as cmds
    _cmds_module = cmds
    return cmds


def cmds():
    """``maya.cmds``, imported (and Maya initialized) on first use."""
    return ensure_maya()


def product():
    """The product script, imported after Maya is available.

    Importing it before Maya starts would leave its ``cmds``/``om`` module
    globals as ``None``; importing it in a running Maya keeps this prototype on
    the product's exact axis conversion, bind resolution and pose sampling.
    """
    global _product_module
    if _product_module is not None:
        return _product_module
    ensure_maya()
    if not PRODUCT_SCRIPT.is_file():
        raise SceneRefused(
            CODE_PRODUCT_UNAVAILABLE,
            "product script not found",
            str(PRODUCT_SCRIPT))
    spec = importlib.util.spec_from_file_location(
        "MtoU_MultiSubject_Product", str(PRODUCT_SCRIPT))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    _product_module = module
    return module


# ------------------------------------------------------------------ scratch

def repository_parent():
    return REPOSITORY_ROOT.parent


def scratch_directory():
    """Default scratch root for generated rigs: ``<repo>/../.tmp``.

    Generated ``.ma`` files never belong in the repository; the recipe that
    produces them is checked in instead. ``MTOU_MULTI_SUBJECT_SCRATCH``
    overrides the location.
    """
    override = os.environ.get(SCRATCH_ENVIRONMENT)
    if override:
        return Path(override)
    external = repository_parent() / ".tmp"
    base = external if os.access(str(repository_parent()), os.W_OK) else \
        REPOSITORY_ROOT / ".tmp"
    return base.joinpath(*DEFAULT_SCRATCH_PARTS)


def load_recipe(path=None):
    """Load and validate a fixture recipe (default: the checked-in one)."""
    recipe_path = Path(path) if path else RECIPE_PATH
    if not Path(recipe_path).is_file():
        raise SceneRefused(CODE_RECIPE_INVALID, "recipe file is missing",
                           str(recipe_path))
    try:
        with open(str(recipe_path), "r", encoding="utf-8") as stream:
            recipe = json.load(stream)
    except (OSError, ValueError) as error:
        raise SceneRefused(CODE_RECIPE_INVALID, "recipe is not readable JSON",
                           "{0}: {1}".format(recipe_path, error))
    validate_recipe(recipe)
    return recipe


def _key_rows(value, field):
    rows = []
    for row in value:
        if not isinstance(row, (list, tuple)) or len(row) != 2:
            raise SceneRefused(CODE_RECIPE_INVALID, "{0} keys must be [time, value]"
                               .format(field), repr(row))
        try:
            time_value, key_value = float(row[0]), float(row[1])
        except (TypeError, ValueError):
            raise SceneRefused(CODE_RECIPE_INVALID, "{0} keys must be numeric"
                               .format(field), repr(row))
        if not (math.isfinite(time_value) and math.isfinite(key_value)):
            raise SceneRefused(CODE_RECIPE_INVALID, "{0} keys must be finite"
                               .format(field), repr(row))
        rows.append([time_value, key_value])
    if not rows:
        raise SceneRefused(CODE_RECIPE_INVALID, "{0} needs at least one key"
                           .format(field))
    return sorted(rows)


def _three_numbers(value, field, context):
    """An optional three-number vector; ``None`` means the default."""
    if value is None:
        return None
    if not isinstance(value, list) or len(value) != 3:
        raise SceneRefused(CODE_RECIPE_INVALID, "{0} must be three numbers".format(field),
                           context)
    for number in value:
        if isinstance(number, bool) or not isinstance(number, (int, float)) \
                or not math.isfinite(float(number)):
            raise SceneRefused(CODE_RECIPE_INVALID,
                               "{0} must be three finite numbers".format(field), context)
    return [float(number) for number in value]


def validate_recipe(recipe):
    """Structural validation of a fixture recipe. Raises ``SceneRefused``."""
    if not isinstance(recipe, dict):
        raise SceneRefused(CODE_RECIPE_INVALID, "recipe must be an object",
                           repr(type(recipe).__name__))
    try:
        fps = float(recipe["fps"])
    except (KeyError, TypeError, ValueError):
        raise SceneRefused(CODE_RECIPE_INVALID, "recipe needs a numeric fps",
                           repr(recipe.get("fps")))
    if not math.isfinite(fps) or fps <= 0.0:
        raise SceneRefused(CODE_RECIPE_INVALID, "recipe fps must be positive",
                           repr(fps))
    rng = recipe.get("range")
    if not isinstance(rng, (list, tuple)) or len(rng) != 2:
        raise SceneRefused(CODE_RECIPE_INVALID, "recipe needs a [start, end] range",
                           repr(rng))
    start, end = float(rng[0]), float(rng[1])
    if not (math.isfinite(start) and math.isfinite(end)) or end <= start:
        raise SceneRefused(CODE_RECIPE_INVALID, "recipe range must increase",
                           repr(rng))
    rigs = recipe.get("rigs")
    if not isinstance(rigs, dict) or len(rigs) < 2:
        raise SceneRefused(CODE_RECIPE_INVALID,
                           "recipe needs at least two rigs", repr(rigs))
    for rig_id, rig in sorted(rigs.items()):
        if not isinstance(rig, dict):
            raise SceneRefused(CODE_RECIPE_INVALID, "rig must be an object", rig_id)
        namespace = rig.get("namespace")
        if not isinstance(namespace, str) or not namespace.isidentifier():
            raise SceneRefused(CODE_RECIPE_INVALID,
                               "rig namespace must be a Maya identifier",
                               "{0}: {1!r}".format(rig_id, namespace))
        ancestors = rig.get("ancestors") or []
        if not isinstance(ancestors, list):
            raise SceneRefused(CODE_RECIPE_INVALID, "ancestors must be a list", rig_id)
        ancestor_names = []
        for ancestor in ancestors:
            if not isinstance(ancestor, dict) or not isinstance(ancestor.get("name"), str):
                raise SceneRefused(CODE_RECIPE_INVALID, "ancestor needs a name", rig_id)
            ancestor_names.append(ancestor["name"])
            _three_numbers(ancestor.get("rotate"), "ancestor rotate", rig_id)
            for attribute, keys in sorted((ancestor.get("animation") or {}).items()):
                _key_rows(keys, "{0}.{1}.{2}".format(rig_id, ancestor["name"], attribute))
        bones = rig.get("bones")
        if not isinstance(bones, list) or not bones:
            raise SceneRefused(CODE_RECIPE_INVALID, "rig needs bones", rig_id)
        names = []
        for index, bone in enumerate(bones):
            if not isinstance(bone, dict) or not isinstance(bone.get("name"), str):
                raise SceneRefused(CODE_RECIPE_INVALID, "bone needs a name", rig_id)
            parent = bone.get("parent", -1)
            if isinstance(parent, bool) or not isinstance(parent, int) \
                    or parent < -1 or parent >= index:
                raise SceneRefused(
                    CODE_RECIPE_INVALID, "bone parent must index an earlier bone",
                    "{0}[{1}]".format(rig_id, index))
            translate = bone.get("translate") or [0.0, 0.0, 0.0]
            if not isinstance(translate, list) or len(translate) != 3:
                raise SceneRefused(CODE_RECIPE_INVALID,
                                   "bone translate must be three numbers",
                                   "{0}[{1}]".format(rig_id, index))
            _three_numbers(bone.get("rotate"), "bone rotate", "{0}[{1}]".format(rig_id, index))
            names.append(bone["name"])
        if len(set(names)) != len(names):
            raise SceneRefused(CODE_RECIPE_INVALID, "bone names must be unique", rig_id)
        mesh = rig.get("mesh") or {}
        size = mesh.get("size")
        if not isinstance(size, list) or len(size) != 3:
            raise SceneRefused(CODE_RECIPE_INVALID,
                               "rig mesh needs a three-number size", rig_id)
        curves = rig.get("curves")
        if not isinstance(curves, dict) or not curves:
            raise SceneRefused(CODE_RECIPE_INVALID, "rig needs Morph curves", rig_id)
        for curve_name, keys in sorted(curves.items()):
            if not isinstance(curve_name, str) or not curve_name:
                raise SceneRefused(CODE_RECIPE_INVALID, "curve needs a name", rig_id)
            _key_rows(keys, "{0}.{1}".format(rig_id, curve_name))
        animation = rig.get("animation") or {}
        if not isinstance(animation, dict):
            raise SceneRefused(CODE_RECIPE_INVALID, "animation must be an object", rig_id)
        for bone_name, channels in sorted(animation.items()):
            if bone_name not in names:
                raise SceneRefused(
                    CODE_RECIPE_INVALID, "animation targets an unknown bone",
                    "{0}.{1}".format(rig_id, bone_name))
            for channel, keys in sorted(channels.items()):
                _key_rows(keys, "{0}.{1}.{2}".format(rig_id, bone_name, channel))
            if bone_name == names[0] and not any(
                    channel.startswith(("translate", "rotate")) for channel in channels):
                # Keyed root motion is what the prototype demonstrates for the
                # root of every rig; a recipe without any is a mistake, not a
                # variant.
                raise SceneRefused(
                    CODE_RECIPE_INVALID, "rig root needs keyed motion",
                    "{0}.{1}".format(rig_id, bone_name))
    return recipe


# ------------------------------------------------------------- fixture build

def fixture_root_path(namespace, ancestor_names, root_bone_name):
    parts = list(ancestor_names) + [root_bone_name]
    return "|" + "|".join("{0}:{1}".format(namespace, part) for part in parts)


def _apply_keys(node, attribute, keys):
    maya = cmds()
    for time_value, key_value in keys:
        maya.setKeyframe(node, attribute=attribute, time=time_value,
                         value=key_value)
    maya.keyTangent(node, attribute=attribute, inTangentType="linear",
                    outTangentType="linear")


def _build_rig(rig_id, rig, fps, directory):
    maya = cmds()
    namespace = rig["namespace"]
    maya.file(new=True, force=True)
    maya.currentUnit(linear="cm", time="{0}fps".format(_fps_text(fps)))

    ancestors = rig.get("ancestors") or []
    ancestor_names = []
    parent = None
    for ancestor in ancestors:
        node = maya.createNode("transform", name=ancestor["name"])
        translate = ancestor.get("translate") or [0.0, 0.0, 0.0]
        maya.setAttr(node + ".translate", *[float(value) for value in translate])
        rotate = ancestor.get("rotate") or [0.0, 0.0, 0.0]
        maya.setAttr(node + ".rotate", *[float(value) for value in rotate])
        if parent is not None:
            maya.parent(node, parent)
        parent = node
        ancestor_names.append(ancestor["name"])

    bone_nodes = []
    for index, bone in enumerate(rig["bones"]):
        node = maya.createNode("joint", name=bone["name"])
        translate = bone.get("translate") or [0.0, 0.0, 0.0]
        maya.setAttr(node + ".translate", *[float(value) for value in translate])
        # A joint's local rotation is part of the rig, not only its translation:
        # an imported target can rest rotated (a -90 degree skeleton root), and
        # the observation bind has to match it.
        rotate = bone.get("rotate") or [0.0, 0.0, 0.0]
        maya.setAttr(node + ".rotateOrder", 0)  # xyz
        maya.setAttr(node + ".rotate", *[float(value) for value in rotate])
        if index == 0:
            if parent is not None:
                maya.parent(node, parent)
        else:
            maya.parent(node, bone_nodes[bone["parent"]], relative=True)
        bone_nodes.append(node)

    mesh_spec = rig["mesh"]
    size = [float(value) for value in mesh_spec["size"]]
    mesh = maya.polyCube(name=mesh_spec["name"], width=size[0], height=size[1],
                         depth=size[2])[0]
    maya.skinCluster(bone_nodes, mesh, tsb=True)

    blend_shape = None
    for index, curve_name in enumerate(sorted(rig["curves"])):
        keys = _key_rows(rig["curves"][curve_name], "{0}.{1}".format(rig_id, curve_name))
        if blend_shape is None:
            blend_shape = maya.blendShape(mesh, name="{0}Morphs".format(rig_id))[0]
        target = maya.polyCube(
            name="{0}{1}Target".format(rig_id, curve_name),
            width=size[0], height=size[1], depth=size[2])[0]
        maya.setAttr(target + ".visibility", 0)
        maya.blendShape(blend_shape, edit=True, target=(mesh, index, target, 1.0))
        plug = "{0}.weight[{1}]".format(blend_shape, index)
        maya.aliasAttr(curve_name, plug)
        _apply_keys(blend_shape, curve_name, keys)

    for bone_name, channels in sorted((rig.get("animation") or {}).items()):
        node = bone_name
        for attribute, keys in sorted(channels.items()):
            _apply_keys(node, attribute, _key_rows(
                keys, "{0}.{1}.{2}".format(rig_id, bone_name, attribute)))

    for ancestor in ancestors:
        for attribute, keys in sorted((ancestor.get("animation") or {}).items()):
            _apply_keys(ancestor["name"], attribute, _key_rows(
                keys, "{0}.{1}.{2}".format(rig_id, ancestor["name"], attribute)))

    path = Path(directory) / "{0}.ma".format(namespace)
    maya.file(rename=str(path))
    maya.file(save=True, type="mayaAscii")
    root_bone = rig["bones"][0]["name"]
    return {
        "file": str(path),
        "namespace": namespace,
        "root": fixture_root_path(namespace, ancestor_names, root_bone),
        "bones": [bone["name"] for bone in rig["bones"]],
        "parents": [int(bone.get("parent", -1)) for bone in rig["bones"]],
        "curves": sorted(rig["curves"]),
        "mesh": mesh_spec["name"],
        "ancestors": ancestor_names,
        "ancestor_animation": {
            ancestor["name"]: sorted((ancestor.get("animation") or {}))
            for ancestor in ancestors},
    }


def _fps_text(fps):
    value = float(fps)
    if abs(value - round(value)) < 1.0e-9:
        return str(int(round(value)))
    return "{0:g}".format(value)


def _refuse_tracked_directory(target):
    """Generated rigs belong outside tracked source (scratch ``.tmp`` is fine)."""
    try:
        relative = Path(target).resolve().relative_to(REPOSITORY_ROOT)
    except ValueError:
        return
    if ".tmp" not in relative.parts:
        raise SceneRefused(
            CODE_RECIPE_INVALID,
            "generated rig files must not be written into tracked source",
            "{0} is inside the repository; use a .tmp path or a directory "
            "outside it".format(target))


def build_fixtures(recipe=None, directory=None, force=False):
    """Materialise every rig of the recipe and return the fixture manifest.

    The manifest is the checked-in recipe plus resolved file paths, root
    paths and per-rig fingerprints. It is also written to the fixture
    directory as ``fixture.json`` so peers (including the Unreal receiver and
    its automation test) can verify the declared targets without reading this
    module.
    """
    ensure_maya()
    recipe = recipe or load_recipe()
    validate_recipe(recipe)
    target = Path(directory) if directory else scratch_directory()
    target = Path(target)
    _refuse_tracked_directory(target)
    target.mkdir(parents=True, exist_ok=True)
    fps = float(recipe["fps"])
    # An older recipe can have the same named .ma files but different bind
    # matrices. Regenerate the disposable rigs when the recipe changes.
    manifest_path = target / "fixture.json"
    try:
        with open(str(manifest_path), "r", encoding="utf-8") as stream:
            previous_recipe = json.load(stream).get("recipe")
    except (OSError, ValueError, AttributeError):
        previous_recipe = None
    rebuild = force or previous_recipe != recipe
    rigs = {}
    for rig_id in sorted(recipe["rigs"]):
        path = target / "{0}.ma".format(recipe["rigs"][rig_id]["namespace"])
        if rebuild or not path.is_file():
            rigs[rig_id] = _build_rig(rig_id, recipe["rigs"][rig_id], fps, target)
        else:
            rigs[rig_id] = _describe_rig(rig_id, recipe["rigs"][rig_id], target)

    manifest = {
        "name": recipe.get("name"),
        "revision": recipe.get("revision"),
        "fps": fps,
        "range": [float(recipe["range"][0]), float(recipe["range"][1])],
        "directory": str(target),
        "recipe": recipe,
        "rigs": rigs,
        "fingerprints": {
            rig_id: {"namespace": rigs[rig_id]["namespace"],
                     "bones": rigs[rig_id]["bones"],
                     "parents": rigs[rig_id]["parents"],
                     "curves": rigs[rig_id]["curves"]}
            for rig_id in sorted(rigs)},
    }
    with open(str(target / "fixture.json"), "w", encoding="utf-8") as stream:
        json.dump(manifest, stream, indent=2, ensure_ascii=True, sort_keys=False)
        stream.write("\n")
    return manifest


def _describe_rig(rig_id, rig, directory):
    """Manifest entry for an already generated rig (no Maya writes)."""
    namespace = rig["namespace"]
    ancestor_names = [ancestor["name"] for ancestor in (rig.get("ancestors") or [])]
    return {
        "file": str(Path(directory) / "{0}.ma".format(namespace)),
        "namespace": namespace,
        "root": fixture_root_path(namespace, ancestor_names, rig["bones"][0]["name"]),
        "bones": [bone["name"] for bone in rig["bones"]],
        "parents": [int(bone.get("parent", -1)) for bone in rig["bones"]],
        "curves": sorted(rig["curves"]),
        "mesh": rig["mesh"]["name"],
        "ancestors": ancestor_names,
        "ancestor_animation": {
            ancestor["name"]: sorted((ancestor.get("animation") or {}))
            for ancestor in (rig.get("ancestors") or [])},
    }


def load_manifest(directory=None):
    """Read ``fixture.json`` from a fixture directory."""
    target = Path(directory) if directory else scratch_directory()
    path = Path(target) / "fixture.json"
    if not path.is_file():
        raise SceneRefused(CODE_FIXTURE_MISSING, "fixture manifest is missing",
                           str(path))
    with open(str(path), "r", encoding="utf-8") as stream:
        return json.load(stream)


def file_digest(path):
    digest = hashlib.sha256()
    with open(str(path), "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    stat = os.stat(str(path))
    return {"sha256": digest.hexdigest(), "size": stat.st_size,
            "mtime_ns": stat.st_mtime_ns}


def fixture_digests(manifest):
    return {rig_id: dict(file_digest(rig["file"]), file=rig["file"])
            for rig_id, rig in sorted(manifest["rigs"].items())}


# ------------------------------------------------------------ session scenes

class SessionScene(object):
    """One session scene: a fresh scene plus references, or a supplied scene.

    ``open()`` never saves and never edits a referenced file. ``rigs`` names the
    fixture rigs to reference by namespace; the driver owns the subject list, so
    a supplied real scene can pair a loaded root with a generated rig.
    """

    def __init__(self, manifest, rigs=(), scene_file=None):
        self.manifest = manifest
        self.rigs = list(rigs)
        self.scene_file = str(scene_file) if scene_file else None
        self.references = []
        self.opened_clean = None

    def rig(self, rig_id):
        try:
            return self.manifest["rigs"][rig_id]
        except KeyError:
            raise SceneRefused(CODE_FIXTURE_MISSING, "unknown rig id",
                               repr(rig_id))

    def root_for(self, rig_id):
        return self.rig(rig_id)["root"]

    def open(self):
        maya = cmds()
        fps = float(self.manifest["fps"])
        if self.scene_file:
            try:
                maya.file(self.scene_file, open=True, force=True, prompt=False)
            except RuntimeError as error:
                raise SceneRefused(CODE_SCENE_OPEN_FAILED, "scene failed to open",
                                   "{0}: {1}".format(self.scene_file, error))
            self.opened_clean = not bool(maya.file(query=True, modified=True))
            # A supplied scene keeps its own units: the prototype only drives a
            # scene that is already centimetres at the requested frame rate.
            require_scene_units(fps)
        else:
            maya.file(new=True, force=True)
            maya.currentUnit(linear="cm", time="{0}fps".format(_fps_text(fps)))
            self.opened_clean = True
        for rig_id in self.rigs:
            rig = self.rig(rig_id)
            path = rig["file"]
            if not Path(path).is_file():
                raise SceneRefused(CODE_FIXTURE_MISSING, "fixture file is missing",
                                   "{0}: {1}".format(rig_id, path))
            maya.file(path, reference=True, namespace=rig["namespace"],
                      mergeNamespacesOnClash=False)
            self.references.append(rig["namespace"])
        return self

    def reference_records(self):
        """Every Maya reference in the scene with its resolved file."""
        maya = cmds()
        records = []
        for node in sorted(maya.ls(type="reference") or []):
            if node == "sharedReferenceNode":
                continue
            path = maya.referenceQuery(node, filename=True, withoutCopyNumber=True)
            namespace = maya.referenceQuery(node, namespace=True)
            records.append({"node": node, "file": path, "namespace": namespace})
        return records

    def close(self):
        """Discard the session scene without saving it."""
        cmds().file(new=True, force=True)

    def snapshot(self, full=False):
        """Small read-only witness of Maya state, for cleanup evidence."""
        maya = cmds()
        data = {
            "current_time": float(maya.currentTime(query=True)),
            "scene_fps": frames_per_second(maya.currentUnit(query=True, time=True)),
            "linear_unit": maya.currentUnit(query=True, linear=True),
            "modified": bool(maya.file(query=True, modified=True)),
            "script_jobs": len(maya.scriptJob(listJobs=True) or []),
            "expressions": sorted(maya.ls(type="expression") or []),
        }
        if full:
            data["nodes"] = len(maya.ls(dag=True, long=True) or [])
            data["references"] = self.reference_records()
        return data


def frames_per_second(unit):
    return float(product().frames_per_second(unit))


def scene_units():
    """Current scene units as a report: ``{"linear_unit", "time_unit", "fps"}``."""
    maya = cmds()
    time_unit = maya.currentUnit(query=True, time=True)
    return {
        "linear_unit": maya.currentUnit(query=True, linear=True),
        "time_unit": time_unit,
        "fps": frames_per_second(time_unit),
    }


def require_scene_units(fps):
    """The session scene must be centimetres at the negotiated frame rate."""
    maya = cmds()
    linear = maya.currentUnit(query=True, linear=True)
    if linear != "cm":
        raise SceneRefused(
            CODE_UNSUPPORTED_SCENE_LINEAR_UNIT,
            "the prototype only drives centimetre scenes",
            "scene linear unit is {0!r}".format(linear))
    unit = maya.currentUnit(query=True, time=True)
    scene_fps = frames_per_second(unit)
    if abs(scene_fps - float(fps)) > 1.0e-9:
        raise SceneRefused(
            CODE_SCENE_FPS_MISMATCH,
            "scene frame rate must match the negotiated fps",
            "scene {0} ({1}) vs requested {2}".format(unit, scene_fps, fps))
    return scene_fps


def set_evaluated_time(time_value):
    cmds().currentTime(float(time_value), edit=True)
    return float(cmds().currentTime(query=True))


# ------------------------------------------------------------ subject guards

def namespace_of(path):
    leaf = str(path).rsplit("|", 1)[-1]
    return leaf.rsplit(":", 1)[0] if ":" in leaf else ""


def resolve_root(root_string):
    """Resolve one explicit root string to a unique long joint path."""
    maya = cmds()
    if not isinstance(root_string, str) or not root_string:
        raise SceneRefused(CODE_MISSING_ROOT, "root string is empty",
                           repr(root_string))
    matches = maya.ls(root_string, long=True) or []
    if not matches:
        raise SceneRefused(CODE_MISSING_ROOT, "root does not resolve",
                           repr(root_string))
    if len(matches) > 1:
        raise SceneRefused(CODE_AMBIGUOUS_ROOT, "root resolves to several nodes",
                           "{0}: {1}".format(root_string, sorted(matches)))
    path = matches[0]
    if maya.nodeType(path) != "joint":
        raise SceneRefused(CODE_NOT_A_JOINT, "root is not a joint",
                           "{0} is a {1}".format(path, maya.nodeType(path)))
    return path


def ancestor_report(root_path):
    """Ancestors of a root, with the motion that would move it twice.

    The Unreal target applies the root world pose once through its Actor
    anchor. Any animated Maya ancestor (a socket or a group above the root) is
    therefore reported here: it is already inside the world pose, and a second
    Unreal-side socket parent would apply it twice.
    """
    maya = cmds()
    report = []
    current = root_path
    while True:
        parents = maya.listRelatives(current, parent=True, fullPath=True) or []
        if not parents:
            break
        current = parents[0]
        keys = maya.keyframe(current, query=True, keyframeCount=True) or 0
        curves = maya.listConnections(current, type="animCurve") or []
        report.append({
            "path": current,
            "type": maya.nodeType(current),
            "animated": bool(keys) or bool(curves),
            "keyed_channels": int(keys),
        })
    return report


def capture_subject(subject_id, root_string, expectations=None,
                    strict_ancestors=False):
    """Capture one subject through the product's ``_capture_subject``.

    ``expectations`` (optional) is the manifest fingerprint of the role this
    subject claims: ``bones``, ``parents`` and ``curves``. A mismatch is a
    refused role swap rather than a silent stream.
    """
    module = product()
    path = resolve_root(root_string)
    namespace = namespace_of(path)
    ancestors = ancestor_report(path)
    if strict_ancestors:
        moving = [entry["path"] for entry in ancestors if entry["animated"]]
        if moving:
            raise SceneRefused(
                CODE_ANIMATED_ANCESTOR,
                "an ancestor of the root is animated; its motion is already in "
                "the root world pose",
                "; ".join(moving))
    try:
        subject = module._capture_subject(path)
    except Exception as error:  # product refusals carry diagnostics
        code = getattr(error, "code", None) or CODE_PRODUCT_CAPTURE_FAILED
        if code == "NO_VISIBLE_SKINNED_MESH":
            code = CODE_SUBJECT_MESH_MISSING
        elif code == "INCOMPLETE_SKELETON":
            code = CODE_SUBJECT_SKELETON_INCOMPLETE
        raise SceneRefused(code, "product subject capture refused",
                           "{0}: {1}".format(type(error).__name__, error))
    bones = subject["bones"]
    record = {
        "id": subject_id,
        "root": path,
        "namespace": namespace,
        "ancestors": ancestors,
        "subject": subject,
        "wire": {
            "id": subject_id,
            "root": path,
            "bones": [{"name": bone["name"], "parent": int(bone["parent"])}
                      for bone in bones],
            "curves": [curve["name"] for curve in subject["curves"]],
            "bind": [list(row) for row in subject["bind_local_transforms"]],
        },
        "profile": {
            "bones": [bone["name"] for bone in bones],
            "parents": [int(bone["parent"]) for bone in bones],
            "curves": [curve["name"] for curve in subject["curves"]],
            "curve_plugs": [list(curve["plugs"]) for curve in subject["curves"]],
            "meshes": list(subject["meshes"]),
            "unit_scale": float(subject["unit_scale"]),
            "bind_conflict_count": int(subject["bind_conflict_count"]),
        },
    }
    if expectations:
        _check_expectations(record, expectations)
    for curve, plugs in zip(record["profile"]["curves"], record["profile"]["curve_plugs"]):
        for plug in plugs:
            plug_namespace = namespace_of(plug)
            if plug_namespace != namespace:
                raise SceneRefused(
                    CODE_CURVE_CROSS_TALK,
                    "Morph plug does not belong to the subject namespace",
                    "{0}.{1} -> {2}".format(subject_id, curve, plug))
    if len(record["wire"]["bind"]) != len(bones):
        raise SceneRefused(CODE_PRODUCT_CAPTURE_FAILED,
                           "bind rows do not match the published bones",
                           "{0}: {1} vs {2}".format(
                               subject_id, len(record["wire"]["bind"]), len(bones)))
    return record


def _check_expectations(record, expectations):
    expected_namespace = expectations.get("namespace")
    if expected_namespace and record["namespace"] != expected_namespace:
        raise SceneRefused(
            CODE_ROLE_SWAP,
            "subject root belongs to another role's reference",
            "{0} declared {1!r}, root namespace is {2!r}".format(
                record["id"], expected_namespace, record["namespace"]))
    expected_bones = expectations.get("bones")
    if expected_bones is not None and list(record["profile"]["bones"]) != list(expected_bones):
        raise SceneRefused(
            CODE_WRONG_BONES,
            "captured skeleton does not match the declared role",
            "{0}: expected {1}, captured {2}".format(
                record["id"], list(expected_bones), record["profile"]["bones"]))
    expected_parents = expectations.get("parents")
    if expected_parents is not None \
            and [int(value) for value in record["profile"]["parents"]] \
            != [int(value) for value in expected_parents]:
        raise SceneRefused(
            CODE_WRONG_BONES, "captured hierarchy does not match the declared role",
            "{0}: expected parents {1}, captured {2}".format(
                record["id"], list(expected_parents), record["profile"]["parents"]))
    expected_curves = expectations.get("curves")
    if expected_curves is not None \
            and list(record["profile"]["curves"]) != list(expected_curves):
        raise SceneRefused(
            CODE_WRONG_CURVES,
            "captured Morph curves do not match the declared role",
            "{0}: expected {1}, captured {2}".format(
                record["id"], list(expected_curves), record["profile"]["curves"]))


def reject_duplicate_roots(records):
    seen = {}
    for record in records:
        other = seen.get(record["root"])
        if other is not None:
            raise SceneRefused(
                CODE_DUPLICATE_ROOT,
                "two subjects cannot share one root",
                "{0} and {1} both use {2}".format(
                    other, record["id"], record["root"]))
        seen[record["root"]] = record["id"]


def sample_subject(record):
    """Sample one subject at Maya's current time (product ``_sample_pose``)."""
    try:
        transforms, curves = product()._sample_pose(record["subject"])
    except Exception as error:
        raise SceneRefused(
            getattr(error, "code", None) or CODE_SAMPLING_FAILED,
            "pose sampling refused",
            "{0}: {1}".format(type(error).__name__, error))
    return [list(row) for row in transforms], list(curves)


def apply_curve_overrides(records, overrides, forced=None):
    """Set Morph weights on the disposable session scene before sampling.

    The prototype never saves the session scene; this makes a Morph channel
    carry a known value for the pair that is being verified. A curve that the
    subject did not capture, or a plug that cannot be written, is refused
    instead of silently streaming zero.

    ``forced`` (``--force-curve``) writes a plug that the scene drives: it
    unlocks a locked plug, disconnects the incoming connection and records both
    in the evidence, so a Morph value can be demonstrated on a production mesh
    whose channels are driven. The manipulation stays in the disposable scene.
    """
    maya = cmds()
    forced = forced or {}
    applied = []
    for record in records:
        wanted = dict(overrides.get(record["id"]) or {})
        want_forced = dict(forced.get(record["id"]) or {})
        if not wanted and not want_forced:
            continue
        captured = record["profile"]["curves"]
        plugs_by_curve = record["profile"]["curve_plugs"]
        missing = sorted(name for name in list(wanted) + list(want_forced)
                         if name not in captured)
        if missing:
            raise SceneRefused(
                CODE_CURVE_OVERRIDE,
                "the subject did not capture the requested curve",
                "{0}: {1}".format(record["id"], missing))
        for index, curve in enumerate(captured):
            if curve not in wanted and curve not in want_forced:
                continue
            plugs = list(plugs_by_curve[index])
            if not plugs:
                raise SceneRefused(
                    CODE_CURVE_OVERRIDE,
                    "the curve has no writable plug",
                    "{0}.{1}".format(record["id"], curve))
            force = curve in want_forced
            value = float(want_forced[curve] if force else wanted[curve])
            entries = []
            for plug in plugs:
                entry = {"plug": plug}
                if force:
                    if bool(maya.getAttr(plug, lock=True)):
                        maya.setAttr(plug, lock=False)
                        entry["unlocked"] = True
                    sources = maya.listConnections(
                        plug, plugs=True, destination=False, source=True) or []
                    for source in sources:
                        maya.disconnectAttr(source, plug)
                    if sources:
                        entry["disconnected"] = list(sources)
                try:
                    maya.setAttr(plug, value)
                except Exception as error:
                    raise SceneRefused(
                        CODE_CURVE_OVERRIDE,
                        "the Morph plug could not be written",
                        "{0}: {1}".format(plug, error))
                entries.append(entry)
            applied.append({
                "id": record["id"],
                "curve": curve,
                "value": value,
                "forced": force,
                "plugs": entries,
            })
    return applied


def reapply_curve_overrides(applied):
    """Set every overridden plug to its value again, after a scene evaluation.

    A keyed channel is rewritten by the evaluation at every sampled time, so the
    override is applied again for each frame that is streamed; the value the
    peer sends is therefore the value it set, and the evidence shows it.
    """
    maya = cmds()
    for entry in applied:
        for plug in entry["plugs"]:
            maya.setAttr(plug["plug"], entry["value"])
    return applied


def mismatched_subject(record, substitute=None, bone_name="Chest"):
    """Wire copy of a subject declared with another skeleton.

    Used by the renegotiation scenario: the receiver must refuse this init
    (the declared skeleton is not its arms target) and then accept a fresh
    negotiation on the same connection.

    ``substitute`` is the wire of the full-body declaration the subject is
    deliberately mis-declared with: its bones, parents and bind rows replace
    the subject's own, while id, root and Morph curves stay the subject's
    (the curves describe the subject, the skeleton is what is being
    mis-declared). Without a substitute the root bone is renamed to
    ``bone_name``, which must not collide with the declared skeleton —
    otherwise the init would be structurally invalid rather than a target
    mismatch.
    """
    wire = {
        "id": record["wire"]["id"],
        "root": record["wire"]["root"],
        "bones": [dict(bone) for bone in record["wire"]["bones"]],
        "curves": list(record["wire"]["curves"]),
        "bind": [list(row) for row in record["wire"]["bind"]],
    }
    if substitute is not None:
        wire["bones"] = [dict(bone) for bone in substitute["bones"]]
        wire["bind"] = [list(row) for row in substitute["bind"]]
        return wire
    existing = set(bone["name"] for bone in wire["bones"])
    candidate = bone_name
    while candidate in existing:
        candidate += "_FullBody"
    wire["bones"][0]["name"] = candidate
    return wire
