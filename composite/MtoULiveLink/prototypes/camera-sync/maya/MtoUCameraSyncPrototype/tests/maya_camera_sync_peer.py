"""Opt-in Maya peer for the camera sync prototype.

Driven by an Unreal automation test (or by the bundled mock publisher through
``--spawn-mock``): connects to the publisher's real socket, applies the frames
it sends through the real Maya follower, and writes a phase tracked result JSON
after every applied frame so the other host can poll it.

    <mayapy> maya_camera_sync_peer.py --fixture fixture.json --result result.json
    <mayapy> maya_camera_sync_peer.py --fixture fixture.json --result result.json --spawn-mock

Fixture keys: ``host``, ``port``, ``scene_fps``, ``maya_start_frame``,
``camera_name``, ``frames_to_apply``, ``render``, ``evidence_dir``,
``playback_range`` and, when present, the publisher's ready made ``frames``,
``session`` and ``expected`` samples. The scene this peer builds is disposable
and is never saved.
"""

import argparse
import importlib.util
import json
import os
import sys
import tempfile
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


def read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        return None


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


def camera_summary(read_back):
    """The wire read-back rendered with the Unreal side's expected names."""
    gate = read_back.get("defaultResolution") or {}
    return {
        "focal_length_mm": read_back.get("focalLength"),
        "horizontal_film_aperture_in": read_back.get("horizontalFilmAperture"),
        "vertical_film_aperture_in": read_back.get("verticalFilmAperture"),
        "horizontal_film_offset_in": read_back.get("horizontalFilmOffset"),
        "vertical_film_offset_in": read_back.get("verticalFilmOffset"),
        "film_fit": read_back.get("filmFit"),
        "lens_squeeze_ratio": read_back.get("lensSqueezeRatio"),
        "f_stop": read_back.get("fStop"),
        "focus_distance_cm": read_back.get("focusDistance"),
        "depth_of_field": read_back.get("depthOfField"),
        "near_clip_cm": read_back.get("nearClipPlane"),
        "far_clip_used_cm": read_back.get("farClipPlane"),
        "far_clip_substituted": read_back.get("far_clip_substituted"),
        "far_clip_source": read_back.get("far_clip_source"),
        "maya_time": read_back.get("maya_time"),
        "subframe": read_back.get("subframe"),
        "world_matrix": read_back.get("world_matrix"),
        "default_resolution": {"width": gate.get("width"), "height": gate.get("height"),
                               "pixel_aspect": gate.get("pixelAspect")},
    }


def marker_summary(markers):
    return [{"name": marker["name"],
             "unreal_ndc": marker["unreal_ndc"],
             "maya_ndc": marker["maya_ndc"],
             "delta": marker["delta"],
             **({"error": marker["error"]} if "error" in marker else {})}
            for marker in markers]


def applied_entry(report, frame, expected):
    read_back = report.get("camera") or {}
    gate = read_back.get("defaultResolution") or {}
    return {
        "frame_serial": report.get("frame_serial"),
        "status": report.get("status"),
        "detail": report.get("detail"),
        "maya_frame": report.get("maya_frame"),
        "unreal_display_frame": frame["time"]["display_frame"],
        "expected": expected,
        "camera": camera_summary(read_back),
        "gate": {"width": gate.get("width"), "height": gate.get("height"),
                 "display_resolution": read_back.get("displayResolution"),
                 "display_gate_mask": read_back.get("displayGateMask")},
        "markers": marker_summary(report.get("markers") or []),
        "pose": report.get("pose"),
    }


