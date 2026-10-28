"""Maya character scene capture, bind pose and evaluated sampling."""

import math
import re

from . import mayaapi
from .config import BIND_MATRIX_TOLERANCE, CURVE_VALUE_TOLERANCE
from .diagnostics import DIAGNOSTICS, make_diagnostic
from .mayaapi import cmds, om, oma
from .protocol import make_frame_message
from .space import centimeters_per_unit, convert_transform


def normalize_name(path):
    return path.rsplit("|", 1)[-1].rsplit(":", 1)[-1]


class DuplicateBoneNamesError(ValueError):
    def __init__(self, duplicate_paths):
        self.duplicate_paths = duplicate_paths
        self.paths = [path for name in sorted(duplicate_paths)
                      for path in duplicate_paths[name]]
        names = ", ".join(sorted(duplicate_paths))
        super(DuplicateBoneNamesError, self).__init__(
            "duplicate normalized bone names: {0}".format(names))


def duplicate_bone_paths(records):
    paths_by_name = {}
    for record in records:
        paths_by_name.setdefault(record["name"], []).append(record["path"])
    return {
        name: paths for name, paths in paths_by_name.items() if len(paths) > 1
    }


def build_hierarchy(root, children, allow_duplicates=False):
    records = []
    stack = [(root, -1)]
    while stack:
        path, parent = stack.pop()
        index = len(records)
        records.append({"path": path, "name": normalize_name(path), "parent": parent})
        child_paths = list(children(path) or [])
        stack.extend((child, index) for child in reversed(child_paths))
    duplicates = duplicate_bone_paths(records)
    if duplicates and not allow_duplicates:
        raise DuplicateBoneNamesError(duplicates)
    return records


def duplicate_bone_diagnostic(error):
    lines = ["Duplicate normalized bone names:"]
    for name in sorted(error.duplicate_paths):
        lines.append("{0}:".format(name))
        lines.extend("  " + path for path in error.duplicate_paths[name])
    diagnostic = make_diagnostic(
        "DUPLICATE_BONE_NAMES", str(error), details="\n".join(lines))
    diagnostic["duplicate_paths"] = list(error.paths)
    return diagnostic


def require_maya():
    if cmds is None or om is None:
        raise RuntimeError("MtoU_LiveLink must be run inside Maya 2022.4")


def dag_path(path):
    selection = om.MSelectionList()
    selection.add(path)
    return selection.getDagPath(0)


def transform_leads_to_joint(path):
    return bool(cmds.listRelatives(
        path, allDescendents=True, fullPath=True, type="joint"))


def maya_children(path):
    children = cmds.listRelatives(path, children=True, fullPath=True) or []
    result = []
    for child in children:
        try:
            node_type = cmds.nodeType(child)
        except (RuntimeError, ValueError):
            continue
        if node_type == "joint":
            result.append(child)
        elif node_type == "transform":
            if transform_leads_to_joint(child):
                result.append(child)
        # Other DAG types (mesh shapes, constraints, controllers) never
        # publish; joints beneath them are reported by
        # ensure_complete_capture instead of being silently pruned.
    return result


def describe_incomplete_joint(root, joint_path, published):
    current = joint_path
    while current != root:
        parents = cmds.listRelatives(current, parent=True, fullPath=True) or []
        if not parents:
            return ("joint {0} has no parent chain reaching selected root {1}".format(
                joint_path, root))
        parent = parents[0]
        if parent not in published:
            try:
                parent_type = cmds.nodeType(parent)
            except (RuntimeError, ValueError):
                parent_type = "unknown"
            if parent_type not in ("joint", "transform"):
                return ("joint {0} passes through {1} ({2}); "
                        "only joint/transform intermediates can be published".format(
                    joint_path, parent, parent_type))
            return ("joint {0} via unpublished {1} ({2}) was not captured".format(
                joint_path, parent, parent_type))
        current = parent
    return "joint {0} was not captured".format(joint_path)


