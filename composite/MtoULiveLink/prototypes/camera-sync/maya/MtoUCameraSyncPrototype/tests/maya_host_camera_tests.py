"""Mayapy host checks for the camera sync prototype.

Runs in a disposable scene and never saves anything. Two levels:

* apply one representative payload directly (non-zero start frame, 2.39:1
  sensor on a 16:9 gate, film offsets, depth of field on) and assert every
  attribute, the resolution gate, the world matrix, the applied time, that no
  keys and no playback range change survive, that the marker NDC matches the
  Unreal NDC that came with the payload, and that ``stop()`` restores the scene;
* drive a real socket session against the bundled mock publisher, including a
  second connection being refused, and compare the wire reports.

``--render`` additionally renders the synced camera with Arnold and compares the
marker centroids in the image with the same projection maths.

    <mayapy> maya_host_camera_tests.py --result host_result.json [--render]
"""

import argparse
import importlib.util
import json
import socket
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


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--result", required=True,
                        help="path for the JSON evidence file")
    parser.add_argument("--render", action="store_true",
                        help="also render the synced camera with Arnold")
    parser.add_argument("--render-dir", default=None,
                        help="directory for rendered evidence images")
    parser.add_argument("--frames", type=int, default=3)
    args = parser.parse_args(argv)

    report = {"ok": False, "phase": "loading", "render": {"available": False,
                                                          "reason": "not requested"}}
    checks = Checks()
    started = time.time()
    try:
        import maya.standalone
        maya.standalone.initialize(name="python")
        import maya.cmds as cmds

        applier = load_module("MtoUCameraSyncPrototype", SCRIPTS / "MtoUCameraSyncPrototype.py")
        mock = load_module("mock_camera_sync_server", HERE / "mock_camera_sync_server.py")
        mapping = applier.mapping()

        report.update(maya_version=cmds.about(version=True),
                      maya_api=cmds.about(apiVersion=True))

        cmds.file(new=True, force=True)
        cmds.currentUnit(linear="cm", time="film")
        fixture = mock.build_fixture(scene_fps=24.0, maya_start_frame=1001.0,
                                     frames_to_apply=args.frames, port=0)
        playback = fixture["playback_range"]
        cmds.playbackOptions(minTime=playback["start"], maxTime=playback["end"])
        cmds.currentTime(fixture["maya_start_frame"])
        witness = cmds.createNode("transform", name="HostWitness")
        cmds.setKeyframe(witness, attribute="translateX",
                         time=playback["start"], value=1.0)
        cmds.setKeyframe(witness, attribute="translateX",
                         time=playback["end"], value=5.0)
        subframe_joint = cmds.createNode("joint", name="MtoUSubframeJoint")
        subframe_before = int(fixture["expected"][2]["maya_frame"])
        cmds.setKeyframe(subframe_joint, attribute="translateX",
                         time=subframe_before, value=0.0)
        cmds.setKeyframe(subframe_joint, attribute="translateX",
                         time=subframe_before + 1, value=10.0)
        cmds.keyTangent(subframe_joint, attribute="translateX",
                        inTangentType="linear", outTangentType="linear")

        check = checks.check
        check("scene linear unit is centimetres",
              cmds.currentUnit(query=True, linear=True).lower() == "cm")
        check("scene time unit is a frame rate",
              applier.scene_time_unit()[0] == "film")
        check("unsupported time units are refused", _refuses_seconds(cmds, applier))

        # ------------------------------------------------------------ direct
        report["phase"] = "direct"
        frame = json.loads(json.dumps(fixture["frames"][2]))  # non-zero start, sub-frame
        frame["type"] = "frame"
        frame["session"] = fixture["session"]["sequence"]
        frame["sequence"] = 1
        follower = applier.CameraSyncFollower(
            port=1, camera_name=fixture["camera_name"],
            maya_origin_frame=fixture["maya_start_frame"])
        follower._scene_fps = 24.0
        follower._start_time = float(cmds.currentTime(query=True))
        follower._undo_state = bool(cmds.undoInfo(query=True, state=True))
        follower.session = dict(mock.session_message(fixture, 0))
        range_before = (float(cmds.playbackOptions(query=True, minTime=True)),
                        float(cmds.playbackOptions(query=True, maxTime=True)))
        applied = follower.apply_frame(frame, follower.session)
        transform, shape = follower.camera_nodes

        check("frame was applied", applied["status"] == "applied",
              str(applied["status"]) + " " + str(applied["detail"]))
        check("report echoes the evaluation identity",
              applied["eval_serial"] == frame["eval_serial"] and
              applied["eval_identity"] == frame["eval_identity"],
              "{0!r} / {1!r}".format(applied["eval_serial"], applied["eval_identity"]))
        check("subframe was applied without quantization",
              applied["camera"]["subframe"],
              "display frame {0}".format(frame["time"]["display_frame"]))

        expected_camera = frame["camera"]
        read_back = {key: value for key, value in applied["camera"].items()
                     if key not in ("world_matrix", "defaultResolution")}
        checks.record("read_back", read_back)
        checks.close("focalLength", read_back["focalLength"],
                     expected_camera["focal_length_mm"])
        checks.close("horizontalFilmAperture", read_back["horizontalFilmAperture"],
                     expected_camera["sensor_width_mm"] / 25.4)
        checks.close("verticalFilmAperture", read_back["verticalFilmAperture"],
                     expected_camera["sensor_height_mm"] / 25.4)
        checks.close("horizontalFilmOffset", read_back["horizontalFilmOffset"],
                     expected_camera["sensor_horizontal_offset_mm"] / 25.4)
        checks.close("verticalFilmOffset", read_back["verticalFilmOffset"],
                     expected_camera["sensor_vertical_offset_mm"] / 25.4)
        check("filmFit is Horizontal for MaintainXFOV",
              read_back["filmFit"] == mapping.FILM_FIT_HORIZONTAL,
              "filmFit={0}".format(read_back["filmFit"]))
        checks.close("lensSqueezeRatio", read_back["lensSqueezeRatio"],
                     expected_camera["squeeze_factor"])
        checks.close("fStop", read_back["fStop"], expected_camera["f_stop"])
        checks.close("focusDistance", read_back["focusDistance"],
                     expected_camera["focus_distance_cm"])
        check("depthOfField is on", read_back["depthOfField"] is True)
        checks.close("nearClipPlane", read_back["nearClipPlane"],
                     expected_camera["near_clip_cm"])
        checks.close("farClipPlane uses the declared fallback",
                     read_back["farClipPlane"],
                     fixture["session"]["far_clip_fallback_cm"])
        check("far clip substitution is reported",
              read_back["far_clip_substituted"] is True and
              read_back["far_clip_source"] == "session_fallback",
              "{0} from {1}".format(read_back["farClipPlane"],
                                    read_back["far_clip_source"]))

        # The world matrix on the node must be the mapped Unreal transform.
        basis = mapping.payload_basis(expected_camera)
        expected_matrix = mapping.ue_basis_to_maya_matrix(
            basis[0], basis[1], basis[2], mapping.payload_location(expected_camera))
        live_matrix = [float(value) for value in
                       cmds.xform(transform, query=True, worldSpace=True, matrix=True)]
        checks.record("world_matrix_live", live_matrix)
        checks.record("world_matrix_expected", list(expected_matrix))
        check("world matrix matches the mapped Unreal transform",
              all(abs(first - second) <= 1e-9
                  for first, second in zip(live_matrix, expected_matrix)),
              "max delta {0}".format(max(abs(first - second) for first, second
                                         in zip(live_matrix, expected_matrix))))
        check("report world matrix matches the node",
              all(abs(first - second) <= 1e-9 for first, second in
                  zip(applied["camera"]["world_matrix"], live_matrix)))

        gate = applied["camera"]["defaultResolution"]
        check("resolution gate is the payload resolution",
              (gate["width"], gate["height"]) ==
              (int(frame["output_resolution"]["x"]), int(frame["output_resolution"]["y"])),
              str(gate))
        check("resolution gate and gate mask are displayed",
              read_back["displayResolution"] is True and read_back["displayGateMask"] is True)

        applied_time = float(cmds.currentTime(query=True))
        checks.close("applied time", applied_time, applied["maya_frame"])
        checks.close("Maya joint evaluates the exact subframe",
                     float(cmds.getAttr(subframe_joint + ".translateX")), 5.0)
        expected_sample = fixture["expected"][2]
        checks.close("applied time follows the contract",
                     applied["maya_frame"], expected_sample["maya_frame"])
        checks.close("the applied frame retains the contract subframe",
                     applied["maya_frame"], expected_sample["maya_time"])
        check("the raw contract time is sub-frame",
              not float(expected_sample["maya_time"]).is_integer(),
              "maya_time {0}".format(expected_sample["maya_time"]))

        # ------------------------------------------------------- marker NDC
        markers = applied["markers"]
        checks.record("markers", markers)
        check("every marker was projected", all(marker["delta"] is not None for marker in markers),
              str([marker.get("error") for marker in markers]))
        worst = max(abs(value) for marker in markers if marker["delta"]
                    for value in marker["delta"]) if markers else None
        checks.record("max_marker_ndc_delta", worst)
        check("marker NDC matches the Unreal NDC",
              worst is not None and worst <= 1e-6,
              "max delta {0}".format(worst))

        # ------------------------------------------------- scene safety
        playback_range_check = (float(cmds.playbackOptions(query=True, minTime=True)),
                                float(cmds.playbackOptions(query=True, maxTime=True)))
        check("playback range is untouched", playback_range_check == range_before,
              "{0} vs {1}".format(playback_range_check, range_before))
        try:
            camera_keys = int(cmds.keyframe(shape, query=True, keyframeCount=True) or 0)
        except RuntimeError:
            camera_keys = 0
        check("no keys on the synced camera", camera_keys == 0, str(camera_keys))
        check("witness keys are untouched",
              int(cmds.keyframe(witness, query=True, keyframeCount=True) or 0) == 2)

        # The publisher's heartbeat repeats an unchanged target. A pose keyed at the very
        # frame the session sits on has to reach Unreal on the next report instead of
        # leaving the previous value converged.
        heartbeat_witness = cmds.createNode("transform", name="MtoUHeartbeatWitness")
        cmds.setKeyframe(heartbeat_witness, attribute="translateX",
                         time=playback["start"], value=1.0)
        cmds.setKeyframe(heartbeat_witness, attribute="translateX",
                         time=playback["end"], value=5.0)
        follower.pose_node = heartbeat_witness
        edited_frame = float(cmds.currentTime(query=True))
        cmds.setKeyframe(heartbeat_witness, attribute="translateX",
                         time=edited_frame, value=42.0)
        # setKeyframe leaves the graph unevaluated until the time changes.
        cmds.currentTime(edited_frame - 1.0)
        cmds.currentTime(edited_frame)
        heartbeat = json.loads(json.dumps(frame))
        heartbeat["frame_serial"] = frame["frame_serial"] + 1
        updated = follower.apply_frame(heartbeat, follower.session)
        checks.record("heartbeat_report", {
            "frame_serial": updated.get("frame_serial"),
            "eval_serial": updated.get("eval_serial"),
            "eval_identity": updated.get("eval_identity"),
            "status": updated.get("status"),
            "pose": updated.get("pose"),
            "detail": updated.get("detail")})
        check("the heartbeat report is applied", updated["status"] == "applied",
              str(updated.get("detail")))
        check("the heartbeat repeats the target and advances the transport serial",
              updated["eval_identity"] == frame["eval_identity"] and
              updated["eval_serial"] == frame["eval_serial"] and
              updated["frame_serial"] == frame["frame_serial"] + 1)
        check("a pose edited at the same frame is reported by the next heartbeat",
              updated["pose"]["translate_x"] == 42.0, str(updated.get("pose")))
        checks.close("the edited pose is sampled at the applied Maya time",
                     updated["pose"]["sampled_maya_frame"], applied["maya_frame"])

        # A rejected frame must leave the camera exactly as it was.
        broken = json.loads(json.dumps(frame))
        del broken["camera"]["focal_length_mm"]
        before_reject = [float(value) for value in
                         cmds.xform(transform, query=True, worldSpace=True, matrix=True)]
        rejected = follower.apply_frame(broken, follower.session)
        after_reject = [float(value) for value in
                        cmds.xform(transform, query=True, worldSpace=True, matrix=True)]
        check("a frame missing a field is rejected",
              rejected["status"] == "rejected" and
              mapping.ERR_MISSING_FIELD in " ".join(rejected["detail"]),
              str(rejected["detail"]))
        check("a rejected frame does not touch the camera", before_reject == after_reject)
        drained = [entry.get("status") for entry in follower.drain_reports()]
        check("a rejected frame reaches the report queue",
              "rejected" in drained, str(drained))

        stop_summary = follower.stop()
        checks.record("stop", stop_summary)
        check("stop restored the captured time",
              abs(float(cmds.currentTime(query=True)) - fixture["maya_start_frame"]) <= 1e-9,
              str(cmds.currentTime(query=True)))
        check("stop removed the camera it created",
              not cmds.objExists(fixture["camera_name"]))
        check("stop left no scriptJob", not (cmds.scriptJob(listJobs=True) or []),
              str(cmds.scriptJob(listJobs=True)))

        # ------------------------------------------------- socket session
        report["phase"] = "socket"
        socket_initial_time = fixture["maya_start_frame"] - 11.0
        cmds.currentTime(socket_initial_time)
        publisher = mock.MockPublisher(fixture, host="127.0.0.1", port=0)
        port = publisher.start()
        driver = applier.CameraSyncFollower(
            port=port, maya_origin_frame=fixture["maya_origin_frame"])
        session = driver.connect()
        check("handshake returned the session", session["protocol"] == mock.PROTOCOL_NAME,
              str(session.get("protocol")))
        check("hello declared the Maya scene rate",
              publisher.hello["scene_fps"] == 24.0 and
              publisher.hello["time_unit"] == "film", str(publisher.hello))
        # A second client is refused while this one holds the session.
        second = socket.create_connection(("127.0.0.1", port), timeout=5.0)
        second.settimeout(5.0)
        refusal = b""
        deadline = time.time() + 5.0
        while time.time() < deadline and b"\n" not in refusal:
            try:
                chunk = second.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            refusal += chunk
        second.close()
        refusal_message = json.loads(refusal.split(b"\n")[0]) if refusal else {}
        check("a second connection is refused",
              refusal_message.get("type") == "error", str(refusal_message))
        # A client message that is not hello/applied/bye is refused by the
        # publisher, which is the protocol's no-feedback-loop guarantee.
        driver._send({"type": "time", "display_frame": 12345.0})
        deadline = time.time() + 10.0
        while time.time() < deadline and not driver.server_errors:
            driver.pump()
            time.sleep(0.005)
        guard = driver.server_errors[-1] if driver.server_errors else None
        check("Maya cannot control Unreal time",
              guard is not None and guard["category"] == "CLIENT_MAY_NOT_CONTROL_TIME",
              str(guard))
        check("a refused client message leaves the session following",
              driver.state == applier.STATE_FOLLOWING, str(driver.state))

        seen = {}
        frames_by_serial = {frame["frame_serial"]: frame for frame in fixture["frames"]}
        deadline = time.time() + 60.0
        while time.time() < deadline and len(seen) < args.frames:
            driver.pump()
            for message in driver.drain_reports():
                if message.get("frame_serial") is not None:
                    seen[message["frame_serial"]] = message
            time.sleep(0.002)
        checks.record("socket_reports", {str(key): {
            "status": value["status"], "maya_frame": value["maya_frame"],
            "markers": value["markers"], "detail": value["detail"]}
            for key, value in seen.items()})
        check("the session applied every frame",
              len(seen) == args.frames, "applied {0} of {1}".format(len(seen), args.frames))
        check("the session ordered the frames",
              sorted(seen) == list(range(1, args.frames + 1)), str(sorted(seen)))
        check("the applied times follow the payload",
              all(abs(seen[serial]["maya_frame"]
                      - fixture["expected"][serial - 1]["maya_frame"]) <= 1e-9
                  for serial in seen),
              str({serial: seen[serial]["maya_frame"] for serial in seen}))
        check("the wire reports carry the applied values",
              all(seen[serial]["camera"] and "focalLength" in seen[serial]["camera"]
                  for serial in seen))
        check("the wire reports echo the evaluation identity",
              all(seen[serial]["eval_serial"] == frames_by_serial[serial]["eval_serial"] and
                  seen[serial]["eval_identity"] == frames_by_serial[serial]["eval_identity"]
                  for serial in seen),
              str({serial: seen[serial].get("eval_identity") for serial in seen}))
        socket_worst = max(abs(value) for serial in seen
                           for marker in seen[serial]["markers"]
                           for value in (marker["delta"] or [])) if seen else None
        checks.record("socket_max_marker_ndc_delta", socket_worst)
        check("socket marker deltas stay inside tolerance",
              socket_worst is not None and socket_worst <= 1e-6, str(socket_worst))
        socket_stop = driver.stop()
        checks.close("session restores its unrelated connection frame",
                     float(cmds.currentTime(query=True)), socket_initial_time)
        check("socket session removed its camera",
              not cmds.objExists(applier.DEFAULT_CAMERA_NAME), str(socket_stop))
        publisher.stop()
        checks.record("publisher_applied_messages", len(publisher.applied))
        check("the publisher received one applied report per frame sent",
              len(publisher.applied) == args.frames * 2,
              "{0} reports for {1} frames".format(len(publisher.applied), args.frames))
        cmds.currentTime(fixture["maya_start_frame"])

        # ------------------------------------------------- lost connection
        report["phase"] = "transport"
        text = mock.build_fixture(scene_fps=24.0, maya_start_frame=1001.0,
                                  frames_to_apply=1, port=0)
        publisher = mock.MockPublisher(text, host="127.0.0.1", port=0)
        port = publisher.start()
        survivor = applier.CameraSyncFollower(port=port)
        survivor.connect()
        deadline = time.time() + 20.0
        while time.time() < deadline and survivor.frames_applied < 1:
            survivor.pump()
            time.sleep(0.002)
        check("the transport case applied its frame", survivor.frames_applied >= 1,
              str(survivor.frames_applied))
        publisher.stop()
        deadline = time.time() + 20.0
        while time.time() < deadline and survivor.state == applier.STATE_FOLLOWING:
            survivor.pump()
            time.sleep(0.005)
        check("a lost connection is reported",
              survivor.state == applier.STATE_FAILED and
              applier.CATEGORY_TRANSPORT in str(survivor.error),
              "{0}: {1}".format(survivor.state, survivor.error))
        checks.record("transport_error", survivor.error)
        transport_stop = survivor.stop()
        check("Maya stays usable after a lost connection",
              cmds.objExists(cmds.createNode("transform", name="AfterLoss")) and
              abs(float(cmds.currentTime(query=True)) - fixture["maya_start_frame"]) <= 1e-9,
              str(transport_stop))

        # ------------------------------------------------------- render check
        if args.render:
            report["phase"] = "render"
            render = load_module("maya_render_marker_check",
                                 HERE / "maya_render_marker_check.py")
            evidence = args.render_dir or str(
                Path(tempfile.gettempdir()) / "mtou_camera_sync_render_evidence")
            configurations = _render_configurations(render, mapping, fixture)
            result = render.run_render_check(configurations, evidence, tolerance=2.0)
            report["render"] = result
            check("the rendered framing check ran",
                  result.get("available") is True, str(result.get("reason")))
            for entry in result.get("configurations", []):
                check("render {0} matched Maya's projection".format(entry["name"]),
                      entry.get("passed") is True,
                      "max pixel delta {0}; errors {1}".format(
                          entry.get("max_pixel_delta_maya"), entry.get("errors")))

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
        Path(args.result).write_text(
            json.dumps(report, indent=2, ensure_ascii=True, default=str), encoding="utf-8")
        try:
            import maya.standalone
            maya.standalone.uninitialize()
        except Exception:
            pass
    print("checks: {0}, failures: {1}".format(len(checks.results), len(checks.failures)))
    for failure in checks.failures:
        print("FAILED " + failure)
    return 0 if report["ok"] else 1


