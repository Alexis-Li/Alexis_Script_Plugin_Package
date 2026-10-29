# ruff: noqa: E402
"""Opt-in Maya GUI driver for the Unreal GUI acceptance tests.

Runs inside an interactive Maya session started by the Unreal test:

    maya.exe -command "python(\"import runpy; runpy.run_path(r'<this file>')\")"

with ``MTOU_LINKAGE_DRIVER_FIXTURE`` naming the fixture. The driver opens the
supplied rig without saving it, keys disposable test motion on the controls
that drive the listed joints, starts the tool through its public ``run()``
entry, sets the role and connects through the panel's own controller actions,
and then follows the commands Unreal writes to the command file. Everything
runs on Maya's main thread from a Qt timer, so the panel's own timers,
scriptJobs and deferred callbacks run exactly as they do for an artist.

Fixture keys: ``scene``, ``root``, ``scripts`` (the tool's ``scripts``
directory), ``command``, ``result``, ``evidence``, ``oracle_joints`` (short
names), ``keyed_joints`` (short names whose orient-constraint driver gets test
keys), ``key_range`` ``[start, end]``, ``key_step``, ``key_degrees``,
optional ``keyed_morphs`` (blendShape aliases keyed between the two weights of
``morph_range``, default ``[0.15, 0.85]``), ``morph_namespace`` (report every
weight of the blendShapes in that namespace whose value is at least 0.05, so a
scene's own animation supplies the Morph oracle without test keys) and
``allow_scene_load_warnings`` and ``start_frame`` (Maya's time before the
first connect). Joint names match without namespaces.

Optional ``secondary`` sets the second object before the first connect, from
the same selection the 对象配对 window reads: ``root``, optional ``display``,
``oracle_joints``, ``keyed_joints`` and ``morph_namespace`` with the meanings
above. Snapshots then carry ``objects`` with ``character`` and ``secondary``
rows of ``joints`` and ``morphs``.

Commands (``{"id": n, "action": ...}``): ``enter``, ``connect``, ``disconnect``,
``leave`` (with ``target`` ``realtime``/``end``), ``workflow`` (``target``
``animation``/``model``), ``mode`` (``target`` ``realtime``/``cached``),
``capture`` (``range`` ``[start, end]``), ``time`` (``frame``), ``snapshot``,
``panel_shot`` (with ``name``), ``stage`` (with ``name``: every dialog is
recorded with the latest stage, so the test can tell which ones a scenario
expects), ``sample_plugins`` (with ``name``), ``set_secondary``,
``remove_secondary`` (the 对象配对 window's **设为副对象** and **移除副对象**),
``secondary_is_primary`` (**设为副对象** with the main character's root
selected), ``objects_window`` (``target`` ``open``/``close``),
``drop_secondary_once`` (the next linkage group leaves out the second object;
this disposable process only), ``import_reference`` (the 场景参考 window's
**导入/更新参考**), ``reference_snapshot`` (the world bounding-box centre of
every transform under the reference container whose name holds ``name``),
``resources`` (the census in ``resources``: live OpenMaya callbacks counted at
their registration, the tool's scriptJobs, Python threads, open sockets,
``MtoU_*`` nodes and MtoU windows), ``close_panel`` (the panel window closes),
``open_panel`` (``run()`` again, then set the role and connect),
``new_scene``, ``reopen_scene`` (the fixture scene again, then connect),
``lock_link_camera`` (``target`` ``unlock`` unlocks and deletes it), ``quit``. Each runs the handler
of the panel control it names. The result file always carries the latest
linkage status, the object set and the panel's workflow, mode, cache state and
status line; ``ack`` names the last finished command.

``prefs`` describes the preferences this Maya started with, read before Maya
can save them again: the ``pluginPrefs.mel`` in this Maya's own preference
folder (``internalVar(userPrefDir=True)``), that file's SHA-256 and the
plug-ins it loads automatically. ``plugin_samples`` keeps, under each
``sample_plugins`` name, the plug-ins loaded when that command ran, its stage
and the seconds since the driver started; a later command never replaces it.
``plugins`` lists the plug-ins loaded at the latest ``stage`` or ``snapshot``.
"""

import json
import math
import os
import sys
import time
import traceback
from pathlib import Path

import maya.cmds as cmds
import maya.utils