def check_expected(observed, expected, label, problems):
    """Compare the read-back sample against the fixture's expectation."""
    if not expected:
        return
    for key, wanted in expected.items():
        if key in ("max_marker_delta", "marker_tolerance_ndc"):
            continue
        if key not in observed:
            problems.append("{0}: {1} missing from the applied values".format(label, key))
            continue
        actual = observed[key]
        if isinstance(wanted, float) or isinstance(actual, float):
            try:
                if abs(float(actual) - float(wanted)) > 1e-6:
                    problems.append("{0}: {1} = {2!r}, expected {3!r}".format(
                        label, key, actual, wanted))
            except (TypeError, ValueError):
                problems.append("{0}: {1} = {2!r} is not comparable to {3!r}".format(
                    label, key, actual, wanted))
        elif actual != wanted:
            problems.append("{0}: {1} = {2!r}, expected {3!r}".format(
                label, key, actual, wanted))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--result", required=True)
    parser.add_argument("--spawn-mock", action="store_true",
                        help="run the bundled mock publisher on an ephemeral port")
    parser.add_argument("--timeout", type=float, default=120.0,
                        help="seconds to wait for the expected frames")
    args = parser.parse_args(argv)

    fixture = read_json(args.fixture)
    result = {
        "ok": False,
        "phase": "loading",
        "error": None,
        "protocol": {"name": "MtoUCameraSync", "version": 1},
        "session": None,
        "scene_fps": None,
        "applied": [],
        "restore": {},
        "render": {"available": False, "reason": "not requested"},
        "problems": [],
    }
    write_json(args.result, result)
    standalone_ready = False
    applier = None
    mock = None
    publisher = None
    problems = result["problems"]
    try:
        import maya.standalone
        maya.standalone.initialize(name="python")
        standalone_ready = True
        import maya.cmds as cmds

        applier = load_module("MtoUCameraSyncPrototype", SCRIPTS / "MtoUCameraSyncPrototype.py")
        mapping = applier.mapping()

        scene_fps = float(fixture.get("scene_fps") or cmds.currentUnit(query=True, time=True))
        if isinstance(scene_fps, str):
            scene_fps = applier.TIME_UNIT_FPS.get(scene_fps.strip().lower(), 24.0)
        cmds.file(new=True, force=True)
        cmds.currentUnit(linear="cm", time=applier.time_unit_for_fps(scene_fps))
        playback_range = fixture.get("playback_range")
        if playback_range:
            cmds.playbackOptions(minTime=float(playback_range["start"]),
                                 maxTime=float(playback_range["end"]))
        start_frame = float(fixture.get("maya_start_frame", 0.0))
        cmds.currentTime(start_frame)
        result["scene_fps"] = scene_fps
        result["protocol"] = {"name": mapping.PROTOCOL_NAME,
                              "version": mapping.PROTOCOL_VERSION}

        # A keyed witness node proves the follower creates no keys and that the
        # playback range is untouched.
        witness = cmds.createNode("joint", name="MtoUCameraSyncWitness")
        for frame, value in ((start_frame - 5.0, 1.0), (start_frame + 5.0, 9.0)):
            cmds.setKeyframe(witness, attribute="translateX", time=frame, value=value)
        witness_keys_before = int(cmds.keyframe(witness, query=True, keyframeCount=True) or 0)
        # setKeyframe leaves the graph unevaluated until the time changes, so the
        # witness value is read after an explicit move to the session start time.
        cmds.currentTime(start_frame)
        witness_value_before = cmds.getAttr(witness + ".translateX")
        range_before = [float(cmds.playbackOptions(query=True, minTime=True)),
                        float(cmds.playbackOptions(query=True, maxTime=True))]
        frame_before = float(cmds.currentTime(query=True))

        if args.spawn_mock:
            mock = load_module("mock_camera_sync_server", HERE / "mock_camera_sync_server.py")
            if "frames" in fixture:
                serve_fixture = fixture
            else:
                serve_fixture = mock.build_fixture(
                    port=0, scene_fps=scene_fps, maya_start_frame=start_frame,
                    frames_to_apply=int(fixture.get("frames_to_apply", 3)),
                    camera_name=fixture.get("camera_name", applier.DEFAULT_CAMERA_NAME),
                    render=bool(fixture.get("render")),
                    evidence_dir=fixture.get("evidence_dir"))
                if playback_range:
                    serve_fixture["playback_range"] = {
                        "start": float(playback_range["start"]),
                        "end": float(playback_range["end"])}
            publisher = mock.MockPublisher(serve_fixture, host="127.0.0.1", port=0)
            port = publisher.start()
            fixture = serve_fixture
        else:
            port = int(fixture["port"])

        result["phase"] = "connecting"
        write_json(args.result, result)
        follower = applier.CameraSyncFollower(
            host=fixture.get("host", "127.0.0.1"), port=port,
            camera_name=fixture.get("camera_name", applier.DEFAULT_CAMERA_NAME),
            maya_origin_frame=fixture.get("maya_origin_frame"), pose_node=witness)
        follower.connect()
        result["session"] = {key: value for key, value in follower.session.items()
                             if key != "type"}
        idle_job = follower.attach_idle_pump(force=True)
        result["restore"]["idle_pump"] = follower.idle_pump_note
        result["restore"]["idle_job"] = idle_job
        # mayapy delivers no idle events, so the same callback body is run
        # explicitly: this is the pump() path headless Maya needs.
        try:
            follower._idle_tick()
            result["restore"]["idle_tick_ok"] = True
        except Exception as error:
            result["restore"]["idle_tick_ok"] = "{0}: {1}".format(type(error).__name__, error)
            problems.append("the idle pump callback raised: {0}".format(error))
        result["phase"] = "session"
        write_json(args.result, result)

        wanted_serials = int(fixture.get("frames_to_apply", 0))
        expected_by_serial = {sample["frame_serial"]: sample
                              for sample in fixture.get("expected", [])}
        frames_by_serial = {frame["frame_serial"]: frame for frame in fixture.get("frames", [])}
        tolerance = float(fixture.get("marker_tolerance_ndc", 1e-6))
        seen = {}
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            follower.pump()
            for report in follower.drain_reports():
                serial = report.get("frame_serial")
                if serial is None:
                    problems.append("the publisher sent an undecodable line: {0}".format(
                        report.get("detail")))
                    continue
                frame = frames_by_serial.get(serial, {"time": {"display_frame": None}})
                entry = applied_entry(report, frame, expected_by_serial.get(serial))
                if serial in seen:
                    for index, existing in enumerate(result["applied"]):
                        if existing["frame_serial"] == serial:
                            result["applied"][index] = entry
                            break
                else:
                    result["applied"].append(entry)
                seen[serial] = report
                if report.get("status") != "applied":
                    problems.append("frame {0} was {1}: {2}".format(
                        serial, report.get("status"), report.get("detail")))
                else:
                    pose = report.get("pose") or {}
                    sampled = pose.get("sampled_maya_frame")
                    if sampled is None or abs(float(sampled) - report["maya_frame"]) > 1e-6:
                        problems.append("frame {0}: pose was not sampled at the applied Maya time"
                                        .format(serial))
                    else:
                        t = max(0.0, min(1.0, (float(sampled) - start_frame + 5.0) / 10.0))
                        expected_pose = 1.0 + 8.0 * t
                        if abs(float(pose.get("translate_x", float("nan")))
                               - expected_pose) > 1e-5:
                            problems.append("frame {0}: joint pose differs from keyed evaluation"
                                            .format(serial))
                for marker in entry["markers"]:
                    if marker["delta"] is None:
                        problems.append("frame {0} marker {1} could not be projected in "
                                        "Maya: {2}".format(serial, marker["name"],
                                                           marker.get("error")))
                        continue
                    if max(abs(delta) for delta in marker["delta"]) > tolerance:
                        problems.append(
                            "frame {0} marker {1} delta {2} exceeds tolerance {3}".format(
                                serial, marker["name"], marker["delta"], tolerance))
                if entry["expected"]:
                    check_expected(entry["camera"], {
                        key: value for key, value in entry["expected"].items()
                        if key in ("focal_length_mm", "horizontal_film_aperture_in",
                                   "vertical_film_aperture_in", "film_fit",
                                   "horizontal_film_offset_in", "vertical_film_offset_in",
                                   "f_stop", "focus_distance_cm", "depth_of_field",
                                   "near_clip_cm", "far_clip_used_cm",
                                   "lens_squeeze_ratio")},
                        "frame {0} camera".format(serial), problems)
                    check_expected({"maya_frame": report.get("maya_frame"),
                                    "unreal_display_frame": entry["unreal_display_frame"]},
                                   {"maya_frame": entry["expected"]["maya_frame"]},
                                   "frame {0} time".format(serial), problems)
                if entry["gate"]["width"] and entry["gate"]["height"]:
                    declared = (report.get("camera") or {}).get("defaultResolution") or {}
                    if (declared.get("width"), declared.get("height")) != (
                            entry["gate"]["width"], entry["gate"]["height"]):
                        problems.append("frame {0}: resolution gate {1} is not the payload "
                                        "resolution {2}".format(
                                            serial, (declared.get("width"), declared.get("height")),
                                            (entry["gate"]["width"], entry["gate"]["height"])))
                result["phase"] = "applied"
                write_json(args.result, result)
            if wanted_serials and len(seen) >= wanted_serials:
                break
            time.sleep(0.002)
        if wanted_serials and len(seen) < wanted_serials:
            problems.append("only {0} of {1} frames arrived within {2}s".format(
                len(seen), wanted_serials, args.timeout))

        during = float(cmds.currentTime(query=True))
        result["restore"]["maya_frame_during"] = during
        result["restore"]["time_moved"] = abs(during - frame_before) > 1e-9
        shape = follower.camera_nodes[1]
        camera_keys = None
        if shape and cmds.objExists(shape):
            try:
                camera_keys = int(cmds.keyframe(shape, query=True, keyframeCount=True) or 0)
            except RuntimeError:
                camera_keys = 0
        result["restore"]["camera_keys"] = camera_keys
        if camera_keys:
            problems.append("the synced camera has {0} animation keys".format(camera_keys))

        render_requested = bool(fixture.get("render"))
        if render_requested:
            render = load_module("maya_render_marker_check",
                                 HERE / "maya_render_marker_check.py")
            evidence = fixture.get("evidence_dir") or str(
                Path(tempfile.gettempdir()) / "mtou_camera_sync_render_evidence")
            session_payload = dict(follower.session)
            configurations = []
            for frame in fixture.get("frames", [])[:1] or []:
                configurations.append(render.configuration_from_payload(
                    "fixture_frame_{0}".format(frame["frame_serial"]), frame["camera"],
                    frame["markers"], (session_payload["output_resolution"]["x"],
                                       session_payload["output_resolution"]["y"]),
                    session_payload, frame))
            if configurations:
                result["render"] = render.run_render_check(
                    configurations, evidence, tolerance=2.0)
                if not result["render"].get("passed"):
                    problems.append("the rendered framing check did not pass: {0}".format(
                        result["render"].get("reason") or [
                            entry.get("errors") for entry in
                            result["render"].get("configurations", [])]))
            else:
                result["render"] = {"available": False,
                                    "reason": "the fixture carries no frames to render"}

        summary = follower.stop()
        result["restore"].update({
            "maya_frame_before": frame_before,
            "maya_frame_after": float(cmds.currentTime(query=True)),
            "restored_time": summary.get("restored_time"),
            "camera_removed": not cmds.objExists(fixture.get("camera_name",
                                                             applier.DEFAULT_CAMERA_NAME)),
            "removed": summary.get("camera_removed"),
            "playback_range_before": range_before,
            "playback_range_after": [float(cmds.playbackOptions(query=True, minTime=True)),
                                     float(cmds.playbackOptions(query=True, maxTime=True))],
            "witness_keys_before": witness_keys_before,
            "witness_keys_after": int(cmds.keyframe(witness, query=True, keyframeCount=True) or 0),
            "witness_value_before": witness_value_before,
            "witness_value_after": cmds.getAttr(witness + ".translateX"),
            "frames_applied": follower.frames_applied,
            "frames_unchanged": follower.frames_unchanged,
            "frames_rejected": follower.frames_rejected,
            "frames_subframe": follower.frames_subframe,
            "far_clip_substitutions": follower.far_clip_substitutions,
            "max_marker_delta": follower.max_marker_delta,
            "state": follower.state,
            "detail": follower.detail,
            "error": follower.error,
        })
        if abs(result["restore"]["maya_frame_after"] - frame_before) > 1e-9:
            problems.append("Maya's current time was not restored: {0} -> {1}".format(
                frame_before, result["restore"]["maya_frame_after"]))
        if not result["restore"]["camera_removed"]:
            problems.append("the synced camera still exists after stop()")
        if result["restore"]["playback_range_before"] != result["restore"]["playback_range_after"]:
            problems.append("the playback range changed: {0} -> {1}".format(
                result["restore"]["playback_range_before"],
                result["restore"]["playback_range_after"]))
        if result["restore"]["witness_keys_after"] != witness_keys_before:
            problems.append("the scene key count changed: {0} -> {1}".format(
                witness_keys_before, result["restore"]["witness_keys_after"]))
        if result["restore"]["witness_value_after"] != witness_value_before:
            problems.append("the witness animation value changed")

        result["ok"] = not problems
        result["phase"] = "done"
        write_json(args.result, result)
    except Exception:
        result["error"] = traceback.format_exc()
        result["phase"] = "failed"
        result["ok"] = False
        write_json(args.result, result)
    finally:
        if publisher is not None:
            try:
                publisher.stop()
            except Exception:
                pass
        if standalone_ready:
            import maya.standalone
            maya.standalone.uninitialize()
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