def ensure_complete_capture(root, bones):
    published = set(bone["path"] for bone in bones)
    joints = cmds.listRelatives(
        root, allDescendents=True, fullPath=True, type="joint") or []
    for joint_path in sorted([root] + list(joints)):
        if joint_path in published:
            continue
        reason = describe_incomplete_joint(root, joint_path, published)
        message = "skeleton under {0} is missing joint {1}".format(root, joint_path)
        details = "{0}; {1}".format(message, reason)
        raise CharacterSceneError(
            "INCOMPLETE_SKELETON", message, details=details,
            context={"root": root, "missing_joint": joint_path})


def selected_root():
    selected = cmds.ls(selection=True, long=True) or []
    if len(selected) != 1 or cmds.nodeType(selected[0]) != "joint":
        raise ValueError("Select exactly one deformation root joint before connecting.")
    return selected[0]


def long_transform(node):
    matches = cmds.ls(node, long=True) or []
    if len(matches) != 1:
        raise ValueError("Expected exactly one Maya node: {0}".format(node))
    path = matches[0]
    if cmds.nodeType(path) in ("mesh", "nurbsCurve"):
        parents = cmds.listRelatives(path, parent=True, fullPath=True) or []
        if not parents:
            raise ValueError("The selected shape has no transform parent: {0}".format(path))
        path = parents[0]
    if cmds.nodeType(path) != "transform":
        raise ValueError("The Display controller must be a transform or curve shape.")
    return path


def namespace(path):
    leaf = path.rsplit("|", 1)[-1]
    return leaf.rsplit(":", 1)[0] if ":" in leaf else ""


def top_level(path):
    parts = [part for part in path.split("|") if part]
    return "|" + parts[0] if parts else path


def enum_attributes(node):
    candidates = []
    for attribute in cmds.listAttr(node, userDefined=True) or []:
        plug = node + "." + attribute
        if cmds.getAttr(plug, type=True) != "enum":
            continue
        definitions = cmds.attributeQuery(attribute, node=node, listEnum=True) or []
        entries = definitions[0].split(":") if definitions else []
        if any(re.match(r"(?i)^clothes", entry or "") for entry in entries):
            candidates.append({"attribute": attribute, "entries": entries, "plug": plug})
    return candidates


def display_candidates(root):
    top = top_level(root)
    descendants = cmds.listRelatives(top, allDescendents=True, fullPath=True,
                                     type="transform") or []
    expected_namespace = namespace(root)
    candidates = []
    for node in descendants:
        leaf = node.rsplit("|", 1)[-1]
        if normalize_name(leaf).lower() != "display_ctrl":
            continue
        if namespace(node) == expected_namespace:
            candidates.append(node)
    return sorted(candidates)


def current_enum_label(display):
    value = int(cmds.getAttr(display["plug"]))
    entries = display["entries"]
    return entries[value] if 0 <= value < len(entries) else str(value)


def effective_mesh_transform(geometry):
    paths = cmds.ls(geometry, long=True) or []
    if not paths:
        return None
    path = paths[0]
    if cmds.nodeType(path) == "mesh":
        shape = path
        parents = cmds.listRelatives(shape, parent=True, fullPath=True) or []
        if not parents:
            return None
        transform = parents[0]
    else:
        transform = path
        shapes = cmds.listRelatives(transform, shapes=True, noIntermediate=True,
                                    fullPath=True, type="mesh") or []
        if not shapes:
            return None
        shape = shapes[0]
    if cmds.getAttr(shape + ".intermediateObject"):
        return None
    if not (cmds.ls(shape, visible=True, long=True) or []):
        return None
    if not cmds.getAttr(shape + ".visibility"):
        return None
    current = transform
    while current:
        if cmds.attributeQuery("visibility", node=current, exists=True) \
                and not cmds.getAttr(current + ".visibility"):
            return None
        if cmds.attributeQuery("lodVisibility", node=current, exists=True) \
                and not cmds.getAttr(current + ".lodVisibility"):
            return None
        if cmds.attributeQuery("overrideEnabled", node=current, exists=True) \
                and cmds.getAttr(current + ".overrideEnabled"):
            if cmds.attributeQuery("overrideVisibility", node=current, exists=True) \
                    and not cmds.getAttr(current + ".overrideVisibility"):
                return None
            if cmds.attributeQuery("overrideDisplayType", node=current, exists=True) \
                    and cmds.getAttr(current + ".overrideDisplayType") != 0:
                return None
        parents = cmds.listRelatives(current, parent=True, fullPath=True) or []
        current = parents[0] if parents else None
    return transform


