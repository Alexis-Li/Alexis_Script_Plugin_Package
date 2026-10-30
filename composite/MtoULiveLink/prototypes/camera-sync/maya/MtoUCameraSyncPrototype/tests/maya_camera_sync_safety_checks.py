"""Real Maya regressions for Issue 52's public exit and borrowing contracts."""

import copy
import json
import socket
import threading
from unittest import mock as unittest_mock


def run_checks(cmds, sync, publisher, checks):
    fixture = publisher.build_fixture(frames_to_apply=1, port=0)
    frame = publisher.frame_message(fixture, fixture["frames"][0])
    session = publisher.session_message(fixture, 0)

    def fresh():
        cmds.file(new=True, force=True)
        cmds.currentUnit(linear="cm", time="film")
        cmds.currentTime(17)
        cmds.undoInfo(state=True)
        for attr, value in (("width", 800), ("height", 600),
                            ("pixelAspect", 1), ("deviceAspectRatio", 4.0 / 3.0)):
            cmds.setAttr("defaultResolution." + attr, value)

    def snapshot():
        return {"time": cmds.currentTime(query=True),
                "undo": cmds.undoInfo(query=True, state=True),
                "gate": {attr: cmds.getAttr("defaultResolution." + attr)
                         for attr in ("width", "height", "pixelAspect", "deviceAspectRatio")},
                "nodes": sorted(cmds.ls())}

    def follower(name):
        result = sync.CameraSyncFollower(camera_name=name)
        result._scene_fps = 24.0
        result._start_time = cmds.currentTime(query=True)
        result.session = copy.deepcopy(session)
        return result

    # Run the public entry point against a real local TCP peer. The peer closes
    # only after receiving a report, so this covers loss after an actual write.
    for mode in ("disconnect", "bad_handshake", "attach_failure", "callback_failure"):
        fresh()
        cmds.undoInfo(state=False)
        before = snapshot()
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(5)
        port = listener.getsockname()[1]
        received = []

        def serve():
            try:
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(5)
                    stream = connection.makefile("rb")
                    try:
                        stream.readline()  # hello
                        if mode == "bad_handshake":
                            connection.sendall(b'{"type":"session"}\n')
                        else:
                            connection.sendall((json.dumps(session) + "\n").encode("utf-8"))
                            connection.sendall((json.dumps(frame) + "\n").encode("utf-8"))
                            received.append(json.loads(stream.readline()))
                    finally:
                        stream.close()
            except (OSError, ValueError) as error:
                received.append({"peer_error": str(error)})
            finally:
                listener.close()

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        instances = []
        original_class = sync.CameraSyncFollower

        def capture(*args, **kwargs):
            result = original_class(*args, **kwargs)
            instances.append(result)
            return result

        def fail_callback(_):
            raise RuntimeError("injected callback failure")

        exception = None
        try:
            with unittest_mock.patch.object(sync, "CameraSyncFollower", side_effect=capture):
                if mode == "attach_failure":
                    with unittest_mock.patch.object(original_class, "attach_idle_pump",
                                                    side_effect=RuntimeError("injected attach failure")):
                        sync.run(port=port, duration=2, idle_pump=True)
                else:
                    sync.run(port=port, duration=2,
                             on_frame=fail_callback if mode == "callback_failure" else None)
        except Exception as error:
            exception = str(error)
        thread.join(6)
        after = snapshot()
        instance = instances[0]
        checks.record("safety_" + mode, {"before": before, "after": after,
                      "summary": instance.summary(), "exception": exception,
                      "peer_reports": received})
        checks.check(mode + " public run restores scene and undo", before == after)
        checks.check(mode + " releases socket and callback",
                     not instance.connected and instance._script_job is None)
        if mode == "disconnect":
            checks.check("disconnect follows a real applied frame",
                         any(item.get("status") == "applied" for item in received))
            checks.check("disconnect retains original transport failure",
                         instance.state == sync.STATE_FAILED and
                         sync.CATEGORY_TRANSPORT in str(instance.error))
        else:
            checks.check(mode + " preserves the original exception", exception is not None)
        # Assert idempotence only after capturing the unassisted public result.
        instance.stop()
        stopped = snapshot()
        instance.stop()
        checks.check(mode + " repeated stop has no scene side effects", snapshot() == stopped)

    # A bound, non-listening socket deterministically refuses connection.
    for entry in ("connect", "run"):
        fresh()
        cmds.undoInfo(state=False)
        before = snapshot()
        with socket.socket() as closed_listener:
            closed_listener.bind(("127.0.0.1", 0))
            port = closed_listener.getsockname()[1]
            try:
                if entry == "run":
                    sync.run(port=port)
                else:
                    sync.CameraSyncFollower(port=port).connect()
            except sync.SyncRefused as error:
                checks.check(entry + " refuses connection with transport category",
                             error.category == sync.CATEGORY_TRANSPORT)
            else:
                checks.check(entry + " refuses connection", False)
        checks.check(entry + " refusal restores scene and undo", snapshot() == before)

    # Direct connect callers also own no partially initialized session after a
    # refused handshake or hello send error, even without a surrounding run().
    for mode in ("hello_send", "handshake_refused"):
        fresh()
        cmds.undoInfo(state=False)
        before = snapshot()
        instance = sync.CameraSyncFollower(connect_timeout=0.1)
        transport = unittest_mock.Mock()
        with unittest_mock.patch.object(sync.socket, "create_connection", return_value=transport):
            with unittest_mock.patch.object(instance, "_send",
                                            side_effect=OSError("injected hello failure") if mode == "hello_send" else None):
                with unittest_mock.patch.object(instance, "_receive", return_value=[{
                        "type": "error", "category": "SESSION_BUSY", "detail": "one client"}]):
                    try:
                        instance.connect()
                    except sync.SyncRefused as error:
                        checks.check(mode + " preserves the classified failure",
                                     error.category == (sync.CATEGORY_TRANSPORT if mode == "hello_send"
                                                        else sync.CATEGORY_HANDSHAKE))
                    else:
                        checks.check(mode + " is refused", False)
        checks.check(mode + " direct connect restores undo and releases transport",
                     snapshot() == before and not instance.connected and transport.close.called)

    # Animation, constraints and ancestor drivers must be refused before any
    # frame writes; preserve both values and graph connections across stop.
    for mode in ("animation", "constraint", "parent_animation"):
        fresh()
        transform, shape = cmds.camera(name="SafetyBorrowed")
        if mode == "animation":
            for time, position, focal in ((1, 10, 28), (30, 90, 70)):
                cmds.setKeyframe(transform, attribute="translateX", time=time, value=position)
                cmds.setKeyframe(shape, attribute="focalLength", time=time, value=focal)
        elif mode == "constraint":
            target = cmds.createNode("transform", name="SafetyTarget")
            cmds.parentConstraint(target, transform)
        else:
            parent = cmds.createNode("transform", name="SafetyParent")
            cmds.parent(transform, parent)
            cmds.setKeyframe(parent, attribute="translateX", time=1, value=10)
            cmds.setKeyframe(parent, attribute="translateX", time=30, value=90)
        cmds.currentTime(17)
        instance = follower(transform)

        def driven_snapshot():
            return {"scene": snapshot(), "matrix": cmds.xform(transform, q=True, ws=True, matrix=True),
                    "focal": cmds.getAttr(shape + ".focalLength"),
                    "keys": cmds.keyframe(q=True, valueChange=True),
                    "connections": sorted(cmds.listConnections(
                        transform, source=True, destination=False, plugs=True) or [])}

        before = driven_snapshot()
        report = instance.apply_frame(copy.deepcopy(frame))
        after_apply = driven_snapshot()
        instance.stop()
        checks.record("safety_" + mode, report)
        checks.check(mode + " camera is classified as rejected",
                     report["status"] == "rejected" and
                     "CAMERA_INPUT_DRIVEN" in str(report["detail"]))
        checks.check(mode + " refusal leaves scene and animation intact",
                     before == after_apply == driven_snapshot())
        checks.check(mode + " refusal does not report false projection success", report["markers"] == [])

    for plug_kind in ("focal", "transform", "resolution", "compound"):
        fresh()
        transform, shape = cmds.camera(name="SafetyLocked")
        plug = {"focal": shape + ".focalLength", "transform": transform + ".translateX",
                "compound": transform + ".translate", "resolution": "defaultResolution.width"}[plug_kind]
        cmds.setAttr(plug, lock=True)
        instance = follower(transform)
        before = snapshot()
        matrix_before = cmds.xform(transform, q=True, ws=True, matrix=True)
        focal_before = cmds.getAttr(shape + ".focalLength")
        report = instance.apply_frame(copy.deepcopy(frame))
        checks.check(plug_kind + " lock is refused before frame writes",
                     report["status"] == "rejected" and "LOCKED_SYNC_ATTRIBUTE" in str(report["detail"]) and
                     snapshot() == before and cmds.getAttr(plug, lock=True))
        instance.stop()
        checks.check(plug_kind + " lock and values survive stop",
                     cmds.getAttr(plug, lock=True) and snapshot() == before and
                     matrix_before == cmds.xform(transform, q=True, ws=True, matrix=True) and
                     focal_before == cmds.getAttr(shape + ".focalLength"))
        cmds.setAttr(plug, lock=False)

    # A heartbeat doesn't write; perturb the supported camera afterward and prove
    # projection evidence follows the host instead of the requested values.
    fresh()
    instance = follower("SafetyReadback")
    first = instance.apply_frame(copy.deepcopy(frame))
    transform, shape = instance.camera_nodes
    cmds.setAttr(shape + ".focalLength", float(first["camera"]["focalLength"]) * 2)
    report = instance.apply_frame(copy.deepcopy(frame))
    checks.record("safety_readback", report)
    checks.check("host mismatch is not reported as applied", report["status"] == "rejected")
    checks.check("marker evidence uses actual host focal length",
                 any(item["delta"] and max(abs(value) for value in item["delta"]) > 0.01
                     for item in report["markers"]))
    repaired = instance.apply_frame(copy.deepcopy(frame))
    checks.check("a later heartbeat repairs a stale host state", repaired["status"] == "applied")
    instance.stop()

    # Force a host-side overwrite after real writes and time evaluation. A
    # mismatch must roll back the transaction rather than report false success.
    fresh()
    transform, shape = cmds.camera(name="SafetyRollback")
    instance = follower(transform)
    before = snapshot()
    matrix_before = cmds.xform(transform, q=True, ws=True, matrix=True)
    focal_before = cmds.getAttr(shape + ".focalLength")
    original_read = instance._read_frame_state

    def overwritten(*args, **kwargs):
        cmds.setAttr(shape + ".focalLength", 99)
        return original_read(*args, **kwargs)

    with unittest_mock.patch.object(instance, "_read_frame_state", side_effect=overwritten):
        report = instance.apply_frame(copy.deepcopy(frame))
    checks.record("safety_rollback", report)
    checks.check("post-write mismatch is classified as rejected",
                 report["status"] == "rejected" and "HOST_STATE_MISMATCH" in str(report["detail"]))
    checks.check("post-write mismatch rolls back camera gate and time",
                 snapshot() == before and focal_before == cmds.getAttr(shape + ".focalLength") and
                 matrix_before == cmds.xform(transform, q=True, ws=True, matrix=True))
    instance.stop()

    fresh()
    before = snapshot()
    instance = follower("SafetyIdleError")
    instance.state = sync.STATE_FOLLOWING
    instance.apply_frame(copy.deepcopy(frame))
    with unittest_mock.patch.object(instance, "pump", side_effect=RuntimeError("injected idle failure")):
        instance._idle_tick()
    checks.check("idle callback exception releases scene and keeps failure",
                 snapshot() == before and instance.state == sync.STATE_FAILED and
                 instance.error == "RuntimeError: injected idle failure")

    fresh()
    instance = follower("SafetyLargeTime")
    late_frame = copy.deepcopy(frame)
    late_frame["time"]["display_frame"] = 1000000.0
    report = instance.apply_frame(late_frame)
    checks.check("large-frame target is supported", report["status"] == "applied")
    cmds.currentTime(report["maya_frame"] + 0.25)
    report = instance.apply_frame(late_frame)
    checks.check("subframe mismatch uses an absolute time tolerance at large frames",
                 report["status"] == "rejected" and "maya_time" in str(report["detail"]))
    instance.stop()


def main():
    """Run only these regressions, using the host suite's evidence collector."""
    import argparse
    from pathlib import Path
    from maya_host_camera_tests import Checks
    import maya.standalone
    maya.standalone.initialize(name="python")
    import maya.cmds as cmds
    import MtoUCameraSyncPrototype as sync
    import mock_camera_sync_server as publisher

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--result", required=True)
    args = parser.parse_args()
    checks = Checks()
    try:
        run_checks(cmds, sync, publisher, checks)
        report = {"ok": not checks.failures, "maya_version": cmds.about(version=True),
                  "checks": checks.results, "failures": checks.failures,
                  "evidence": checks.evidence}
        Path(args.result).write_text(json.dumps(report, indent=2), encoding="utf-8")
        print("checks: {0}, failures: {1}".format(len(checks.results), len(checks.failures)))
        return 0 if report["ok"] else 1
    finally:
        maya.standalone.uninitialize()


if __name__ == "__main__":
    raise SystemExit(main())