def _refuses_seconds(cmds, applier):
    """The follower refuses a scene whose time unit is not a frame rate."""
    previous = cmds.currentUnit(query=True, time=True)
    try:
        cmds.currentUnit(time="sec")
        try:
            applier.scene_time_unit()
            return False
        except applier.SyncRefused as error:
            return error.category == applier.CATEGORY_UNSUPPORTED_TIME_UNIT
    finally:
        cmds.currentUnit(time=previous)


def _render_configurations(render, mapping, fixture):
    """The fixture payload plus one configuration per remaining film fit."""
    session = fixture["session"]
    resolution = (session["output_resolution"]["x"], session["output_resolution"]["y"])
    frame = fixture["frames"][0]
    configurations = [render.configuration_from_payload(
        "fixture_horizontal", frame["camera"], frame["markers"], resolution, session, frame)]
    extras = [
        # Fill with a gate narrower than the 2.39:1 sensor keeps the vertical
        # aperture; a gate wider than the sensor keeps the horizontal one.
        ("fill_narrow_gate", None, (1600, 1200), 0),
        ("fill_wide_gate", None, (2560, 720), 0),
        ("vertical_fit", "MaintainYFOV", (1920, 1080), 2),
    ]
    for name, constraint, extra_resolution, expected_fit in extras:
        camera = json.loads(json.dumps(frame["camera"]))
        camera["aspect_axis_constraint"] = constraint
        config = render.configuration_from_payload(
            name, camera, frame["markers"], extra_resolution, session, frame,
            compare_unreal_ndc=False)
        if config["film_fit"] != expected_fit:
            raise AssertionError("{0}: film fit {1}, expected {2}".format(
                name, config["film_fit"], expected_fit))
        configurations.append(config)
    return configurations


if __name__ == "__main__":
    raise SystemExit(main())