FIXTURE = json.loads(Path(os.environ["MTOU_LINKAGE_DRIVER_FIXTURE"]).read_text(
    encoding="utf-8-sig"))
SCRIPTS = Path(FIXTURE["scripts"])
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))
TESTS = str(Path(__file__).resolve().parent)
if TESTS not in sys.path:
    sys.path.insert(0, TESTS)

import maya_prefs_template

RESULT = {"phase": "starting", "ok": True, "ack": -1, "dialogs": [], "errors": [],
          "events": [], "shots": [], "maya_version": cmds.about(version=True),
          "pid": os.getpid(),
          "prefs": maya_prefs_template.describe(cmds.internalVar(userPrefDir=True),
                                                os.environ.get("MAYA_APP_DIR", ""))}
STARTED = time.time()


def write_result():
    data = json.dumps(RESULT, ensure_ascii=False, default=str)
    path = Path(FIXTURE["result"])
    for attempt in range(50):
        try:
            path.write_text(data, encoding="utf-8")
            return
        except PermissionError:
            if attempt == 49:
                raise
            time.sleep(0.02)


def fail(message):
    RESULT["ok"] = False
    RESULT["errors"].append(message)
    write_result()


# A modal dialog would stop the event loop the whole test runs on. Every
# dialog is recorded and answered with its default button instead; only this
# disposable Maya process is affected.
_original_confirm = cmds.confirmDialog


def _recorded_confirm(*args, **kwargs):
    diagnostic = getattr(CONTROLLER, "_last_diagnostic", None) if CONTROLLER else None
    RESULT["dialogs"].append({"title": kwargs.get("title"), "message": kwargs.get("message"),
                              "diagnostic": diagnostic, "stage": RESULT.get("stage", "startup")})
    write_result()
    return kwargs.get("defaultButton") or (kwargs.get("button") or ["OK"])[0]


cmds.confirmDialog = _recorded_confirm


def open_scene():
    try:
        cmds.file(FIXTURE["scene"], open=True, force=True, prompt=False,
                  executeScriptNodes=False, ignoreVersion=True)
    except RuntimeError as exc:
        if not FIXTURE.get("allow_scene_load_warnings") or not cmds.objExists(FIXTURE["root"]):
            raise
        RESULT["scene_load_warning"] = str(exc)[:400]


def joint_path(short, root=None):
    joints = cmds.listRelatives(root or FIXTURE["root"], allDescendents=True, type="joint",
                                fullPath=True) or []
    for path in joints:
        if path.split("|")[-1].split(":")[-1] == short:
            return path
    return None


def object_specs():
    """``(object_id, root, oracle joints, morph namespace)`` of every fixture object."""
    specs = [("character", FIXTURE["root"], FIXTURE.get("oracle_joints", []),
              FIXTURE.get("morph_namespace"))]
    secondary = FIXTURE.get("secondary")
    if secondary:
        specs.append(("secondary", secondary["root"], secondary.get("oracle_joints", []),
                      secondary.get("morph_namespace")))
    return specs


def key_test_motion():
    """Key a slow alternating rotation directly on the listed deform joints.

    Production rigs drive deform joints through constraint chains whose
    controls are themselves driven, so the joint's own rotate input is
    detached and keyed instead. Only this disposable, unsaved process changes.

    The keys change joint rotations only. In a rig whose joints are each
    point-constrained to their controls, such as the C01 production rig, no
    joint position moves, so the oracle's world and screen positions are the
    same at every oracle frame and cannot show which frame a pose came from.
    The oracle joints' world rotations and the keyed Morph weights do differ
    between oracle frames, and the oracle compares both. The keys alternate
    every ``key_step`` frames, so the oracle frames' poses must differ: their
    Maya times must not lie a whole number of ``2 * key_step`` apart, nor
    symmetrically about a key. Those conditions are necessary, not sufficient;
    the Unreal test fails when any two oracle frames' poses are too close.
    """
    start, end = FIXTURE.get("key_range", [0, 2500])
    step = float(FIXTURE.get("key_step", 42))
    degrees = float(FIXTURE.get("key_degrees", 25))
    keyed = []
    listed = [(FIXTURE["root"], short) for short in FIXTURE.get("keyed_joints", [])]
    secondary = FIXTURE.get("secondary") or {}
    listed += [(secondary["root"], short) for short in secondary.get("keyed_joints", [])]
    for index, (root, short) in enumerate(listed):
        path = joint_path(short, root)
        if not path:
            RESULT["errors"].append("no joint named " + short)
            continue
        axis = "XYZ"[index % 3]
        for channel in ("rotate", "rotateX", "rotateY", "rotateZ"):
            plug = "{0}.{1}".format(path, channel)
            for source in cmds.listConnections(plug, source=True, destination=False,
                                               plugs=True) or []:
                cmds.disconnectAttr(source, plug)
        plug = "{0}.rotate{1}".format(path, axis)
        cmds.setAttr(plug, lock=False)
        base = cmds.getAttr(plug)
        frame = float(start)
        sign = 1.0
        while frame <= float(end):
            cmds.setKeyframe(plug, time=frame, value=base + sign * degrees)
            sign = -sign
            frame += step
        keyed.append(short + "." + "rotate" + axis)
    RESULT["keyed"] = keyed