def visible_skinned_meshes(bone_paths):
    skin_clusters = set()
    for path in bone_paths:
        skin_clusters.update(cmds.listConnections(path, type="skinCluster") or [])
    meshes = []
    seen = set()
    for skin_cluster in sorted(skin_clusters):
        for geometry in cmds.skinCluster(skin_cluster, query=True, geometry=True) or []:
            transform = effective_mesh_transform(geometry)
            if transform and transform not in seen:
                seen.add(transform)
                meshes.append(transform)
    return sorted(meshes)


def skin_clusters_for_meshes(mesh_paths):
    skin_clusters = set()
    for mesh_path in mesh_paths:
        skin_clusters.update(
            node for node in cmds.listHistory(mesh_path, pruneDagObjects=True) or []
            if cmds.nodeType(node) == "skinCluster")
    return sorted(skin_clusters)


def matrix_attr(plug):
    value = cmds.getAttr(plug)
    if isinstance(value, (list, tuple)) and len(value) == 1:
        value = value[0]
    return om.MMatrix(value)


def matrix_is_finite(matrix):
    return all(math.isfinite(float(value)) for value in matrix)


def matrix_is_equivalent(left, right, tolerance=BIND_MATRIX_TOLERANCE):
    return all(abs(float(a) - float(b)) <= tolerance for a, b in zip(left, right))


class BindPoseError(ValueError):
    pass


class BindMatrixCandidate(object):
    SKIN_CLUSTER = "skinCluster"
    DAG_POSE = "dagPose"

    def __init__(self, kind, source, matrix, influence_count=0):
        self.kind = kind
        self.source = source
        self.matrix = matrix
        self.influence_count = int(influence_count)


def matrix_inverse(matrix, bone_path, source):
    if not matrix_is_finite(matrix):
        raise BindPoseError(
            "bone {0} has non-finite bind matrix from {1}".format(
                bone_path, source))
    determinant = float(matrix.det4x4())
    if not math.isfinite(determinant) or determinant == 0.0:
        raise BindPoseError(
            "bone {0} has non-invertible bind matrix from {1}".format(
                bone_path, source))
    inverse = matrix.inverse()
    if not matrix_is_finite(inverse):
        raise BindPoseError(
            "bone {0} has non-finite inverse bind matrix from {1}".format(
                bone_path, source))
    return inverse


def highest_priority_bind_candidates(records):
    dag_pose_records = [
        record for record in records
        if record.kind == BindMatrixCandidate.DAG_POSE]
    if dag_pose_records:
        return dag_pose_records
    largest_influence_count = max(record.influence_count for record in records)
    return [record for record in records
            if record.influence_count == largest_influence_count]


def select_bind_world_candidate(bone_path, records):
    preferred = highest_priority_bind_candidates(records)
    selected = preferred[0]
    if any(not matrix_is_equivalent(selected.matrix, record.matrix)
           for record in preferred[1:]):
        sources = ", ".join(sorted(record.source for record in preferred))
        raise BindPoseError(
            "bone {0} has conflicting equally ranked bind matrices: {1}".format(
                bone_path, sources))
    conflict = any(not matrix_is_equivalent(selected.matrix, record.matrix)
                   for record in records)
    return selected.matrix, conflict


