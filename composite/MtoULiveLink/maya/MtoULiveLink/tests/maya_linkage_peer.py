# ruff: noqa: E402
"""Opt-in linkage peer for the Unreal linkage acceptance test.

Runs in `mayapy` with a disposable synthetic character, connects to the Unreal
listener from the fixture, enters the linkage mode, follows the camera/time
targets Unreal evaluates, answers with complete pose groups, records the actual
application evidence, then returns to realtime and verifies that the scene was
restored. It never opens or saves a production scene.

    <mayapy> maya_linkage_peer.py --fixture fixture.json --result result.json

Fixture keys: `port`, optional `object_id` (default `character`), `targets`
(number of applied groups to wait for), `timeout_seconds`, `fps`, optional
`delay_seconds` (pause before the first answer), `synthetic_bones`.
Result keys: `ok`, `phase`, `targets`, `groups`, `applied`, `converged`,
`restore`, `errors`, `group_bytes`, `time_before`, `time_after`, `camera_after`.
"""

import argparse
import importlib.util
import json
import sys
import time
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPTS = HERE.parent / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

from mtou_livelink import config as mtou_config  # noqa: E402
from mtou_livelink import link_host as mtou_link_host  # noqa: E402
from mtou_livelink import linkage as mtou_linkage  # noqa: E402
from mtou_livelink import scene as mtou_scene  # noqa: E402
from mtou_livelink import session as mtou_session  # noqa: E402
from mtou_livelink import space as mtou_space  # noqa: E402


def read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        return None


def write_json(path, value):
    data = json.dumps(value, ensure_ascii=True)
    for attempt in range(50):
        try:
            Path(path).write_text(data, encoding="utf-8")
            return
        except PermissionError:
            if attempt == 49:
                raise
            time.sleep(0.02)