def morph_plugs(name):
    """Every blendShape weight plug whose alias is ``name``."""
    plugs = []
    for deformer in cmds.ls(type="blendShape") or []:
        aliases = cmds.aliasAttr(deformer, query=True) or []
        for alias, attribute in zip(aliases[0::2], aliases[1::2]):
            if alias == name:
                plugs.append(deformer + "." + attribute)
    return plugs


def key_test_morphs():
    """Key every listed Morph between two non-zero weights.

    The rig drives its blendShape weights, so each weight's input is detached
    in this disposable process and keyed instead; the weights alternate out of
    phase so no oracle frame sees them all at one value.
    """
    start, end = FIXTURE.get("key_range", [0, 2500])
    step = float(FIXTURE.get("key_step", 42))
    low, high = FIXTURE.get("morph_range", [0.15, 0.85])
    keyed = []
    for index, name in enumerate(FIXTURE.get("keyed_morphs", [])):
        plugs = morph_plugs(name)
        if not plugs:
            RESULT["errors"].append("no blendShape weight named " + name)
            continue
        for plug in plugs:
            for source in cmds.listConnections(plug, source=True, destination=False,
                                               plugs=True) or []:
                cmds.disconnectAttr(source, plug)
            frame = float(start) + index * step / 3.0
            high_next = index % 2 == 0
            while frame <= float(end):
                cmds.setKeyframe(plug, time=frame, value=high if high_next else low)
                high_next = not high_next
                frame += step
        keyed.append(name)
    RESULT["keyed_morphs"] = keyed


def camera_record():
    """The camera Maya's model panels look through, with the values that frame it."""
    panel = None
    for candidate in cmds.getPanel(type="modelPanel") or []:
        if cmds.modelPanel(candidate, query=True, exists=True):
            panel = candidate
            break
    camera = cmds.modelPanel(panel, query=True, camera=True) if panel else None
    if not camera:
        return None
    shapes = cmds.listRelatives(camera, shapes=True, type="camera", fullPath=True)
    shape = shapes[0] if shapes else camera
    matrix = cmds.xform(camera, query=True, worldSpace=True, matrix=True)
    return {
        "camera": camera, "panel": panel, "matrix": matrix,
        "focal_mm": cmds.getAttr(shape + ".focalLength"),
        "hfa_mm": cmds.getAttr(shape + ".horizontalFilmAperture") * 25.4,
        "vfa_mm": cmds.getAttr(shape + ".verticalFilmAperture") * 25.4,
        "hfo_mm": cmds.getAttr(shape + ".horizontalFilmOffset") * 25.4,
        "vfo_mm": cmds.getAttr(shape + ".verticalFilmOffset") * 25.4,
        "film_fit": cmds.getAttr(shape + ".filmFit"),
        "squeeze": cmds.getAttr(shape + ".lensSqueezeRatio"),
        "resolution": [cmds.getAttr("defaultResolution.width"),
                       cmds.getAttr("defaultResolution.height"),
                       cmds.getAttr("defaultResolution.pixelAspect")],
    }