def bind_world_candidates(subject):
    candidates = dict((bone["path"], []) for bone in subject["bones"])
    bone_paths = set(candidates)
    for skin_cluster in skin_clusters_for_meshes(subject["meshes"]):
        selection = om.MSelectionList()
        selection.add(skin_cluster)
        function = oma.MFnSkinCluster(selection.getDependNode(0))
        influences = function.influenceObjects()
        for influence in influences:
            path = influence.fullPathName()
            if path not in bone_paths:
                continue
            logical_index = function.indexForInfluenceObject(influence)
            plug = "{0}.bindPreMatrix[{1}]".format(skin_cluster, logical_index)
            candidates[path].append(BindMatrixCandidate(
                BindMatrixCandidate.SKIN_CLUSTER,
                plug,
                matrix_inverse(matrix_attr(plug), path, plug),
                influence_count=len(influences)))

    for bone in subject["bones"]:
        if cmds.nodeType(bone["path"]) != "joint":
            # Structural transforms have no joint bindPose/dagPose; when they
            # are not skin influences they fall back to the capture-time
            # local re-anchored to the parent bind frame in
            # capture_bind_local_transforms.
            continue
        plug = bone["path"] + ".bindPose"
        for pose_plug in cmds.listConnections(
                plug, source=False, destination=True, plugs=True) or []:
            pose = pose_plug.split(".", 1)[0]
            if cmds.nodeType(pose) == "dagPose" and cmds.getAttr(pose + ".bindPose"):
                # The joint-connected dagPose is the pose Go to Bind Pose
                # restores and the most likely Skeletal Mesh export pose.
                matrix = matrix_attr(pose_plug)
                matrix_inverse(matrix, bone["path"], pose_plug)
                candidates[bone["path"]].append(BindMatrixCandidate(
                    BindMatrixCandidate.DAG_POSE, pose_plug, matrix))
    return candidates


def capture_bind_local_transforms(subject):
    candidates = bind_world_candidates(subject)
    current_world = [bone["dag_path"].inclusiveMatrix() for bone in subject["bones"]]
    bind_world = []
    conflicts = 0
    for index, bone in enumerate(subject["bones"]):
        records = candidates[bone["path"]]
        if records:
            # Skin clusters can legitimately disagree when outfits were bound
            # at different poses; the joint-connected dagPose wins, otherwise
            # the skin cluster with the most influences. Equally ranked
            # candidates must agree so node naming cannot choose the pose.
            matrix, conflict = select_bind_world_candidate(bone["path"], records)
            conflicts += int(conflict)
        elif bone["parent"] >= 0:
            # Bones with no stored bind data (for example corrective slider
            # joints added after binding) use their capture-time local offset
            # re-anchored to the parent's bind frame.
            parent = bone["parent"]
            parent_inverse = matrix_inverse(
                current_world[parent], bone["path"],
                "capture-time parent matrix")
            matrix = current_world[index] * parent_inverse \
                * bind_world[parent]
        else:
            matrix = current_world[index]
        bind_world.append(matrix)

    bind_world_inverse = [
        matrix_inverse(matrix, bone["path"], "resolved bind world matrix")
        for bone, matrix in zip(subject["bones"], bind_world)]
    bind_local = []
    for index, bone in enumerate(subject["bones"]):
        matrix = bind_world[index]
        if bone["parent"] >= 0:
            matrix = matrix * bind_world_inverse[bone["parent"]]
        bind_local.append(sample_matrix(matrix, subject["unit_scale"]))
    return bind_local, conflicts