def create_character(cmds, bones=None):
    """Disposable character: one chain, one skinned mesh, one BlendShape."""
    bones = bones or [
        ["root", -1, [0, 0, 0, 0, 0, 0, 1, 1, 1, 1]],
        ["spine", 0, [0, 5, 0, 0, 0, 0, 1, 1, 1, 1]],
        ["tip", 1, [0, 5, 0, 0, 0, 0, 1, 1, 1, 1]],
    ]
    cmds.file(new=True, force=True)
    cmds.currentUnit(linear="cm", time="ntsc")
    group = cmds.createNode("transform", name="Group")
    motion = cmds.createNode("transform", name="MotionSystem", parent=group)
    add = cmds.createNode("transform", name="Add_Ctrl_grp", parent=motion)
    display_group = cmds.createNode("transform", name="Display_ctrl_grp", parent=add)
    display = cmds.createNode("transform", name="Display_ctrl", parent=display_group)
    cmds.addAttr(display, longName="clothes", attributeType="enum",
                 enumName="Clothes01")
    import maya.api.OpenMaya as om
    joints = []
    for name, parent, transform in bones:
        joint = cmds.createNode(
            "joint", name=name, parent=group if parent < 0 else joints[parent])
        matrix = om.MTransformationMatrix()
        matrix.setTranslation(
            om.MVector(transform[0], transform[2], transform[1]), om.MSpace.kTransform)
        matrix.setRotation(
            om.MQuaternion(-transform[3], -transform[5], -transform[4], transform[6]))
        matrix.setScale((transform[7], transform[9], transform[8]),
                        om.MSpace.kTransform)
        cmds.xform(joint, objectSpace=True, matrix=list(matrix.asMatrix()))
        joints.append(joint)
    mesh = cmds.polyCube(name="SM_Clothes01")[0]
    cmds.parent(mesh, group)
    target = cmds.duplicate(mesh, name="ShapeTarget")[0]
    cmds.move(0.2, 0, 0, target + ".vtx[*]", relative=True)
    blend = cmds.blendShape(target, mesh)[0]
    cmds.aliasAttr("Smile", blend + ".weight[0]")
    cmds.delete(target)
    # The Unreal fixture decides the skeleton, so skin whatever chain it gave
    # us instead of assuming a fixed joint count.
    deformers = joints[1:] or joints
    cmds.skinCluster(*(deformers + [mesh]), toSelectedBones=True)
    tip = bones[-1][0]
    cmds.setKeyframe(tip + ".translateY", time=1, value=0)
    cmds.setKeyframe(tip + ".translateY", time=24, value=8)
    # The alias names the plug; keying goes through the node attribute.
    cmds.setKeyframe(blend + ".weight[0]", time=1, value=0.0)
    cmds.setKeyframe(blend + ".weight[0]", time=24, value=0.75)
    cmds.currentTime(1)
    return group, joints[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--result", required=True)
    args = parser.parse_args()
    fixture = read_json(args.fixture) or {}
    result = {"ok": False, "phase": "loading", "targets": 0, "groups": 0,
              "applied": [], "converged": False, "restore": [], "errors": []}
    write_json(args.result, result)

    import maya.standalone
    maya.standalone.initialize(name="python")
    try:
        import maya.cmds as cmds

        entry = SCRIPTS / "MtoULiveLink.py"
        spec = importlib.util.spec_from_file_location("MtoULinkagePeer", str(entry))
        entry_module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(entry_module)
        mtou_config.PORT = int(fixture["port"])

        unused_group, root = create_character(cmds, fixture.get("synthetic_bones"))
        fps = float(fixture.get("fps", 24.0))
        cmds.playbackOptions(minTime=1.0, maxTime=24.0)
        cmds.currentTime(1.0)
        scene = mtou_scene.CharacterScene.capture(root)
        object_id = str(fixture.get("object_id", mtou_config.CHARACTER_OBJECT_ID))
        unit_scale = mtou_space.centimeters_per_unit(
            cmds.currentUnit(query=True, linear=True))
        result["phase"] = "connecting"
        write_json(args.result, result)

        events = []
        session = mtou_session.StreamingSession.start(
            scene, fps, on_event=events.append, object_id=object_id,
            set_revision=1, space_revision=1,
            scene_units=(cmds.currentUnit(query=True, linear=True), unit_scale),
            maya_version=cmds.about(version=True))
        deadline = time.time() + float(fixture.get("timeout_seconds", 60.0))
        while not session.is_ready and time.time() < deadline:
            if any(event.kind == "failed" for event in events):
                break
            time.sleep(0.05)
        if not session.is_ready:
            result["errors"].append("streaming session did not become ready")
            result["phase"] = "not-ready"
            return finish(args.result, result, scene, session, cmds)

        # The greeting is recorded once the handshake completed, not when the
        # worker was merely started.
        result["hello_reply"] = session.hello_reply
        result["session_id"] = (session.hello_reply or {}).get("session_id")
        time_before = float(cmds.currentTime(query=True))
        result["time_before"] = time_before
        host = mtou_link_host.MayaLinkageHost(scene, [object_id], fps)
        status = {"targets": 0, "applied": [], "groups": 0, "errors": []}

        def dispatch(reply):
            reply_type = reply.get("type")
            try:
                if reply_type == "camera_time":
                    status["targets"] += 1
                    link.offer_target(reply)
                elif reply_type == "applied":
                    link.note_applied(reply)
                    status["applied"].append({
                        "eval_id": reply.get("eval_id"),
                        "applied": bool(reply.get("applied")),
                        "sample_serial": reply.get("sample_serial"),
                        "lag_seconds": reply.get("lag_seconds"),
                        "reason": reply.get("reason"),
                    })
                elif reply_type == "mode_state":
                    link.note_mode_state(reply)
                elif reply_type == "reference_offer":
                    link.note_reference_offer(reply)
                elif reply_type == "error":
                    status["errors"].append(
                        "{0}: {1}".format(reply.get("code"), reply.get("message")))
            except Exception as exc:  # a refusal must never kill the worker
                status["errors"].append("{0}: {1}".format(type(exc).__name__, exc))

        link = mtou_linkage.LinkageSession(
            host, session, on_event=events.append, object_id=object_id)
        session.begin_linkage(dispatch)
        link.set_session_id((session.hello_reply or {}).get("session_id"))
        link.enter(1)
        result["phase"] = "linkage"
        write_json(args.result, result)

        delay = float(fixture.get("delay_seconds", 0.0))
        wanted = int(fixture.get("targets", 5))
        group_bytes = 0
        while time.time() < deadline:
            pumped = link.pump()
            if pumped:
                status["groups"] += 1
                group_bytes = max(group_bytes, session.ordered_pending())
            if len(status["applied"]) >= wanted and link.settled():
                break
            if status["errors"] and not status["applied"]:
                break
            time.sleep(0.005)
        if delay:
            time.sleep(delay)
        # One more pump after the wait proves the waiting target is not lost.
        link.pump()

        result.update(targets=status["targets"], groups=status["groups"],
                      applied=status["applied"], errors=status["errors"],
                      converged=link.settled(),
                      linkage_status=link.status().detail,
                      recovery=list(link.recovery_records()),
                      events=[{"kind": event.kind, "detail": event.detail,
                               "diagnostic": event.diagnostic}
                              for event in events[-12:]],
                      pending=session.ordered_pending())
        result["phase"] = "leaving"
        write_json(args.result, result)
        # 返回实时 keeps Unreal's takeover; 结束预览 hands the writers back.
        # The fixture picks which exit this run proves.
        exit_target = str(fixture.get("exit", "realtime"))
        if exit_target not in ("realtime", "end"):
            raise ValueError("fixture exit must be 'realtime' or 'end'")
        result["exit"] = exit_target
        result["restore"] = list(link.leave(exit_target, 2))
        session.resume_after_linkage()
        time.sleep(0.2)
        result["time_after"] = float(cmds.currentTime(query=True))
        result["camera_after"] = bool(cmds.objExists(mtou_config.LINK_CAMERA_NAME))
        result["ok"] = (not result["errors"] and result["groups"] > 0
                        and result["applied"] and not result["restore"]
                        and abs(result["time_after"] - time_before) < 1.0e-6
                        and not result["camera_after"])
        result["phase"] = "done"
        return finish(args.result, result, scene, session, cmds)
    except Exception as exc:
        result["errors"].append(str(exc))
        result["traceback"] = traceback.format_exc()[-2000:]
        result["phase"] = "failed"
        write_json(args.result, result)
        maya.standalone.uninitialize()
        return 1


def finish(result_path, result, scene, session, cmds):
    try:
        if session is not None:
            session.stop()
    except Exception as exc:
        result["errors"].append("stop: {0}".format(exc))
    try:
        if scene is not None:
            scene.close()
    except Exception as exc:
        result["errors"].append("close: {0}".format(exc))
    write_json(result_path, result)
    import maya.standalone
    maya.standalone.uninitialize()
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