def screen_point(record, world):
    """Normalized resolution-gate coordinates of one Maya world point (cm).

    Maya cameras look down -Z with +Y up. The film-fit rules follow the Maya
    documentation for the resolution gate against the film gate.
    """
    m = record["matrix"]
    origin = (m[12], m[13], m[14])
    axes = [(m[0], m[1], m[2]), (m[4], m[5], m[6]), (m[8], m[9], m[10])]
    rel = [world[i] - origin[i] for i in range(3)]

    def dot(a, b):
        return sum(a[i] * b[i] for i in range(3))

    def unit(v):
        length = math.sqrt(dot(v, v))
        return [c / length for c in v]

    x = dot(rel, unit(axes[0]))
    y = dot(rel, unit(axes[1]))
    depth = -dot(rel, unit(axes[2]))
    if depth <= 1e-6:
        return None
    film_x = record["focal_mm"] * x / depth + record["hfo_mm"]
    film_y = record["focal_mm"] * y / depth + record["vfo_mm"]
    width, height, pixel_aspect = record["resolution"]
    aspect = float(width) * float(pixel_aspect) / float(height)
    hfa, vfa = record["hfa_mm"] * record["squeeze"], record["vfa_mm"]
    film_aspect = hfa / vfa
    fit = record["film_fit"]
    if fit == 1 or (fit == 0 and aspect > film_aspect) or (fit == 3 and aspect <= film_aspect):
        gate_w, gate_h = hfa, hfa / aspect
    else:
        gate_w, gate_h = vfa * aspect, vfa
    return [film_x / (gate_w / 2.0), film_y / (gate_h / 2.0)]


def world_axes(matrix):
    """The world X, Y and Z axes of a row-major world matrix as unit vectors, flattened."""
    axes = []
    for row in range(3):
        axis = matrix[row * 4:row * 4 + 3]
        length = math.sqrt(sum(c * c for c in axis))
        axes.extend(c / length for c in axis)
    return axes


def snapshot():
    record = camera_record()
    # The joints and weights are evaluated at the current time itself, so the
    # oracle does not depend on whether Maya re-evaluated the scene after the
    # time change: xform and a plain getAttr return the last evaluated values.
    now = cmds.currentTime(query=True)
    objects = {}
    for object_id, root, oracle_joints, namespace in object_specs():
        joints = {}
        for short in oracle_joints:
            path = joint_path(short, root)
            if not path:
                continue
            matrix = cmds.getAttr(path + ".worldMatrix[0]", time=now)
            world = list(matrix[12:15])
            joints[short] = {"world": world,
                             "screen": screen_point(record, world) if record else None,
                             "axes": world_axes(matrix)}
        morphs = {}
        if object_id == "character":
            for name in RESULT.get("keyed_morphs", []):
                plugs = morph_plugs(name)
                if plugs:
                    morphs[name] = cmds.getAttr(plugs[0], time=now)
        if namespace:
            morphs.update(animated_morphs(namespace, now))
        objects[object_id] = {"joints": joints, "morphs": morphs}
    status = CONTROLLER.linkage_status()
    return {"time": now, "camera": record, "joints": objects["character"]["joints"],
            "morphs": objects["character"]["morphs"], "objects": objects, "status": status}


def animated_morphs(namespace, now):
    """Every weight of the namespace's blendShapes at ``now`` that is at least 0.05."""
    weights = {}
    for deformer in cmds.ls(namespace + ":*", type="blendShape") or []:
        aliases = cmds.aliasAttr(deformer, query=True) or []
        for alias in aliases[0::2]:
            value = cmds.getAttr(deformer + "." + alias, time=now)
            if abs(value) >= 0.05:
                weights[alias] = value
    return weights


def grab_windows(name):
    """Save the main panel, the 诊断详情 and the open 对象配对 window as PNG images."""
    from maya import OpenMayaUI as omui
    try:
        from PySide2.QtWidgets import QWidget
        from shiboken2 import wrapInstance
    except ImportError:  # Maya 2025+
        from PySide6.QtWidgets import QWidget
        from shiboken6 import wrapInstance
    saved = []
    for window in ("MtoULiveLinkWindow", "MtoULiveLinkDiagnosticWindow",
                   "MtoULiveLinkObjectsWindow"):
        pointer = omui.MQtUtil.findWindow(window)
        if not pointer:
            continue
        widget = wrapInstance(int(pointer), QWidget)
        path = Path(FIXTURE["evidence"]) / "{0}-{1}.png".format(name, window)
        widget.grab().save(str(path))
        saved.append({"window": window, "path": str(path),
                      "size": [widget.width(), widget.height()]})
    RESULT["shots"].extend(saved)
    return saved


CONTROLLER = None
LAST_ID = [-1]
TIMER = [None]


