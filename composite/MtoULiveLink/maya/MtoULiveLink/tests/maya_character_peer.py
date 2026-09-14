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
        base = {}
        for pose in fixture["poses"]:
            for plug in pose.get("edits", {}):
                base[plug] = cmds.getAttr(plug)
                # Test controls can be driven/locked. Detach only in this
                # disposable process; the supplied scene is never saved.
                cmds.setAttr(plug, lock=False)
                for source in cmds.listConnections(plug, source=True, destination=False, plugs=True) or []:
                    cmds.disconnectAttr(source, plug)

        def pump():
            if controller._session is not None:
                controller._session._sample_and_submit(force=True)
            time.sleep(0.02)

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
                        for plug, value in base.items():
                            cmds.setAttr(plug, value)
                        for plug, delta in fixture["poses"][command["pose"]].get("edits", {}).items():
                            cmds.setAttr(plug, base[plug] + delta)
                        cmds.dgdirty(allPlugs=True)
                        result["pose"] = command["pose"]
                        result["phase"] = "posed"
                    elif action == "disconnected":
                        until(lambda: controller._session is None, "refresh disconnect")
                        until(lambda: True, "pump")
                        result["phase"] = "disconnected"
                    elif action == "stop":
                        result.update(id=last_id, phase="stopped", ok=True)
                        write_json(args.result, result)
                        break
                    result.update(id=last_id, warning=controller._last_warning, transforms=list(scene.sample().transforms), ok=True)
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
