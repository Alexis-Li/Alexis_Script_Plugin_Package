"""Real Maya peer for MtoULiveLink.Editor.Preview.MayaRefreshPeer.

Streams Animation through the real sender transport, observes the Unreal-side
explicit-refresh disconnect, proves no automatic reconnect happens, then
reconnects explicitly on the test's command and streams again.

Run with Maya's mayapy; --fixture and --result are supplied by the Unreal
Automation test. Uses a disposable scene, the real controller, sender, and
transport. No production scene or preferences are saved.

Fixture: {"port": int, "bones": [[name, parent], ...], "command": path}.
The test drives phases through the command file: {"action": "stream",
"time": N}, {"action": "reconnect"}, {"action": "stop"}.
The result file is rewritten at every phase so the test can poll it.
"""
import argparse
import hashlib
import importlib.util
import json
import time
import traceback
from pathlib import Path


def read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        return None


def write_json(path, payload):
    Path(path).write_text(
        json.dumps(payload, indent=2, ensure_ascii=True), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--result", required=True)
    args = parser.parse_args()
    result = {"ok": False, "phase": "starting"}
    write_json(args.result, result)
    import maya.standalone
    maya.standalone.initialize(name="python")
    controller = None
    try:
        import maya.cmds as cmds
        fixture = json.loads(Path(args.fixture).read_text(encoding="utf-8-sig"))
        command_path = fixture["command"]
        script = Path(__file__).resolve().parents[1] / "scripts" / "MtoULiveLink.py"
        spec = importlib.util.spec_from_file_location("MtoURefreshHost", str(script))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.PORT = fixture["port"]
        result["maya_version"] = cmds.about(version=True)
        result["maya_api"] = cmds.about(apiVersion=True)
        result["runtime_sha256"] = hashlib.sha256(script.read_bytes()).hexdigest()
        cmds.file(new=True, force=True)
        cmds.currentUnit(linear="cm", time="ntsc")
        group = cmds.createNode("transform", name="RefreshCharacter")
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
        for frame, value in enumerate((10, 20, 30), 1):
            cmds.setKeyframe(joints[0], attribute="translateX", time=frame, value=value)
        cmds.playbackOptions(minTime=1, maxTime=3)
        cmds.currentTime(1)

        class HeadlessController(module._Controller):
            # Presentation only: retain the real lifecycle and error transition.
            def _show_error(self, diagnostic):
                result.setdefault("diagnostics", []).append(diagnostic)

        controller = HeadlessController()
        controller._scene = module._CharacterScene.capture(joints[0], display, "clothes")

        def pump(timeout, label, predicate=None, sample=True):
            deadline = time.time() + timeout
            while time.time() < deadline:
                session = controller._session
                if sample and session is not None:
                    session._sample_and_submit(force=True)
                if predicate is not None and predicate():
                    return True
                time.sleep(0.005)
            if predicate is not None:
                raise AssertionError("Timed out: {0}; diagnostics={1}".format(
                    label, result.get("diagnostics")))
            return False

        def wait_command(action, timeout=180):
            deadline = time.time() + timeout
            while time.time() < deadline:
                command = read_json(command_path)
                if isinstance(command, dict) and command.get("action") == action:
                    return command
                session = controller._session
                if session is not None:
                    session._sample_and_submit(force=True)
                time.sleep(0.01)
            raise AssertionError("Timed out waiting for command: {0}".format(action))

        def wait_stream(timeout=180):
            """Next stream command, or None once the session has ended."""
            deadline = time.time() + timeout
            while time.time() < deadline:
                if controller._session is None:
                    return None
                command = read_json(command_path)
                if isinstance(command, dict) and command.get("action") == "stream":
                    return command
                session = controller._session
                if session is not None:
                    session._sample_and_submit(force=True)
                time.sleep(0.01)
            raise AssertionError("Timed out waiting for stream command")

        # Phase A: explicit connect, then stream whatever time the test asks
        # until the session ends.
        controller.connect()
        pump(60, "first ready",
             lambda: controller._session is not None
             and controller._cached_playback is not None)
        first_session = controller._session
        result["phase"] = "streaming"
        result["streaming_a"] = {"session": id(first_session)}
        write_json(args.result, result)
        last_marker = ""
        while True:
            command = wait_stream()
            if command is None:
                break
            marker = json.dumps(command, sort_keys=True)
            if marker != last_marker:
                last_marker = marker
                cmds.currentTime(command.get("time", 1))
            session = controller._session
            if session is not None:
                session._sample_and_submit(force=True)
            result["streaming_a"]["time"] = cmds.currentTime(query=True)
            write_json(args.result, result)
            time.sleep(0.01)

        # The Unreal refresh closed the socket: the real sender must observe
        # transport loss as a terminal session event.
        pump(30, "refresh disconnect", lambda: controller._session is None,
             sample=False)
        diagnostic = dict(controller._last_diagnostic or {})
        result["disconnected"] = {
            "observed": True,
            "diagnostic_code": diagnostic.get("code"),
            "auto_reconnect": controller._session is not None,
        }
        write_json(args.result, result)
        # Quiet window: nothing may reconnect on its own.
        pump(1.5, "quiet window", predicate=None, sample=False)
        result["disconnected"]["auto_reconnect"] = controller._session is not None
        result["disconnected"]["retained_cache"] = controller._cached_retention is not None
        result["phase"] = "disconnected"
        write_json(args.result, result)
        assert controller._session is None, "sender reconnected without an explicit connect"

        # Explicit reconnect on the test's command only.
        wait_command("reconnect", timeout=180)
        controller.connect()
        pump(60, "second ready",
             lambda: controller._session is not None
             and controller._cached_playback is not None)
        second_session = controller._session
        assert second_session is not first_session, "reconnect reused the ended session"
        result["reconnected"] = {"session": id(second_session),
                                 "new_session": True}
        result["phase"] = "reconnected"
        write_json(args.result, result)
        cmds.currentTime(3)
        wait_command("stop", timeout=180)
        controller.close()
        controller = None
        result["phase"] = "stopped"
        result["ok"] = True
    except Exception:
        result["error"] = traceback.format_exc()
    finally:
        try:
            if controller is not None:
                controller.close()
        except Exception:
            pass
        write_json(args.result, result)
        maya.standalone.uninitialize()
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