def record_plugins():
    RESULT["plugins"] = {"stage": RESULT.get("stage", "startup"),
                         "loaded": sorted(cmds.pluginInfo(query=True, listPlugins=True) or [])}


# ------------------------------------------------------------ resource census
# INT-09: every Maya callback the tool registers is counted where it reaches
# OpenMaya. The census wraps the add and remove entry points of the message
# classes before the tool imports them, so it sees the registrations
# themselves rather than the tool's own bookkeeping.
LIVE_CALLBACKS = {}


class _CountingMessages(object):
    def __init__(self, real, family):
        self._real = real
        self._family = family

    def __getattr__(self, name):
        value = getattr(self._real, name)
        if name.startswith("add") and callable(value):
            def add(*args, **kwargs):
                callback_id = value(*args, **kwargs)
                LIVE_CALLBACKS[callback_id] = self._family
                return callback_id
            return add
        if name in ("removeCallback", "removeCallbacks") and callable(value):
            def remove(ids, *args, **kwargs):
                result = value(ids, *args, **kwargs)
                for callback_id in (ids if name == "removeCallbacks" else [ids]):
                    LIVE_CALLBACKS.pop(callback_id, None)
                return result
            return remove
        return value


class _CountingOpenMaya(object):
    FAMILIES = ("MSceneMessage", "MTimerMessage", "MEventMessage", "MNodeMessage",
                "MDGMessage", "MMessage")

    def __init__(self, real):
        self._real = real

    def __getattr__(self, name):
        value = getattr(self._real, name)
        if name in self.FAMILIES:
            return _CountingMessages(value, name)
        return value


def install_callback_census():
    from mtou_livelink import mayaapi
    if not isinstance(mayaapi.om, _CountingOpenMaya):
        mayaapi.om = _CountingOpenMaya(mayaapi.om)


def census():
    """What the tool holds in this Maya right now."""
    import gc
    import socket
    import threading
    families = {}
    for family in LIVE_CALLBACKS.values():
        families[family] = families.get(family, 0) + 1
    jobs = [job for job in cmds.scriptJob(listJobs=True) or []
            if "mtou" in job.lower() or "Controller" in job]
    sockets = 0
    for item in gc.get_objects():
        try:
            if isinstance(item, socket.socket) and item.fileno() != -1:
                sockets += 1
        except Exception:
            continue
    windows = [name for name in ("MtoULiveLinkWindow", "MtoULiveLinkReferenceWindow",
                                 "MtoULiveLinkObjectsWindow")
               if cmds.window(name, exists=True)]
    return {"callbacks": len(LIVE_CALLBACKS), "callback_families": families,
            "script_jobs": len(jobs),
            "threads": sorted(thread.name for thread in threading.enumerate()
                              if thread is not threading.main_thread()),
            "sockets": sockets,
            "nodes": sorted(cmds.ls("MtoU_*") or []),
            "windows": windows,
            "session_open": CONTROLLER is not None and CONTROLLER._session is not None}


def scene_state():
    """What linkage captures and must put back: time, range, gate, cameras, undo."""
    from mtou_livelink.camera import RESOLUTION_PLUGS, model_panels, panel_camera
    return {
        "time": cmds.currentTime(query=True),
        "range": [cmds.playbackOptions(query=True, **{flag: True})
                  for flag in ("minTime", "maxTime", "animationStartTime",
                               "animationEndTime")],
        "gate": dict((plug, cmds.getAttr(plug)) for plug in RESOLUTION_PLUGS),
        "panels": dict((panel, panel_camera(panel)) for panel in model_panels()),
        "undo": bool(cmds.undoInfo(query=True, state=True)),
        "link_camera": bool(cmds.objExists("MtoU_LinkCamera")),
    }


def open_panel():
    """The public entry again, then the role and the connection."""
    global CONTROLLER
    import MtoULiveLink
    CONTROLLER = MtoULiveLink.run()
    connect_role()


def reopen_scene():
    """Open the fixture scene again, as an artist does, and connect."""
    open_scene()
    # The panel's SceneOpened scriptJob runs on the idle queue; Maya runs it
    # before a person can click 设置角色, so the driver does too.
    cmds.flushIdleQueue()
    if "start_frame" in FIXTURE:
        cmds.currentTime(float(FIXTURE["start_frame"]), update=True)
    connect_role()