def discover_curve_plugs(bone_paths, visible_meshes=None):
    if visible_meshes is None:
        visible_meshes = visible_skinned_meshes(bone_paths)
    deformers = set()
    for geometry in visible_meshes:
        deformers.update(node for node in cmds.listHistory(geometry, pruneDagObjects=True) or []
                         if cmds.nodeType(node) == "blendShape")
    curves = []
    for deformer in sorted(deformers):
        aliases = cmds.aliasAttr(deformer, query=True) or []
        for alias, attribute in zip(aliases[0::2], aliases[1::2]):
            match = re.match(r"(?:weight|w)\[(\d+)\]$", attribute)
            if match:
                curves.append({"name": alias, "plug": deformer + "." + attribute,
                               "sort": (deformer, int(match.group(1)))})
    curves.sort(key=lambda curve: curve["sort"])
    grouped = []
    by_name = {}
    for curve in curves:
        group = by_name.get(curve["name"])
        if group is None:
            group = {"name": curve["name"], "plugs": [], "sort": curve["sort"]}
            by_name[curve["name"]] = group
            grouped.append(group)
        group["plugs"].append(curve["plug"])
    return grouped


def capture_subject(root=None):
    require_maya()
    root = root or selected_root()
    bones = build_hierarchy(root, maya_children, allow_duplicates=True)
    ensure_complete_capture(root, bones)
    for bone in bones:
        bone["dag_path"] = dag_path(bone["path"])
    mesh_paths = visible_skinned_meshes([bone["path"] for bone in bones])
    if not mesh_paths:
        raise ValueError("NO_VISIBLE_SKINNED_MESH")
    subject = {"root": root, "bones": bones,
               "meshes": mesh_paths,
               "curves": discover_curve_plugs(
                   [bone["path"] for bone in bones], mesh_paths),
               "unit_scale": centimeters_per_unit(
                   cmds.currentUnit(query=True, linear=True))}
    subject["bind_local_transforms"], subject["bind_conflict_count"] = \
        capture_bind_local_transforms(subject)
    return subject


def sample_matrix(matrix, unit_scale):
    transform = om.MTransformationMatrix(matrix)
    translation = transform.translation(om.MSpace.kTransform)
    rotation = transform.rotation(asQuaternion=True)
    scale = transform.scale(om.MSpace.kTransform)
    return convert_transform(translation, rotation, scale, unit_scale)


def sample_pose(subject):
    world_matrices = [bone["dag_path"].inclusiveMatrix() for bone in subject["bones"]]
    transforms = []
    for index, bone in enumerate(subject["bones"]):
        matrix = world_matrices[index]
        if bone["parent"] >= 0:
            matrix = matrix * world_matrices[bone["parent"]].inverse()
        transforms.append(sample_matrix(matrix, subject["unit_scale"]))
    curves = []
    for curve in subject["curves"]:
        values = [float(cmds.getAttr(plug)) for plug in curve["plugs"]]
        reference = values[0]
        if any(abs(value - reference) > CURVE_VALUE_TOLERANCE
               for value in values[1:]):
            details = ", ".join("{0}={1}".format(plug, value)
                                for plug, value in zip(curve["plugs"], values))
            raise ValueError(
                "conflicting values for BlendShape alias {0}: {1}".format(
                    curve["name"], details))
        curves.append(reference)
    make_frame_message(transforms, curves)
    return transforms, curves


class CharacterSceneError(RuntimeError):
    def __init__(self, code, message="", details="", context=None,
                 duplicate_paths=None):
        self._code = code
        self._message = message or code
        self._details = details or self._message
        self._context = dict(context or {})
        self._duplicate_paths = tuple(duplicate_paths or ())
        super(CharacterSceneError, self).__init__(self._message)

    @property
    def code(self):
        return self._code

    @property
    def message(self):
        return self._message

    @property
    def details(self):
        return self._details

    @property
    def context(self):
        return dict(self._context)

    @property
    def duplicate_paths(self):
        return self._duplicate_paths


