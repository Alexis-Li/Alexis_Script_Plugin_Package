"""Long-run Maya peer for the prototype's natural editor-loop check.

Unlike ``maya_camera_sync_peer.py``, which applies a fixture with a known frame
list, this peer follows whatever the open Unreal Sequencer publishes: the editor
loop decides how many frames and which times arrive. It stays alive across the
checks the Unreal side performs, so one Maya process covers

* follow, seeks and camera-cut changes driven by the editor playhead;
* a camera edited at the parked frame (a new evaluation identity, same time);
* a connection the client drops without ``bye``, and a manual reconnect;
* every publisher start/stop cycle, each of which releases Maya's own scene.

Each connection lifetime is recorded as a *cycle*: what was applied, how the
connection ended, and what Maya looked like after the session released it
(current time, camera node, scene resolution gate, scriptJobs, keys and
playback range). The result file is replaced as the session progresses so the
publisher can poll it, and a control file lets the publisher ask for a
disconnect or for the peer to exit.

    <mayapy> maya_camera_sync_live_peer.py --port 54330 --result live.json \
        [--control control.json] [--budget 300] [--idle-exit 12]

Nothing is ever saved; the scene is disposable.
"""

import argparse
import importlib.util
import json
import os
import sys
import time
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SCRIPTS = ROOT / "scripts"
for entry in (str(HERE), str(SCRIPTS)):
    if entry not in sys.path:
        sys.path.insert(0, entry)


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, str(path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_json(path, value):
    """Replace the result file, retrying while a reader holds it open."""
    data = json.dumps(value, indent=2, ensure_ascii=True, default=str)
    target = Path(path)
    temporary = target.with_name(target.name + ".tmp")
    for attempt in range(50):
        try:
            temporary.write_text(data, encoding="utf-8")
            os.replace(str(temporary), str(target))
            return
        except (PermissionError, OSError):
            if attempt == 49:
                raise
            time.sleep(0.02)


def read_control(path):
    """The publisher's control actions, or ``None`` when there is no file."""
    if not path:
        return None
    try:
        return json.loads(Path(path).read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        return None


def control_actions(control):
    return [action.get("action") for action in (control or {}).get("actions", [])]


def scene_snapshot(cmds, applier, pose_node, camera_name):
    """The parts of the Maya scene a session is allowed to touch."""
    resolution = {}
    for plug in ("defaultResolution.width", "defaultResolution.height",
                 "defaultResolution.pixelAspect",
                 "defaultResolution.deviceAspectRatio"):
        try:
            resolution[plug] = cmds.getAttr(plug)
        except RuntimeError:
            resolution[plug] = None
    keys = None
    if pose_node and cmds.objExists(pose_node):
        try:
            keys = int(cmds.keyframe(pose_node, query=True, keyframeCount=True) or 0)
        except RuntimeError:
            keys = None
    return {
        "maya_frame": float(cmds.currentTime(query=True)),
        "playback_range": [float(cmds.playbackOptions(query=True, minTime=True)),
                           float(cmds.playbackOptions(query=True, maxTime=True))],
        "default_resolution": resolution,
        "camera_exists": bool(cmds.objExists(camera_name)),
        "script_jobs": list(cmds.scriptJob(listJobs=True) or []),
        "pose_keys": keys,
    }


def cycle_evidence(cmds, applier, pose_node, camera_name, before):
    """What the scene looks like after a session released it, against ``before``."""
    after = scene_snapshot(cmds, applier, pose_node, camera_name)
    evidence = {
        "maya_frame_before": before["maya_frame"],
        "maya_frame_after": after["maya_frame"],
        "time_restored": abs(after["maya_frame"] - before["maya_frame"]) <= 1e-9,
        "camera_before": before["camera_exists"],
        "camera_after": after["camera_exists"],
        "resolution_before": before["default_resolution"],
        "resolution_after": after["default_resolution"],
        "resolution_restored": after["default_resolution"] == before["default_resolution"],
        "playback_range_before": before["playback_range"],
        "playback_range_after": after["playback_range"],
        "range_restored": after["playback_range"] == before["playback_range"],
        "pose_keys_before": before["pose_keys"],
        "pose_keys_after": after["pose_keys"],
        "keys_restored": after["pose_keys"] == before["pose_keys"],
        "script_jobs_before": before["script_jobs"],
        "script_jobs_after": after["script_jobs"],
        "callbacks_restored": after["script_jobs"] == before["script_jobs"],
    }
    if evidence["camera_after"] and not evidence["camera_before"]:
        # A camera the session created has to be gone; one it found stays.
        evidence["camera_released"] = False
    else:
        evidence["camera_released"] = True
    evidence["restored"] = all(evidence[key] for key in
                               ("time_restored", "resolution_restored", "range_restored",
                                "keys_restored", "callbacks_restored", "camera_released"))
    return evidence


def check_cycle(cycle, problems):
    """Turn one cycle's evidence into reported problems."""
    label = "cycle {0}".format(cycle["index"])
    restore = cycle["restore"]
    if cycle["frames_rejected"]:
        problems.append("{0}: {1} frames were rejected".format(
            label, cycle["frames_rejected"]))
    checks = (("time_restored", "maya_frame_before", "maya_frame_after",
               "Maya's current time was not restored"),
              ("resolution_restored", "resolution_before", "resolution_after",
               "the scene resolution gate was not restored"),
              ("range_restored", "playback_range_before", "playback_range_after",
               "the playback range changed"),
              ("keys_restored", "pose_keys_before", "pose_keys_after",
               "the keyed witness node changed"),
              ("callbacks_restored", "script_jobs_before", "script_jobs_after",
               "a scriptJob survived the session"))
    for key, before, after, message in checks:
        if not restore.get(key):
            problems.append("{0}: {1} ({2!r} -> {3!r})".format(
                label, message, restore.get(before), restore.get(after)))
    if not restore["camera_released"]:
        problems.append("{0}: the camera the session created still exists".format(label))
    if not restore["restored"]:
        problems.append("{0}: the session did not release the scene completely".format(label))


def applied_entry(report, cycle_index):
    """One applied frame in the vocabulary the Unreal side asserts on."""
    read_back = report.get("camera") or {}
    deltas = [max(abs(value) for value in marker["delta"])
              for marker in (report.get("markers") or []) if marker.get("delta")]
    pose = report.get("pose") or {}
    return {
        "cycle": cycle_index,
        "frame_serial": report.get("frame_serial"),
        "eval_serial": report.get("eval_serial"),
        "eval_identity": report.get("eval_identity"),
        "maya_frame": report.get("maya_frame"),
        "unreal_display_frame": report.get("unreal_display_frame"),
        "focal_length_mm": read_back.get("focalLength"),
        "film_fit": read_back.get("filmFit"),
        "f_stop": read_back.get("fStop"),
        "focus_distance_cm": read_back.get("focusDistance"),
        "far_clip_used_cm": read_back.get("farClipPlane"),
        "default_resolution": read_back.get("defaultResolution"),
        "subframe": read_back.get("subframe"),
        "max_marker_delta": max(deltas) if deltas else None,
        "pose_value": pose.get("translate_x"),
        "pose_sampled_frame": pose.get("sampled_maya_frame"),
    }


def check_pose(entry, start_frame, problems):
    """The witness has to be sampled at the applied time and match the keys."""
    label = "cycle {0} frame {1}".format(entry["cycle"], entry["frame_serial"])
    sampled = entry["pose_sampled_frame"]
    if sampled is None:
        problems.append("{0} carries no pose witness".format(label))
        return
    if abs(float(sampled) - float(entry["maya_frame"])) > 1e-6:
        problems.append("{0} sampled the pose at {1}, not at {2}".format(
            label, sampled, entry["maya_frame"]))
        return
    t = max(0.0, min(1.0, (float(sampled) - (start_frame - 5.0)) / 10.0))
    expected = 1.0 + 8.0 * t
    entry["expected_pose"] = expected
    if abs(float(entry["pose_value"]) - expected) > 1e-5:
        problems.append("{0} witness {1} differs from the keyed {2}".format(
            label, entry["pose_value"], expected))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--result", required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--control", default=None,
                        help="JSON file the publisher writes control actions into")
    parser.add_argument("--scene-fps", type=float, default=24.0)
    parser.add_argument("--start-frame", type=float, default=1001.0)
    parser.add_argument("--camera-name", default="MtoU_UE_Camera")
    parser.add_argument("--pose-node", default="MtoUCameraSyncWitness")
    parser.add_argument("--budget", type=float, default=300.0,
                        help="overall wall clock limit for the whole peer")
    parser.add_argument("--idle-exit", type=float, default=12.0,
                        help="after the publisher ends a session, how long to wait for the next")
    parser.add_argument("--reconnect-wait", type=float, default=0.5)
    parser.add_argument("--max-applied", type=int, default=60,
                        help="applied frames kept per cycle; the totals are always recorded")
    parser.add_argument("--marker-tolerance-ndc", type=float, default=1e-6)
    args = parser.parse_args(argv)

    result = {
        "ok": False,
        "phase": "loading",
        "error": None,
        "protocol": {"name": "MtoUCameraSync", "version": 1},
        "host": args.host,
        "port": args.port,
        "scene_fps": args.scene_fps,
        "pose_node": args.pose_node,
        "connected": False,
        "detail": "",
        "cycles": [],
        "control": {"actions": []},
        "scene_before": {},
        "final": {},
        "problems": [],
    }
    write_json(args.result, result)
    problems = result["problems"]
    started = time.time()
    deadline = started + args.budget
    standalone_ready = False
    follower = None
    try:
        import maya.standalone
        maya.standalone.initialize(name="python")
        standalone_ready = True
        import maya.cmds as cmds

        applier = load_module("MtoUCameraSyncPrototype", SCRIPTS / "MtoUCameraSyncPrototype.py")
        mapping = applier.mapping()
        result["protocol"] = {"name": mapping.PROTOCOL_NAME,
                              "version": mapping.PROTOCOL_VERSION}

        cmds.file(new=True, force=True)
        cmds.currentUnit(linear="cm", time=applier.time_unit_for_fps(args.scene_fps))
        cmds.playbackOptions(minTime=args.start_frame, maxTime=args.start_frame + 50.0)
        cmds.currentTime(args.start_frame)
        pose_node = None
        if args.pose_node:
            pose_node = cmds.createNode("joint", name=args.pose_node)
            cmds.setKeyframe(pose_node, attribute="translateX",
                             time=args.start_frame - 5.0, value=1.0)
            cmds.setKeyframe(pose_node, attribute="translateX",
                             time=args.start_frame + 5.0, value=9.0)
            # Linear tangents make the witness value a known function of the sampled
            # frame, so the publisher's own check has an expectation to compare with.
            cmds.keyTangent(pose_node, attribute="translateX",
                            inTangentType="linear", outTangentType="linear")
            cmds.currentTime(args.start_frame)
        scene_before = scene_snapshot(cmds, applier, pose_node, args.camera_name)
        result["scene_before"] = scene_before

        follower = applier.CameraSyncFollower(
            host=args.host, port=args.port, camera_name=args.camera_name,
            connect_timeout=10.0, pose_node=pose_node)
        # The publisher's session message sets the Maya origin to the Unreal playback
        # start, so both hosts agree on frame numbers regardless of Maya's own frame.
        cycle_index = 0
        handled = set()
        exit_requested = False
        publisher_ended_at = None
        last_written = 0.0
        while time.time() < deadline and not exit_requested:
            for action in control_actions(read_control(args.control)):
                if action in handled:
                    continue
                handled.add(action)
                if action == "exit":
                    exit_requested = True
                    result["control"]["actions"].append({"action": action, "state": "done"})
            if exit_requested:
                break
            session_before = scene_snapshot(cmds, applier, pose_node, args.camera_name)
            # The follower's counters are snapshot before connecting: a publisher can send
            # its first frames in the same batch as the session message, and those belong
            # to this cycle, not to the baseline.
            counter_before = (follower.frames_applied, follower.frames_unchanged,
                              follower.frames_subframe)
            if not follower.connected:
                # The publisher may start another session on the same port, so a peer
                # that is between sessions keeps trying until the exit action arrives or
                # the publisher has been gone for `idle-exit` seconds.
                if (publisher_ended_at is not None
                        and time.time() - publisher_ended_at > args.idle_exit):
                    break
                result["phase"] = "connecting"
                attempt = {"at": round(time.time() - started, 3), "connected": False,
                           "error": None, "started": round(time.time() - started, 3)}
                result.setdefault("connect_attempts", []).append(attempt)
                try:
                    follower.connect()
                    attempt["connected"] = True
                except applier.SyncRefused as error:
                    attempt["error"] = str(error)
                    attempt["seconds"] = round(time.time() - started - attempt["at"], 3)
                    result["detail"] = str(error)
                    write_json(args.result, result)
                    time.sleep(args.reconnect_wait)
                    continue
                attempt["seconds"] = round(time.time() - started - attempt["at"], 3)
                publisher_ended_at = None

            cycle_index += 1
            result["phase"] = "following"
            result["connected"] = True
            cycle = {
                "index": cycle_index,
                "session": {key: value for key, value in (follower.session or {}).items()
                            if key != "type"},
                "frames_applied": 0,
                "frames_unchanged": 0,
                "frames_subframe": 0,
                "frames_rejected": 0,
                "max_marker_delta": None,
                "focal_lengths": [],
                "display_frame_min": None,
                "display_frame_max": None,
                "disconnect": None,
                "applied_entries": [],
                "restore": {},
            }
            result["cycles"].append(cycle)
            cycle_started = time.time()
            write_json(args.result, result)

            while follower.state == applier.STATE_FOLLOWING and time.time() < deadline:
                follower.pump()
                for report in follower.drain_reports():
                    if report.get("frame_serial") is None:
                        problems.append("cycle {0}: undecodable publisher line {1}".format(
                            cycle_index, report.get("detail")))
                        continue
                    if report.get("status") != applier.STATUS_APPLIED:
                        cycle["frames_rejected"] += 1
                        problems.append("cycle {0}: frame {1} was {2}: {3}".format(
                            cycle_index, report.get("frame_serial"), report.get("status"),
                            report.get("detail")))
                        continue
                    entry = applied_entry(report, cycle_index)
                    check_pose(entry, args.start_frame, problems)
                    cycle["frames_applied"] += 1
                    display_frame = entry["unreal_display_frame"]
                    if display_frame is not None:
                        cycle["display_frame_min"] = (display_frame if
                                                      cycle["display_frame_min"] is None
                                                      else min(cycle["display_frame_min"],
                                                               display_frame))
                        cycle["display_frame_max"] = (display_frame if
                                                      cycle["display_frame_max"] is None
                                                      else max(cycle["display_frame_max"],
                                                               display_frame))
                    if entry["focal_length_mm"] is not None and \
                            entry["focal_length_mm"] not in cycle["focal_lengths"]:
                        cycle["focal_lengths"].append(entry["focal_length_mm"])
                    if entry["max_marker_delta"] is not None:
                        cycle["max_marker_delta"] = max(
                            cycle["max_marker_delta"] or 0.0, entry["max_marker_delta"])
                        if entry["max_marker_delta"] > args.marker_tolerance_ndc:
                            problems.append(
                                "cycle {0}: frame {1} marker delta {2} exceeds {3}".format(
                                    cycle_index, entry["frame_serial"],
                                    entry["max_marker_delta"], args.marker_tolerance_ndc))
                    retained = cycle["applied_entries"]
                    retained.append(entry)
                    if len(retained) > args.max_applied:
                        del retained[0:len(retained) - args.max_applied]
                    # The publisher polls this file, but a summary is enough between
                    # heartbeats: the peer writes at most a few times per second.
                    if time.time() - last_written >= 0.2:
                        last_written = time.time()
                        write_json(args.result, result)

                for action in control_actions(read_control(args.control)):
                    if action in handled:
                        continue
                    handled.add(action)
                    if action == "drop" and follower.connected:
                        follower.abort("client dropped the connection")
                        cycle["disconnect"] = "client_drop"
                        result["control"]["actions"].append({"action": action, "state": "done"})
                    elif action == "exit":
                        exit_requested = True
                        result["control"]["actions"].append({"action": action, "state": "done"})
                    write_json(args.result, result)
                if (follower.state != applier.STATE_FOLLOWING or exit_requested
                        or not follower.connected):
                    break
                time.sleep(0.002)

            # The publisher's "end" already released the scene; a dropped or lost
            # connection is released here, before the cycle's evidence is taken.
            if cycle["disconnect"] is None:
                cycle["disconnect"] = ("publisher_end"
                                       if follower.state == applier.STATE_STOPPED
                                       else "lost_connection")
            follower.stop("live peer released cycle {0}".format(cycle_index))
            result["connected"] = False
            cycle["seconds"] = round(time.time() - cycle_started, 3)
            cycle["frames_unchanged"] = follower.frames_unchanged - counter_before[1]
            cycle["frames_subframe"] = follower.frames_subframe - counter_before[2]
            cycle["follower_totals"] = {
                "frames_applied": follower.frames_applied,
                "frames_unchanged": follower.frames_unchanged,
                "frames_subframe": follower.frames_subframe,
            }
            cycle["restore"] = cycle_evidence(cmds, applier, pose_node, args.camera_name,
                                              session_before)
            check_cycle(cycle, problems)
            write_json(args.result, result)
            if cycle["disconnect"] == "publisher_end":
                publisher_ended_at = time.time()

            if exit_requested or time.time() >= deadline:
                break
        
        if time.time() >= deadline and not exit_requested:
            problems.append("the peer reached its {0}s budget".format(args.budget))
            result["phase"] = "budget"
        else:
            result["phase"] = "done"
        result["final"] = scene_snapshot(cmds, applier, pose_node, args.camera_name)
        if result["final"]["script_jobs"] != scene_before["script_jobs"]:
            problems.append("scriptJobs remain after the last session: {0}".format(
                result["final"]["script_jobs"]))
        result["ok"] = not problems
    except Exception:
        result["error"] = traceback.format_exc()
        result["phase"] = "failed"
        result["ok"] = False
    finally:
        if follower is not None:
            try:
                follower.stop("peer shutting down")
            except Exception:
                pass
        result["seconds"] = round(time.time() - started, 2)
        write_json(args.result, result)
        if standalone_ready:
            import maya.standalone
            maya.standalone.uninitialize()
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