def connect_role():
    cmds.select(FIXTURE["root"], replace=True)
    CONTROLLER.set_role()
    if FIXTURE.get("secondary"):
        select_secondary()
        CONTROLLER.set_secondary_object()
    CONTROLLER.connect()


def run_command(command):
    action = command.get("action")
    if action == "enter":
        CONTROLLER.enter_linkage()
    elif action == "connect":
        CONTROLLER.connect()
    elif action == "leave":
        CONTROLLER.leave_linkage(command.get("target", "realtime"))
    elif action == "stage":
        # Names the scenario the following dialogs belong to.
        RESULT["stage"] = command.get("name") or "startup"
        record_plugins()
    elif action == "snapshot":
        RESULT["snapshot"] = snapshot()
        record_plugins()
    elif action == "sample_plugins":
        maya_prefs_template.record_plugin_sample(
            RESULT, command.get("name") or "sample", RESULT.get("stage", "startup"),
            cmds.pluginInfo(query=True, listPlugins=True) or [], time.time() - STARTED)
    elif action == "panel_shot":
        CONTROLLER.show_diagnostics()
        grab_windows(command.get("name", "panel"))
    elif action == "time":
        cmds.currentTime(float(command["frame"]), update=True)
    elif action == "disconnect":
        CONTROLLER.disconnect()
    elif action == "workflow":
        # The 动画／模型 tab buttons' own handler.
        CONTROLLER._on_workflow_changed(command["target"])
    elif action == "mode":
        # The 实时预览／缓存播放 buttons' own handler.
        from mtou_livelink import controller as panel
        CONTROLLER._on_mode_changed(
            panel.CACHED_MODE if command["target"] == "cached" else panel.REALTIME_MODE)
    elif action == "capture":
        # 自定义捕获范围 and its fields, then 捕获并上传.
        start, end = command["range"]
        cmds.checkBox(CONTROLLER._custom_range_checkbox, edit=True, value=True)
        cmds.intField(CONTROLLER._range_start_field, edit=True, value=int(start))
        cmds.intField(CONTROLLER._range_end_field, edit=True, value=int(end))
        CONTROLLER._capture_cached_playback()
    elif action == "set_secondary":
        select_secondary()
        CONTROLLER.set_secondary_object()
    elif action == "secondary_is_primary":
        cmds.select(FIXTURE["root"], replace=True)
        CONTROLLER.set_secondary_object()
    elif action == "remove_secondary":
        CONTROLLER.remove_secondary_object()
    elif action == "objects_window":
        if command.get("target") == "close":
            CONTROLLER.close_objects_window()
        else:
            CONTROLLER.open_objects_window()
    elif action == "drop_secondary_once":
        drop_secondary_once()
    elif action == "import_reference":
        CONTROLLER.import_reference()
    elif action == "reference_snapshot":
        RESULT["reference_nodes"] = reference_nodes(command.get("name") or "")
    elif action == "resources":
        RESULT["resources"] = census()
    elif action == "scene_state":
        RESULT["scene_state"] = scene_state()
    elif action == "close_panel":
        # The panel window's own close, as its title bar does.
        cmds.deleteUI("MtoULiveLinkWindow", window=True)
    elif action == "open_panel":
        open_panel()
    elif action == "new_scene":
        cmds.file(new=True, force=True)
    elif action == "reopen_scene":
        reopen_scene()
    elif action == "lock_link_camera":
        # A restore that cannot delete the tool's camera: its residual must be
        # reported, and the person removes the node.
        locking = command.get("target") != "unlock"
        cmds.lockNode("MtoU_LinkCamera", lock=locking)
        if not locking:
            cmds.delete("MtoU_LinkCamera")
    elif action == "quit":
        quit_maya()
        return
    else:
        RESULT["errors"].append("unknown command " + str(action))


def select_secondary():
    secondary = FIXTURE["secondary"]
    selection = [secondary["root"]]
    if secondary.get("display"):
        selection.append(secondary["display"])
    cmds.select(selection, replace=True)


def drop_secondary_once():
    """The next linkage group leaves out the second object's row.

    Patches the group builder the linkage session imported, in this disposable
    process only, so Unreal receives one incomplete group through the real
    connection; every later group is complete again.
    """
    from mtou_livelink import linkage
    original = linkage.make_group_message

    def incomplete(*args, **kwargs):
        linkage.make_group_message = original
        message = original(*args, **kwargs)
        message["objects"] = [row for row in message["objects"]
                              if row.get("object_id") != "secondary"]
        RESULT["dropped_group"] = {"eval_id": message.get("eval_id"),
                                   "objects": [row.get("object_id") for row in message["objects"]]}
        return message

    linkage.make_group_message = incomplete


