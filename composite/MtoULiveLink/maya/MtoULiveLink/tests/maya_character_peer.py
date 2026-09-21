"""Opt-in character peer for the Unreal CharacterAcceptance test.

Reads a supplied scene without saving it. Fixture paths and pose edits are
external data, never hard-coded character names. Edits affect this disposable
Maya process only. Uses the production capture/controller/socket lifecycle.
"""
import argparse
import importlib.util
import json
import time
import traceback
from pathlib import Path
from unittest import mock


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--result", required=True)
    args = parser.parse_args()
    fixture = read_json(args.fixture)
    result = {"id": -1, "ok": False, "phase": "loading"}
    write_json(args.result, result)
    import maya.standalone
    maya.standalone.initialize(name="python")
    controller = None
    try:
        import maya.cmds as cmds
        script = Path(__file__).resolve().parents[1] / "scripts" / "MtoULiveLink.py"
        spec = importlib.util.spec_from_file_location("MtoUCharacterPeer", str(script))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.PORT = fixture["port"]
        cmds.file(fixture["scene"], open=True, force=True, prompt=False,
                  executeScriptNodes=False, ignoreVersion=True)

        class Controller(module._Controller):
            def _show_error(self, diagnostic):
                result.setdefault("diagnostics", []).append(diagnostic)

        controller = Controller()
        controller._scene = module._CharacterScene.capture(fixture["root"])
        scene = controller._scene
        snapshot = scene.snapshot()
        result.update(maya_version=cmds.about(version=True),
                      maya_api=cmds.about(apiVersion=True),
                      bones=list(snapshot.bones),
                      paths=[b["path"] for b in scene._subject["bones"]],
                      bind=list(snapshot.bind_local_transforms))
        frame_range = fixture.get("frame_range")
        if frame_range:
            cmds.playbackOptions(minTime=float(frame_range[0]),
                                 maxTime=float(frame_range[1]))
            cmds.currentTime(float(frame_range[0]))
        alias_plugs = {}
        for mesh in scene._subject["meshes"]:
            history = cmds.listHistory(mesh, pruneDagObjects=True) or []
            for node in history:
                if cmds.nodeType(node) != "blendShape":
                    continue
                pairs = cmds.aliasAttr(node, query=True) or []
                for alias, attribute in zip(pairs[0::2], pairs[1::2]):
                    alias_plugs.setdefault(alias, node + "." + attribute)

        base = {}

        def detach(plug):
            """Make one authored plug writable in this disposable process."""
            if plug not in base:
                base[plug] = cmds.getAttr(plug)
            # Test controls and facial weights can be driven or locked. Detach
            # only here; the supplied scene is never saved.
            cmds.setAttr(plug, lock=False)
            for source in cmds.listConnections(
                    plug, source=True, destination=False, plugs=True) or []:
                cmds.disconnectAttr(source, plug)

        for pose in fixture["poses"]:
            for plug in pose.get("edits", {}):
                detach(plug)

        def pump():
            session = controller._session
            cached = controller._cached_playback
            # Live sampling belongs to real-time mode; cached mode is driven by
            # the cache callbacks below.
            if session is not None and controller._mode == module.REALTIME_MODE:
                session._sample_and_submit(force=True)
            cached = controller._cached_playback
            if cached is not None:
                # mayapy has no UI idle loop. Invoke the same bounded timer
                # callbacks on Maya's main thread, with real Maya sampling.
                if cached.view.state == cached.CAPTURING:
                    cached._capture_step()
                cached._poll()
            time.sleep(0.002)

        def until(predicate, label, timeout=60):
            deadline = time.time() + timeout
            while time.time() < deadline:
                pump()
                if predicate():
                    return
            raise AssertionError("Timed out: " + label + "; " + str(result.get("diagnostics")))

        last_id = -1
        result.update(phase="loaded", ok=True)
        write_json(args.result, result)
        # Disable only warning-dialog presentation in standalone Maya.
        with mock.patch.object(module.cmds, "checkBox", return_value=False):
            deadline = time.time() + 900
            while time.time() < deadline:
                command = read_json(fixture["command"])
                if command and command["id"] != last_id:
                    last_id = command["id"]
                    action = command["action"]
                    if action == "connect":
                        controller._workflow = command["workflow"]
                        controller._blendshapes_enabled = True
                        controller.connect()
                        until(lambda: controller._session is not None and controller._session._phase == "ready", "ready")
                        result["warning"] = controller._last_warning
                        result["phase"] = "connected"
                    elif action == "pose":
                        pose = fixture["poses"][command["pose"]]
                        # Sample the composed character after the edit so the
                        # next streamed frame carries the posed values.
                        for plug, value in base.items():
                            cmds.setAttr(plug, value)
                        for plug, delta in pose.get("edits", {}).items():
                            cmds.setAttr(plug, base[plug] + delta)
                        for alias, value in pose.get("alias_edits", {}).items():
                            plug = alias_plugs[alias]
                            detach(plug)
                            cmds.setAttr(plug, base[plug] + value)
                        # Only the edited plugs are dirtied: a production scene
                        # re-evaluates the rest of the graph on read.
                        result["pose"] = command["pose"]
                        result["phase"] = "posed"
                    elif action == "alias":
                        # Drive one BlendShape alias that the Unreal side asked
                        # for, resolved from the character's own libraries.
                        name = command["alias"]
                        plug = alias_plugs[name]
                        detach(plug)
                        cmds.setAttr(plug, base[plug] + float(command["value"]))
                        result["alias"] = name
                        result["alias_value"] = float(command["value"])
                        result["phase"] = "alias"
                    elif action == "cache_play":
                        # Production path: enter cached mode, capture the
                        # Playback Range, upload it, and replay locally.
                        result.pop("cache_error", None)
                        controller._on_mode_changed(module.CACHED_MODE)
                        cached = controller._ensure_cached_playback()
                        started = time.time()
                        controller._capture_cached_playback()
                        until(lambda: cached.view.state in (
                            cached.COMPLETED, cached.FAILED, cached.REALTIME), "cache play")
                        result["cache_capture_seconds"] = time.time() - started
                        result["cache_summary"] = cached.view.cache_summary._values()
                        result["cache_applied"] = cached.view.current
                        result["cache_state"] = cached.view.state
                        result["cache_upload_id"] = cached._upload_id
                        result["cache_play_id"] = cached._play_id
                        result["phase"] = "cached"
                    elif action == "cache_replay":
                        cached = controller._cached_playback
                        started = time.time()
                        controller._replay_cached_playback()
                        until(lambda: cached.view.state in (
                            cached.COMPLETED, cached.FAILED), "cache replay")
                        result["cache_replay_seconds"] = time.time() - started
                        result["cache_applied"] = cached.view.current
                        result["cache_state"] = cached.view.state
                        result["phase"] = "replayed"
                    elif action == "cache_stop":
                        controller._stop_cached_replay()
                        result["cache_state"] = controller._cached_playback.view.state
                        result["phase"] = "stopped"
                    elif action == "cache_live":
                        controller._on_mode_changed(module.REALTIME_MODE)
                        until(lambda: controller._cached_playback is None
                              or controller._cached_playback.view.state
                              == controller._cached_playback.REALTIME, "cache live")
                        result["phase"] = "realtime"
                    elif action == "reconnect":
                        controller.disconnect()
                        until(lambda: controller._session is None, "reconnect disconnect")
                        controller.connect()
                        until(
                            lambda: controller._session is not None
                            and controller._session._phase == "ready",
                            "reconnect ready",
                        )
                        result["warning"] = controller._last_warning
                        result["phase"] = "connected"
                    elif action == "disconnected":
                        until(lambda: controller._session is None, "refresh disconnect")
                        until(lambda: True, "pump")
                        result["phase"] = "disconnected"
                    elif action == "stop":
                        result.update(id=last_id, phase="stopped", ok=True)
                        write_json(args.result, result)
                        break
                    result.update(id=last_id, warning=controller._last_warning,
                                  transforms=list(scene.sample().transforms), ok=True)
                    write_json(args.result, result)
                pump()
            else:
                raise AssertionError("Character acceptance command timeout")
    except Exception:
        result.update(ok=False, error=traceback.format_exc())
        write_json(args.result, result)
    finally:
        if controller is not None:
            controller.close()
        maya.standalone.uninitialize()
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
