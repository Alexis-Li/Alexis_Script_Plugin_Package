"""Opt-in real Maya peer for MtoULiveLink.Source.MayaInteractivePlayback.

Run with Maya's mayapy; --fixture and --result are supplied by the Unreal
Automation test. Uses a disposable skinned scene, the real controller, capture,
upload, cache files and transport, records every cached-playback outcome Unreal
sent, and leaves through the real cached/realtime boundary. No production scene
or preferences are saved.
"""
import argparse
import hashlib
import importlib.util
import json
import socket
import time
import traceback
from pathlib import Path

# The disposable animation: one keyframed value per captured frame so an
# applied pose names its own source frame in the Unreal evaluation.
FIRST_FRAME = 1
LAST_FRAME = 24
VALUE_PER_FRAME = 10


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--result", required=True)
    args = parser.parse_args()
    result = {"ok": False, "outcomes": [], "view_snapshots": []}
    import maya.standalone
    maya.standalone.initialize(name="python")
    controller = None
    try:
        import maya.cmds as cmds
        fixture = json.loads(Path(args.fixture).read_text(encoding="utf-8-sig"))
        script = Path(__file__).resolve().parents[1] / "scripts" / "MtoULiveLink.py"
        spec = importlib.util.spec_from_file_location("MtoUInteractiveHost", str(script))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.PORT = fixture["port"]
        result["maya_version"] = cmds.about(version=True)
        result["maya_api"] = cmds.about(apiVersion=True)
        result["runtime_sha256"] = hashlib.sha256(script.read_bytes()).hexdigest()
        result["protocol_version"] = module.PROTOCOL_VERSION
        cmds.file(new=True, force=True)
        cmds.currentUnit(linear="cm", time="ntsc")
        group = cmds.createNode("transform", name="InteractiveCharacter")
        display = cmds.createNode("transform", name="Display_ctrl", parent=group)
        cmds.addAttr(display, longName="clothes", attributeType="enum", enumName="Clothes01")
        joints = []
        for name, parent in fixture["bones"]:
            cmds.select(clear=True)
            joint = cmds.joint(name=name)
            cmds.parent(joint, joints[parent] if parent >= 0 else group)
            joints.append(cmds.ls(joint, long=True)[0])
        mesh = cmds.polyCube(name="Clothes01")[0]
        cmds.parent(mesh, group)
        cmds.skinCluster(joints, mesh, toSelectedBones=True)
        for frame in range(FIRST_FRAME, LAST_FRAME + 1):
            cmds.setKeyframe(joints[0], attribute="translateX", time=frame,
                             value=frame * VALUE_PER_FRAME)
        cmds.playbackOptions(minTime=FIRST_FRAME, maxTime=LAST_FRAME)
        cmds.currentTime(FIRST_FRAME)

        class HeadlessController(module._Controller):
            # Presentation only: retain the real lifecycle and error transition.
            def _show_error(self, diagnostic):
                result.setdefault("diagnostics", []).append(diagnostic)

        controller = HeadlessController()
        controller._scene = module._CharacterScene.capture(joints[0], display, "clothes")
        # Every user-visible status the cached workflow publishes, so the
        # Unreal side can verify Maya's own wording per interactive state.
        presentation = []
        set_text = controller._set_text
        set_connected = controller._set_connected

        def record_set_text(control, text):
            presentation.append(text)
            return set_text(control, text)

        def record_set_connected(connected, status):
            presentation.append(status)
            return set_connected(connected, status)

        controller._set_text = record_set_text
        controller._set_connected = record_set_connected

        def pump_until(predicate, label, timeout=120):
            deadline = time.time() + timeout
            while time.time() < deadline:
                session = controller._session
                cached = controller._cached_playback
                # Real-time sampling also observes the transport: outside cached
                # playback it is what notices a socket that died.
                if session is not None and not session._paused_for_cached:
                    session._sample_and_submit()
                cached = controller._cached_playback
                if cached is not None and not cached._closed:
                    # mayapy has no UI idle loop. Invoke the same bounded timer
                    # callbacks on Maya's main thread, with real Maya sampling.
                    if cached.view.state == cached.CAPTURING:
                        cached._capture_step()
                    cached._poll()
                if predicate():
                    return
                time.sleep(0.002)
            cached = controller._cached_playback
            session = controller._session
            raise AssertionError("Timed out: {0}; diagnostics={1}; phase={2};"
                                 " view={3}; applied={4}; session_ready={5}".format(
                                     label, result.get("diagnostics"),
                                     None if cached is None else cached._phase,
                                     None if cached is None else cached.view.state,
                                     None if cached is None else cached.view.current,
                                     None if session is None else session.is_ready))

        def connect():
            controller.connect()
            pump_until(lambda: controller._cached_playback is not None
                       and not controller._cached_playback._closed, "ready")
            controller._on_mode_changed(module.CACHED_MODE)
            return controller._cached_playback

        def record_outcomes(cached, name):
            """Records the real outcomes the cached session routes, and the
            Maya view each one produced.

            Outcomes stay in memory: the Unreal side polls the result file, so
            it is only written at the phase boundaries the test waits for.
            """
            original_route = cached._route_outcome

            def route(reply):
                original_route(reply)
                result["outcomes"].append(dict(reply, session=name))
                view = cached.view
                result["view_snapshots"].append({
                    "type": reply.get("type"),
                    "state": view.state,
                    "current": view.current,
                    "total": view.total,
                    "source_frame": view.source_frame,
                    "loop_round": view.loop_round,
                    "loop_enabled": view.loop_enabled,
                    "loop": reply.get("loop"),
                })

            cached._route_outcome = route

        def announce(phase):
            result["phase"] = phase
            payload = json.dumps(result, indent=2, ensure_ascii=True)
            target = Path(args.result)
            # The Unreal test polls this file, so a write can collide with its
            # reader; retry briefly instead of failing the whole session.
            for attempt in range(50):
                try:
                    target.write_text(payload, encoding="utf-8")
                    return
                except OSError:
                    time.sleep(0.02)
            raise AssertionError("Cannot write the integration result file")

        def seen(outcome_type, **fields):
            for outcome in result["outcomes"]:
                if outcome.get("type") != outcome_type:
                    continue
                if all(outcome.get(key) == value for key, value in fields.items()):
                    return True
            return False

        def seen_count(outcome_type):
            return sum(1 for outcome in result["outcomes"]
                       if outcome.get("type") == outcome_type)

        # Session A: capture and upload the whole range, then serve every
        # interactive control the Unreal test drives.
        first = connect()
        record_outcomes(first, "A")
        controller._capture_cached_playback()
        # The outcome record is latched: Unreal drives the interactive sequence
        # as soon as its own view is Ready, so a transient view state is not a
        # reliable rendezvous here.
        pump_until(lambda: seen("cache_ready", session="A"), "A upload ready")
        cache_path = Path(first._cache._frames_path)
        cache_digest = hashlib.sha256(cache_path.read_bytes()).hexdigest()
        result["sessions"] = [{
            "name": "A",
            "upload_id": first._upload_id,
            "range": [first.view.cache_summary.capture_start,
                      first.view.cache_summary.capture_end],
            "frame_count": first.view.cache_summary.frame_count,
        }]
        announce("a_ready")

        pump_until(lambda: seen("cache_paused"), "A pause outcome")
        pump_until(lambda: seen("cache_resumed"), "A resume outcome")
        pump_until(lambda: seen_count("cache_seeked") >= 3, "A seek outcomes")
        pump_until(lambda: seen("cache_looped", round=1)
                   and seen("cache_looped", round=2), "A two loop rounds")
        # Completion and stop are independent: either may arrive first.
        pump_until(lambda: seen("cache_complete"), "A completion outcome")
        pump_until(lambda: seen("cache_stopped"), "A stop outcome")
        announce("a_interactive_done")
        result["presentation"] = presentation
        result["outcome_types"] = [outcome.get("type") for outcome in result["outcomes"]]
        # Leaving cached playback clears Unreal's cache and resumes live
        # frames, while the completed local cache stays for a later upload.
        controller._on_mode_changed(module.REALTIME_MODE)
        pump_until(lambda: first.view.state == first.REALTIME, "A leave")
        result["cache_retained_after_leave"] = cache_path.exists()
        announce("a_left")

        # Session B: a real transport loss, then a fresh negotiation that
        # captures and reaches Ready again before leaving once more.
        controller._session._worker._socket.shutdown(socket.SHUT_RDWR)
        pump_until(lambda: controller._session is None, "transport loss")
        replay_diagnostics = len(result.get("diagnostics", []))
        second = connect()
        record_outcomes(second, "B")
        controller._capture_cached_playback()
        pump_until(lambda: seen("cache_ready", session="B"), "B upload ready")
        result["sessions"].append({
            "name": "B",
            "upload_id": second._upload_id,
            "range": [second.view.cache_summary.capture_start,
                      second.view.cache_summary.capture_end],
            "frame_count": second.view.cache_summary.frame_count,
        })
        announce("b_ready")
        # Unreal plays the reconnected cache so the leave below follows a
        # real attempt instead of racing its upload.
        pump_until(lambda: seen("cache_complete", session="B")
                   or seen("cache_stopped", session="B"), "B playback outcome")
        second_path = Path(second._cache._frames_path)
        controller._on_mode_changed(module.REALTIME_MODE)
        pump_until(lambda: second.view.state == second.REALTIME, "B leave")
        announce("b_left")
        controller.close()
        controller = None
        result["reconnect_diagnostics"] = result.get("diagnostics", [])[replay_diagnostics:]
        result["caches_cleaned"] = not cache_path.exists() and not second_path.exists()
        result["cache_sha256"] = cache_digest
        result["ok"] = True
        announce("done")
    except Exception:
        result["error"] = traceback.format_exc()
    finally:
        if controller is not None:
            controller.close()
        Path(args.result).write_text(
            json.dumps(result, indent=2, ensure_ascii=True), encoding="utf-8")
        maya.standalone.uninitialize()
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