def reference_nodes(fragment):
    """World bounding-box centres (cm) of the reference container's matching transforms."""
    nodes = {}
    # The importer reports the group it placed, inside its own namespace.
    report = CONTROLLER.reference_report().get("report") or {}
    group = (report.get("container") or {}).get("group_path")
    if not group or not cmds.objExists(group):
        return nodes
    for path in cmds.listRelatives(group, allDescendents=True, type="transform",
                                   fullPath=True) or []:
        name = path.split("|")[-1]
        if fragment in name and cmds.listRelatives(path, shapes=True, type="mesh"):
            box = cmds.exactWorldBoundingBox(path)
            nodes[name] = [(box[0] + box[3]) / 2.0, (box[1] + box[4]) / 2.0,
                           (box[2] + box[5]) / 2.0]
    return nodes


def tick():
    try:
        status = CONTROLLER.linkage_status()
        RESULT["status"] = status
        RESULT["objects"] = CONTROLLER.objects_state()
        # Unreal's last applied evidence, one component row per object.
        applied = getattr(CONTROLLER._linkage, "_applied", None)
        RESULT["applied"] = dict(applied) if applied else None
        RESULT["reference"] = {"offered": bool(CONTROLLER.reference_offer()),
                               "report": CONTROLLER.reference_report()}
        RESULT["connected"] = CONTROLLER._session is not None and CONTROLLER._session_ready()
        # 连接 does nothing while an ending session is still being torn down.
        RESULT["session_open"] = CONTROLLER._session is not None
        RESULT["maya_time"] = cmds.currentTime(query=True)
        RESULT["link_camera_exists"] = bool(cmds.objExists("MtoU_LinkCamera"))
        RESULT["panel_camera"] = (camera_record() or {}).get("camera")
        view = CONTROLLER._cached_view
        RESULT["panel"] = {
            "workflow": CONTROLLER._workflow, "mode": CONTROLLER._mode,
            "cached_state": getattr(view, "state", None) if view else None,
            # The panel's controls are gone while it is closed (close_panel).
            "status": cmds.text(CONTROLLER._status_text, query=True, label=True)
            if CONTROLLER._status_text and cmds.text(CONTROLLER._status_text, exists=True)
            else None}
        try:
            command = json.loads(Path(FIXTURE["command"]).read_text(encoding="utf-8-sig"))
        except (OSError, ValueError):
            command = {}
        if command.get("id", -1) > LAST_ID[0]:
            LAST_ID[0] = command["id"]
            run_command(command)
            RESULT["ack"] = command["id"]
        write_result()
    except Exception:
        RESULT["errors"].append(traceback.format_exc()[-1500:])
        write_result()


def quit_maya():
    TIMER[0].stop()
    RESULT["phase"] = "quitting"
    try:
        CONTROLLER.close()
    except Exception:
        RESULT["errors"].append("close: " + traceback.format_exc()[-800:])
    # The supplied rig is never saved; the keyed copy only lived in memory.
    cmds.file(modified=False)
    RESULT["phase"] = "quit"
    write_result()
    cmds.quit(force=True)


def start():
    global CONTROLLER
    try:
        RESULT["phase"] = "loading"
        write_result()
        load_start = time.time()
        open_scene()
        RESULT["load_seconds"] = round(time.time() - load_start, 1)
        key_test_motion()
        key_test_morphs()
        install_callback_census()
        import MtoULiveLink
        CONTROLLER = MtoULiveLink.run()
        RESULT["phase"] = "panel"
        write_result()
        if "start_frame" in FIXTURE:
            # Where the artist starts: linkage maps every target from here.
            cmds.currentTime(float(FIXTURE["start_frame"]), update=True)
        connect_role()
        RESULT["phase"] = "running"
        write_result()
        try:
            from PySide2.QtCore import QTimer
        except ImportError:
            from PySide6.QtCore import QTimer
        TIMER[0] = QTimer()
        TIMER[0].timeout.connect(tick)
        TIMER[0].start(50)
    except Exception:
        fail(traceback.format_exc()[-2000:])


maya.utils.executeDeferred(start)
