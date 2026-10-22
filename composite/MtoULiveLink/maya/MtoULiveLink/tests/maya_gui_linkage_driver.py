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
``morph_range``, default ``[0.15, 0.85]``) and ``allow_scene_load_warnings``.

Commands (``{"id": n, "action": ...}``): ``enter``, ``connect``, ``disconnect``,
``leave`` (with ``target`` ``realtime``/``end``), ``workflow`` (``target``
``animation``/``model``), ``mode`` (``target`` ``realtime``/``cached``),
``capture`` (``range`` ``[start, end]``), ``time`` (``frame``), ``snapshot``,
``panel_shot`` (with ``name``), ``stage`` (with ``name``: every dialog is
recorded with the latest stage, so the test can tell which ones a scenario
expects), ``quit``. Each runs the handler of the panel
control it names. The result file always carries the latest linkage status
and the panel's workflow, mode, cache state and status line; ``ack`` names
the last finished command.
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

RESULT = {"phase": "starting", "ok": True, "ack": -1, "dialogs": [], "errors": [],
          "events": [], "shots": [], "maya_version": cmds.about(version=True),
          "pid": os.getpid()}


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


def joint_path(short):
    joints = cmds.listRelatives(FIXTURE["root"], allDescendents=True, type="joint",
                                fullPath=True) or []
    for path in joints:
        if path.split("|")[-1] == short:
            return path
    return None


def key_test_motion():
    """Key a slow alternating rotation directly on the listed deform joints.

    Production rigs drive deform joints through constraint chains whose
    controls are themselves driven, so the joint's own rotate input is
    detached and keyed instead. Only this disposable, unsaved process changes.
    """
    start, end = FIXTURE.get("key_range", [0, 2500])
    step = float(FIXTURE.get("key_step", 45))
    degrees = float(FIXTURE.get("key_degrees", 25))
    keyed = []
    for index, short in enumerate(FIXTURE.get("keyed_joints", [])):
        path = joint_path(short)
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
    step = float(FIXTURE.get("key_step", 45))
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


def snapshot():
    record = camera_record()
    joints = {}
    for short in FIXTURE.get("oracle_joints", []):
        path = joint_path(short)
        if not path:
            continue
        world = cmds.xform(path, query=True, worldSpace=True, translation=True)
        joints[short] = {"world": world,
                         "screen": screen_point(record, world) if record else None}
    morphs = {}
    for name in RESULT.get("keyed_morphs", []):
        plugs = morph_plugs(name)
        if plugs:
            morphs[name] = cmds.getAttr(plugs[0])
    status = CONTROLLER.linkage_status()
    return {"time": cmds.currentTime(query=True), "camera": record, "joints": joints,
            "morphs": morphs, "status": status}


def grab_windows(name):
    """Save the main panel and the 镜头联动 window as PNG images."""
    from maya import OpenMayaUI as omui
    try:
        from PySide2.QtWidgets import QWidget
        from shiboken2 import wrapInstance
    except ImportError:  # Maya 2025+
        from PySide6.QtWidgets import QWidget
        from shiboken6 import wrapInstance
    saved = []
    for window in ("MtoULiveLinkWindow", "MtoULiveLinkLinkWindow"):
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
    elif action == "snapshot":
        RESULT["snapshot"] = snapshot()
    elif action == "panel_shot":
        CONTROLLER.open_link_window()
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
    elif action == "quit":
        quit_maya()
        return
    else:
        RESULT["errors"].append("unknown command " + str(action))


def tick():
    try:
        status = CONTROLLER.linkage_status()
        RESULT["status"] = status
        RESULT["connected"] = CONTROLLER._session is not None and CONTROLLER._session_ready()
        RESULT["maya_time"] = cmds.currentTime(query=True)
        RESULT["link_camera_exists"] = bool(cmds.objExists("MtoU_LinkCamera"))
        RESULT["panel_camera"] = (camera_record() or {}).get("camera")
        view = CONTROLLER._cached_view
        RESULT["panel"] = {
            "workflow": CONTROLLER._workflow, "mode": CONTROLLER._mode,
            "cached_state": getattr(view, "state", None) if view else None,
            "status": cmds.text(CONTROLLER._status_text, query=True, label=True)
            if CONTROLLER._status_text else None}
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
        import MtoULiveLink
        CONTROLLER = MtoULiveLink.run()
        RESULT["phase"] = "panel"
        write_result()
        cmds.select(FIXTURE["root"], replace=True)
        CONTROLLER.set_role()
        CONTROLLER.connect()
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