class CharacterSnapshot(object):
    def __init__(self, revision, root, outfit, bones, curve_names,
                 duplicate_paths=(), mesh_paths=(), bind_local_transforms=(),
                 bind_conflict_count=0):
        self._revision = int(revision)
        self._root = root
        self._outfit = outfit
        self._bones = tuple((name, int(parent)) for name, parent in bones)
        self._curve_names = tuple(curve_names)
        self._duplicate_paths = tuple(duplicate_paths)
        self._mesh_paths = tuple(mesh_paths)
        self._bind_conflict_count = int(bind_conflict_count)
        self._bind_local_transforms = tuple(
            tuple(value for value in transform) for transform in bind_local_transforms)
        if len(self._bind_local_transforms) != len(self._bones):
            raise ValueError("bind pose count must match skeleton bone count")

    @property
    def revision(self):
        return self._revision

    @property
    def root(self):
        return self._root

    @property
    def outfit(self):
        return self._outfit

    @property
    def bones(self):
        return self._bones

    @property
    def curve_names(self):
        return self._curve_names

    @property
    def duplicate_paths(self):
        return self._duplicate_paths

    @property
    def mesh_paths(self):
        return self._mesh_paths

    @property
    def bind_local_transforms(self):
        return self._bind_local_transforms

    @property
    def bind_conflict_count(self):
        return self._bind_conflict_count


class CharacterFrame(object):
    def __init__(self, revision, transforms, curves):
        self._revision = int(revision)
        self._transforms = tuple(tuple(value for value in transform)
                                 for transform in transforms)
        self._curves = tuple(curves)

    @property
    def revision(self):
        return self._revision

    @property
    def transforms(self):
        return self._transforms

    @property
    def curves(self):
        return self._curves


class CharacterSceneEvent(object):
    def __init__(self, kind, snapshot=None, error=None):
        self._kind = kind
        self._snapshot = snapshot
        self._error = error

    @property
    def kind(self):
        return self._kind

    @property
    def snapshot(self):
        return self._snapshot

    @property
    def error(self):
        return self._error


