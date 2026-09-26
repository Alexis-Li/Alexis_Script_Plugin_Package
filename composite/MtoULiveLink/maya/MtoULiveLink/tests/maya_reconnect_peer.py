"""Opt-in real Maya peer for MtoULiveLink.Source.MayaCacheReconnect.

Run with Maya's mayapy; --fixture and --result are supplied by the Unreal
Automation test. Uses a disposable skinned scene, real controller, sender,
cache files and transport. No production scene or preferences are saved.
"""
import argparse
import hashlib
import importlib.util
import json
import socket
import time
import traceback
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--result", required=True)
    args = parser.parse_args()
    result = {"ok": False, "sessions": []}
    import maya.standalone
    maya.standalone.initialize(name="python")
    controller = None
    try:
        import maya.cmds as cmds
        fixture = json.loads(Path(args.fixture).read_text(encoding="utf-8-sig"))
        script = Path(__file__).resolve().parents[1] / "scripts" / "MtoULiveLink.py"
        spec = importlib.util.spec_from_file_location("MtoUReconnectHost", str(script))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.PORT = fixture["port"]
        result["maya_version"] = cmds.about(version=True)
        result["maya_api"] = cmds.about(apiVersion=True)
        result["runtime_sha256"] = hashlib.sha256(script.read_bytes()).hexdigest()
        cmds.file(new=True, force=True)
        cmds.currentUnit(linear="cm", time="ntsc")
        group = cmds.createNode("transform", name="ReconnectCharacter")
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
        for frame, value in enumerate((10, 20, 30, 40), 1):
            cmds.setKeyframe(joints[0], attribute="translateX", time=frame, value=value)
        cmds.playbackOptions(minTime=1, maxTime=4)
        cmds.currentTime(1)

        class HeadlessController(module._Controller):
            # Presentation only: retain the real lifecycle and error transition.
            def _show_error(self, diagnostic):
                result.setdefault("diagnostics", []).append(diagnostic)

        controller = HeadlessController()
        controller._scene = module._CharacterScene.capture(joints[0], display, "clothes")

        def pump_until(predicate, label, timeout=20):
            deadline = time.time() + timeout
            while time.time() < deadline:
                session = controller._session
                cached = controller._cached_playback
                if session is not None and (cached is None or cached._closed):
                    session._sample_and_submit()
                cached = controller._cached_playback
                if cached is not None:
                    # mayapy has no UI idle loop. Invoke the same bounded timer
                    # callbacks on Maya's main thread, with real Maya sampling.
                    if cached.view.state == cached.CAPTURING:
                        cached._capture_step()
                    cached._poll()
                if predicate():
                    return
                time.sleep(0.002)
            raise AssertionError("Timed out: {0}; diagnostics={1}".format(
                label, result.get("diagnostics")))

        def connect():
            controller.connect()
            pump_until(lambda: controller._cached_playback is not None
                       and not controller._cached_playback._closed, "ready")
            controller._on_mode_changed(module.CACHED_MODE)
            return controller._cached_playback

        def announce(phase):
            result["phase"] = phase
            Path(args.result).write_text(
                json.dumps(result, indent=2, ensure_ascii=True), encoding="utf-8")

        first = connect()
        controller._capture_cached_playback()
        pump_until(lambda: first.view.state == first.READY, "A upload ready")
        announce("a_ready")
        pump_until(lambda: first.view.state == first.COMPLETED, "A capture/play complete")
        cache_path = Path(first._cache._frames_path)
        cache_digest = hashlib.sha256(cache_path.read_bytes()).hexdigest()
        cache_summary = first.view.cache_summary._values()
        result["sessions"].append({"name": "A", "upload_id": first._upload_id,
                                   "play_id": first._play_id, "applied": first.view.current})
        assert first._upload_id == first._play_id == 1
        assert first.view.current == 4
        # Actual socket failure, not a fabricated terminal event. The cached
        # poller detects it and the controller retains the compatible cache.
        controller._session._worker._socket.shutdown(socket.SHUT_RDWR)
        pump_until(lambda: controller._session is None, "transport-loss detach")
        assert controller._cached_retention is not None
        assert cache_path.exists()
        second = connect()
        assert second is not first
        assert second.view.cache_summary._values() == cache_summary
        assert Path(second._cache._frames_path) == cache_path
        # A different current animation makes an accidental recapture visible.
        cmds.setKeyframe(joints[0], attribute="translateX", time=4, value=999)
        controller._upload_retained_cache()
        pump_until(lambda: second.view.state == second.READY, "B retained upload ready")
        announce("b_ready")
        pump_until(lambda: second.view.state == second.COMPLETED,
                   "B retained-cache replay complete")
        assert hashlib.sha256(cache_path.read_bytes()).hexdigest() == cache_digest
        assert second._upload_id == second._play_id == 1
        assert second.view.current == 4
        result["sessions"].append({"name": "B", "upload_id": second._upload_id,
                                   "play_id": second._play_id, "applied": second.view.current})
        result["same_completed_cache"] = True
        result["cache_sha256"] = cache_digest
        # Recapture on the connected session exercises clear acknowledgement
        # and the enabled custom range, including a negative start frame.
        cmds.setKeyframe(joints[0], attribute="translateX", time=-1, value=-10)
        cmds.setKeyframe(joints[0], attribute="translateX", time=0, value=0)
        controller._selected_capture_range = lambda: (-1, 1)
        controller._capture_cached_playback()
        pump_until(lambda: second.view.state == second.READY, "C custom upload ready")
        assert second.view.cache_summary.capture_start == -1
        assert second.view.cache_summary.capture_end == 1
        assert second.view.cache_summary.frame_count == 3
        assert not cache_path.exists()
        custom_path = Path(second._cache._frames_path)
        announce("c_ready")
        pump_until(lambda: second.view.state == second.COMPLETED,
                   "C custom-range replay complete")
        assert second.view.current == 3
        result["sessions"].append({"name": "C", "upload_id": second._upload_id,
                                   "play_id": second._play_id, "applied": second.view.current,
                                   "range": [-1, 1]})
        result["capture_calls"] = 2
        controller.close()
        controller = None
        assert not custom_path.exists()
        result["cache_cleaned"] = True
        result["ok"] = True
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