class CharacterScene(object):
    """Owns one configured, sampleable Maya character scene."""

    def __init__(self, root, display, display_attribute, on_event):
        self._root = root
        self._display_path = display
        self._display_attribute = display_attribute
        self._on_event = on_event
        self._subject = None
        self._snapshot = None
        self._callback_ids = []
        self._refresh_generation = 0
        self._refreshing = False
        self._closed = False
        self._terminal_error = None
        self._cleanup_error = None

    @classmethod
    def capture(cls, root, display=None, display_attribute=None, on_event=None,
                display_optional=False):
        """Capture one object scene.

        ``display_optional`` is for the independent object beside the primary
        character: a skeletal prop has no Display controller, so a root
        without one captures without an outfit instead of being refused.
        """
        require_maya()
        scene = None
        try:
            root = cls._resolve_root(root)
            display, display_attribute = cls._resolve_display(
                root, display, display_attribute, display_optional)
            scene = cls(root, display, display_attribute, on_event)
            scene._commit_capture(revision=1)
            scene._register_callbacks()
            return scene
        except CharacterSceneError:
            if scene is not None:
                scene.close()
            raise
        except (RuntimeError, ValueError) as exc:
            if scene is not None:
                scene.close()
            raise cls._wrap_error("ROLE_SETUP_FAILED", exc)

    @staticmethod
    def _resolve_root(root):
        matches = cmds.ls(root, long=True) or []
        if len(matches) != 1 or cmds.nodeType(matches[0]) != "joint":
            raise CharacterSceneError(
                "INVALID_CHARACTER_ROOT",
                "Select exactly one deformation root joint before connecting.")
        return matches[0]

    @staticmethod
    def _resolve_display(root, display, display_attribute, display_optional=False):
        if display is None:
            candidates = display_candidates(root)
            if not candidates and display_optional:
                return None, None
            if len(candidates) != 1:
                raise CharacterSceneError(
                    "AMBIGUOUS_DISPLAY",
                    "Unable to identify exactly one Display controller.",
                    context={"display_candidates": tuple(candidates)})
            display = candidates[0]
        else:
            try:
                display = long_transform(display)
            except (RuntimeError, ValueError) as exc:
                raise CharacterScene._wrap_error("INVALID_DISPLAY", exc)
        role_top = top_level(root)
        if display != role_top and not display.startswith(role_top + "|"):
            raise CharacterSceneError(
                "INVALID_DISPLAY",
                "The Display controller must be inside the character's top-level group.")
        if namespace(display) != namespace(root):
            raise CharacterSceneError(
                "INVALID_DISPLAY",
                "The Display controller and character root must use the same namespace.")
        attributes = enum_attributes(display)
        by_name = dict((item["attribute"], item) for item in attributes)
        if display_attribute is None:
            if len(attributes) != 1:
                raise CharacterSceneError(
                    "AMBIGUOUS_DISPLAY_ATTRIBUTE",
                    "Unable to identify exactly one Clothes enum attribute.",
                    context={"attribute_candidates": tuple(sorted(by_name))})
            display_attribute = attributes[0]["attribute"]
        if display_attribute not in by_name:
            raise CharacterSceneError(
                "INVALID_DISPLAY_ATTRIBUTE",
                "The selected Display attribute is not a Clothes enum.",
                context={"attribute_candidates": tuple(sorted(by_name))})
        return display, display_attribute

    @staticmethod
    def _wrap_error(code, error):
        if isinstance(error, CharacterSceneError):
            return error
        return CharacterSceneError(code, str(error), details=str(error))

    def _display_record(self):
        for record in enum_attributes(self._display_path):
            if record["attribute"] == self._display_attribute:
                return record
        raise CharacterSceneError(
            "INVALID_DISPLAY_ATTRIBUTE",
            "The configured Clothes enum attribute is no longer available.")

    def _capture_values(self, revision):
        try:
            subject = capture_subject(self._root)
            outfit = ""
            if self._display_path is not None:
                outfit = current_enum_label(self._display_record())
        except BindPoseError as exc:
            raise self._wrap_error("BIND_POSE_INVALID", exc)
        except (RuntimeError, ValueError) as exc:
            code = str(exc) if str(exc) in DIAGNOSTICS else "ROLE_SETUP_FAILED"
            raise self._wrap_error(code, exc)
        duplicates = duplicate_bone_paths(subject["bones"])
        duplicate_paths = [path for name in sorted(duplicates)
                           for path in duplicates[name]]
        snapshot = CharacterSnapshot(
            revision,
            self._root,
            outfit,
            [(bone["name"], bone["parent"]) for bone in subject["bones"]],
            [curve["name"] for curve in subject["curves"]],
            duplicate_paths,
            subject["meshes"],
            subject["bind_local_transforms"],
            subject.get("bind_conflict_count", 0))
        return subject, snapshot

    def _commit_capture(self, revision):
        subject, snapshot = self._capture_values(revision)
        self._subject = subject
        self._snapshot = snapshot

    def _register_callbacks(self):
        try:
            self._callback_ids.append(om.MNodeMessage.addNodeDestroyedCallback(
                self._subject["bones"][0]["dag_path"].node(),
                self._on_root_destroyed))
            if self._display_path is None:
                return
            self._callback_ids.append(om.MNodeMessage.addNodeDestroyedCallback(
                dag_path(self._display_path).node(), self._on_display_destroyed))
            self._callback_ids.append(om.MNodeMessage.addAttributeChangedCallback(
                dag_path(self._display_path).node(), self._on_display_changed))
        except (RuntimeError, ValueError) as exc:
            self._remove_callbacks()
            raise self._wrap_error("ROLE_SETUP_FAILED", exc)

    def snapshot(self):
        self._ensure_available()
        return self._snapshot

    def sample(self):
        self._ensure_available()
        revision = self._snapshot.revision
        subject = self._subject
        try:
            transforms, curves = sample_pose(subject)
        except (RuntimeError, ValueError) as exc:
            raise self._wrap_error("SAMPLING_FAILED", exc)
        return CharacterFrame(revision, transforms, curves)

    def close(self):
        if self._closed:
            return
        self._closed = True
        self._refresh_generation += 1
        self._refreshing = False
        self._remove_callbacks()
        self._subject = None
        if self._terminal_error is None:
            self._terminal_error = CharacterSceneError(
                "CHARACTER_SCENE_CLOSED", "The character scene is closed.")

    def _ensure_available(self):
        if self._terminal_error is not None:
            raise self._terminal_error
        if self._closed:
            raise CharacterSceneError(
                "CHARACTER_SCENE_CLOSED", "The character scene is closed.")
        if self._refreshing:
            raise CharacterSceneError(
                "CHARACTER_SCENE_REFRESHING", "The character scene is refreshing.")

    def _remove_callbacks(self):
        callback_ids, self._callback_ids = self._callback_ids, []
        for callback_id in callback_ids:
            try:
                om.MMessage.removeCallback(callback_id)
            except RuntimeError as exc:
                if self._cleanup_error is None:
                    self._cleanup_error = exc

    def _emit(self, event):
        if self._on_event is None:
            return
        try:
            self._on_event(event)
        except Exception as exc:
            if cmds is not None:
                try:
                    cmds.warning(
                        "MtoU_LiveLink character event callback failed: {0}".format(exc))
                except Exception:
                    pass

    def _on_display_changed(self, message, plug, other_plug, client_data):
        del other_plug, client_data
        if self._closed or self._terminal_error is not None:
            return
        if not (message & om.MNodeMessage.kAttributeSet):
            return
        if plug.partialName(useLongNames=True) != self._display_attribute:
            return
        try:
            if current_enum_label(self._display_record()) == self._snapshot.outfit:
                return
        except CharacterSceneError as error:
            self._invalidate(error)
            return
        if not self._refreshing:
            self._refreshing = True
            self._emit(CharacterSceneEvent(
                "character_change_started", snapshot=self._snapshot))
        self._refresh_generation += 1
        generation = self._refresh_generation
        mayaapi.run_deferred(lambda: self._refresh_after_display_change(generation),
                             cmds.evalDeferred)

    def _refresh_after_display_change(self, generation):
        if self._closed or generation != self._refresh_generation:
            return
        try:
            subject, candidate = self._capture_values(self._snapshot.revision + 1)
        except CharacterSceneError as error:
            self._invalidate(error)
            return
        previous = self._snapshot
        changed = (
            candidate.outfit != previous.outfit
            or candidate.bones != previous.bones
            or candidate.curve_names != previous.curve_names
            or candidate.duplicate_paths != previous.duplicate_paths)
        if not changed:
            candidate = CharacterSnapshot(
                previous.revision, candidate.root, candidate.outfit,
                candidate.bones, candidate.curve_names, candidate.duplicate_paths,
                candidate.mesh_paths, candidate.bind_local_transforms,
                candidate.bind_conflict_count)
        self._subject = subject
        self._snapshot = candidate
        self._refreshing = False
        self._emit(CharacterSceneEvent("outfit_changed", snapshot=candidate))

    def _on_root_destroyed(self, *unused):
        self._invalidate(CharacterSceneError(
            "CHARACTER_ROOT_DESTROYED", "The character root was deleted or unloaded."))

    def _on_display_destroyed(self, *unused):
        self._invalidate(CharacterSceneError(
            "DISPLAY_DESTROYED", "The Display controller was deleted or unloaded."))

    def _invalidate(self, error):
        if self._closed or self._terminal_error is not None:
            return
        self._terminal_error = error
        self._closed = True
        self._refresh_generation += 1
        self._refreshing = False
        self._remove_callbacks()
        self._subject = None
        self._emit(CharacterSceneEvent("character_invalidated", error=error))
