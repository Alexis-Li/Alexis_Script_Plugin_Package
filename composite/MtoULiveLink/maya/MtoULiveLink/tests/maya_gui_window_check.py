# ruff: noqa: E402
"""Opt-in window lifecycle and exit check in real Maya GUI sessions.

Run with a host Python (not mayapy):

    python maya_gui_window_check.py --maya <Maya2024>/bin/maya.exe
        --app-dir-template <initialized MAYA_APP_DIR> --evidence <scratch dir>

``--app-dir-template`` is a Maya preference directory a person has already
started Maya with once and whose first-start dialogs they answered; the check
never answers them. Every scenario copies it, so neither the person's daily
preferences nor the template change, and starts its own Maya GUI process with
``TEMP`` inside the evidence directory, where Maya writes a crash log.

The package comes from ``tools/package_maya_tool.py`` like the install check;
the panel is opened from the extracted copy. Windows close with the
``WM_CLOSE`` message their title-bar button sends, and buttons are clicked
through Qt, so Maya runs its own close and click handling.

1. ``exit_both_open``: the panel and the scene-reference window are open
   when Maya quits.
2. ``exit_after_reference_close``: the reference window was closed first.
3. ``exit_after_all_closed``: both windows were closed first.
4. ``close_cycles``: the reference window is opened from the panel twice
   (one window) and closed from its title bar 20 times; Maya quits with the
   panel open.
5. ``owned_paths``: three rounds of the reference window's own close button,
   then closing the panel (which closes the reference window), switching
   workflow, a new scene and opening a scene, each with the reference window
   open; then Maya quits with both open.

A scenario passes when every expectation held, Maya ran the quit, exited
with code 0 within the timeout and wrote no crash log. Inside Maya the same
file runs one scenario, chosen by ``MTOU_WINDOW_STEP``, and keeps
``<scenario>.json`` current after every stage, so a crash leaves the stage
it happened in.
"""

import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

MAIN = "MtoULiveLinkWindow"
REFERENCE = "MtoULiveLinkReferenceWindow"
STEPS = ("exit_both_open", "exit_after_reference_close", "exit_after_all_closed",
         "close_cycles", "owned_paths")
CLOSE_CYCLES = 20
OWNED_ROUNDS = 3
WAIT_SECONDS = 15.0
WM_CLOSE = 0x0010


# ------------------------------------------------------------------ in Maya
def _in_maya():
    import traceback

    import maya.cmds as cmds
    import maya.utils
    from PySide2 import QtCore

    step = os.environ["MTOU_WINDOW_STEP"]
    evidence = Path(os.environ["MTOU_WINDOW_EVIDENCE"])
    sys.path.insert(0, str(Path(os.environ["MTOU_WINDOW_RELEASE"]) / "scripts"))
    import MtoULiveLink
    from mtou_livelink import controller as panel

    result = {"step": step, "pid": os.getpid(), "maya_version": cmds.about(version=True),
              "api": cmds.about(apiVersion=True), "cut": cmds.about(cutIdentifier=True),
              "entry_file": MtoULiveLink.__file__, "stage": "start", "stages": [],
              "snapshots": [], "errors": [], "quit_requested": False}
    path = evidence / (step + ".json")

    def write():
        path.write_text(json.dumps(result, ensure_ascii=False, indent=1, default=str),
                        encoding="utf-8")

    def stage(name):
        result["stage"] = name
        result["stages"].append(name)
        write()

    def widget(name, find):
        from PySide2.QtWidgets import QWidget
        from shiboken2 import wrapInstance

        from maya import OpenMayaUI as omui
        pointer = getattr(omui.MQtUtil, find)(name)
        return wrapInstance(int(pointer), QWidget) if pointer else None

    def windows():
        names = cmds.lsUI(windows=True) or []
        return {name: names.count(name) for name in (MAIN, REFERENCE)}

    baseline_jobs = len(cmds.scriptJob(listJobs=True) or [])

    def snapshot(label):
        controller = panel.CONTROLLER
        entry = {"label": label, "windows": windows(),
                 "script_jobs": len(cmds.scriptJob(listJobs=True) or []) - baseline_jobs,
                 "reference_held": bool(controller and controller._reference_window),
                 "scene_replace_callbacks":
                     len(controller._scene_replace_callbacks) if controller else 0}
        result["snapshots"].append(entry)
        write()
        return entry

    def expect(label, condition, detail=""):
        if not condition:
            result["errors"].append("{0}: {1}".format(label, detail or "expectation failed"))
            write()

    def wait(label, predicate):
        """Yielded to the runner, which returns to Maya's event loop meanwhile."""
        return label, predicate

    # ------------------------------------------------------------ actions
    def open_panel():
        stage("open panel")
        MtoULiveLink.run()
        yield wait("panel open", lambda: cmds.window(MAIN, exists=True))

    def open_reference():
        stage("open reference window")
        button = widget(panel.CONTROLLER._reference_window_button, "findControl")
        button.click()
        yield wait("reference window open", lambda: cmds.window(REFERENCE, exists=True))
        button.click()  # a second click reuses the window
        yield wait("reference reuse", lambda: True)
        expect("one reference window", windows()[REFERENCE] == 1, str(windows()))

    def close_natively(name):
        import ctypes
        stage("title-bar close " + name)
        hwnd = int(widget(name, "findWindow").window().winId())
        if not ctypes.windll.user32.PostMessageW(hwnd, WM_CLOSE, 0, 0):
            raise RuntimeError("WM_CLOSE was not posted to " + name)
        yield wait(name + " closed", lambda: not cmds.window(name, exists=True))

    def click_workflow(attribute):
        stage("workflow " + attribute)
        widget(getattr(panel.CONTROLLER, attribute), "findControl").click()
        yield wait("workflow switched", lambda: True)

    def check_panel_usable(label):
        controller = panel.CONTROLLER
        expect(label + " panel open", cmds.window(MAIN, exists=True))
        expect(label + " panel button", bool(
            widget(controller._reference_window_button, "findControl")))

    # ---------------------------------------------------------- scenarios
    def exit_both_open():
        yield from open_panel()
        yield from open_reference()

    def exit_after_reference_close():
        yield from open_panel()
        yield from open_reference()
        yield from close_natively(REFERENCE)
        check_panel_usable("after close")

    def exit_after_all_closed():
        yield from exit_after_reference_close()
        yield from close_natively(MAIN)
        expect("no tool window", windows() == {MAIN: 0, REFERENCE: 0}, str(windows()))
        expect("panel script jobs released", snapshot("all closed")["script_jobs"] == 0)

    def close_cycles():
        yield from open_panel()
        for cycle in range(CLOSE_CYCLES):
            yield from open_reference()
            yield from close_natively(REFERENCE)
            check_panel_usable("cycle {0}".format(cycle + 1))
            entry = snapshot("cycle {0}".format(cycle + 1))
            expect("cycle {0} state".format(cycle + 1),
                   entry["windows"] == {MAIN: 1, REFERENCE: 0}
                   and not entry["reference_held"] and entry["script_jobs"] == 3,
                   str(entry))

    def owned_paths():
        scene = evidence / (step + "-scene.ma")
        cmds.file(rename=str(scene))
        cmds.file(save=True, type="mayaAscii", force=True)
        yield from open_panel()
        for round_index in range(OWNED_ROUNDS):
            label = "round {0}".format(round_index + 1)
            yield from open_reference()
            stage("close button")
            widget(panel.CONTROLLER._reference_window._close_button, "findControl").click()
            yield wait("reference closed by its button",
                       lambda: not cmds.window(REFERENCE, exists=True))
            yield from open_reference()
            yield from close_natively(MAIN)  # the panel closes its child window
            expect(label + " panel close", windows() == {MAIN: 0, REFERENCE: 0},
                   str(windows()))
            yield from open_panel()
            yield from open_reference()
            yield from click_workflow("_model_workflow_button")
            expect(label + " workflow closes the reference", not cmds.window(
                REFERENCE, exists=True))
            yield from click_workflow("_animation_workflow_button")
            yield from open_reference()
            stage("new scene")
            cmds.file(new=True, force=True)
            yield wait("new scene", lambda: True)
            expect(label + " new scene closes the reference", not cmds.window(
                REFERENCE, exists=True))
            yield from open_reference()
            stage("open scene")
            cmds.file(str(scene), open=True, force=True)
            yield wait("open scene", lambda: True)
            expect(label + " open scene closes the reference", not cmds.window(
                REFERENCE, exists=True))
            entry = snapshot(label)
            expect(label + " state", entry["windows"] == {MAIN: 1, REFERENCE: 0}
                   and entry["script_jobs"] == 3 and entry["scene_replace_callbacks"] == 2,
                   str(entry))
        yield from open_reference()

    # ------------------------------------------------------------- runner
    scenario = {"exit_both_open": exit_both_open,
                "exit_after_reference_close": exit_after_reference_close,
                "exit_after_all_closed": exit_after_all_closed,
                "close_cycles": close_cycles, "owned_paths": owned_paths}[step]()

    def quit_maya():
        snapshot("before quit")
        result["quit_requested"] = True
        stage("quit")
        cmds.quit(force=True)

    def advance(pending=None, deadline=None):
        try:
            if pending is not None:
                label, predicate = pending
                if not predicate():
                    if time.time() > deadline:
                        raise RuntimeError("timed out waiting for " + label)
                    QtCore.QTimer.singleShot(50, lambda: advance(pending, deadline))
                    return
            pending = next(scenario)
            # Return to Maya's event loop before checking, as a user's next
            # click would.
            QtCore.QTimer.singleShot(
                50, lambda: advance(pending, time.time() + WAIT_SECONDS))
        except StopIteration:
            quit_maya()
        except Exception:
            result["errors"].append(traceback.format_exc()[-1500:])
            quit_maya()

    def when_ready():
        stage("ready")
        advance()

    # Let Maya finish building its UI, as the install check does.
    maya.utils.executeDeferred(lambda: QtCore.QTimer.singleShot(3000, when_ready))


# ------------------------------------------------------------- host driver
def check_template(template):
    """The template's Maya 2024 preference files, or why it cannot be used."""
    template = Path(template)
    if not template.is_dir():
        raise SystemExit("--app-dir-template {0} is not a directory".format(template))
    prefs = sorted(template.glob("2024/**/prefs/userPrefs.mel"))
    if not prefs:
        raise SystemExit(
            "--app-dir-template has no 2024/<language>/prefs/userPrefs.mel: start Maya 2024 "
            "once with MAYA_APP_DIR set to it, answer its first-start dialogs and quit.")
    return [path.relative_to(template).as_posix() for path in prefs]


def crash_files(*folders):
    found = []
    for folder in folders:
        for path in Path(folder).rglob("*"):
            name = path.name.lower()
            if path.is_file() and ("crash" in name or name.endswith(".dmp")):
                found.append(path.relative_to(folder.parent).as_posix())
    return sorted(found)


def run_step(maya, template, evidence, release, step, timeout):
    app_dir = evidence / ("mayaapp-" + step)
    temp = evidence / ("temp-" + step)
    shutil.copytree(str(template), str(app_dir))
    temp.mkdir()
    env = dict(os.environ)
    env.update({"MAYA_APP_DIR": str(app_dir), "TEMP": str(temp), "TMP": str(temp),
                "MTOU_WINDOW_STEP": step, "MTOU_WINDOW_EVIDENCE": str(evidence),
                "MTOU_WINDOW_RELEASE": str(release)})
    env.pop("PYTHONPATH", None)
    command = ('python("import runpy; runpy.run_path(r\'{0}\', '
               'run_name=\'mtou_window_step\')")').format(Path(__file__).resolve().as_posix())
    started = time.time()
    process = subprocess.Popen([maya, "-command", command], env=env, cwd=str(evidence))
    outcome = {"pid": process.pid, "timed_out": False}
    try:
        outcome["exit_code"] = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()  # only the process this check started
        outcome["timed_out"] = True
        outcome["exit_code"] = process.wait()
    outcome["seconds"] = round(time.time() - started, 1)
    outcome["crash_files"] = crash_files(temp, app_dir)
    path = evidence / (step + ".json")
    outcome["maya"] = json.loads(path.read_text(encoding="utf-8")) if path.exists() \
        else {"errors": ["no result"]}
    maya_result = outcome["maya"]
    outcome["passed"] = bool(
        not outcome["timed_out"] and outcome["exit_code"] == 0
        and not outcome["crash_files"] and maya_result.get("quit_requested")
        and not maya_result.get("errors"))
    return outcome


def main(argv):
    import argparse

    from maya_gui_install_check import _build_package, _extract

    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--maya", required=True)
    parser.add_argument("--app-dir-template", required=True)
    parser.add_argument("--evidence", required=True)
    parser.add_argument("--step", action="append", choices=STEPS,
                        help="run only these scenarios (repeatable)")
    parser.add_argument("--timeout", type=float, default=300.0)
    args = parser.parse_args(argv)

    template = Path(args.app_dir_template).resolve()
    summary = {"template_prefs": check_template(template), "steps": {}}
    evidence = Path(args.evidence).resolve()
    if evidence.exists():
        shutil.rmtree(evidence)
    evidence.mkdir(parents=True)
    archive = _build_package(evidence)
    release = _extract(archive, evidence / "release" / "MtoULiveLink")
    summary["archive"] = archive.name
    for step in args.step or STEPS:
        summary["steps"][step] = run_step(
            args.maya, template, evidence, release, step, args.timeout)
    summary["passed"] = all(outcome["passed"] for outcome in summary["steps"].values())
    (evidence / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=1), encoding="utf-8")
    print(json.dumps({step: {key: outcome[key] for key in (
        "passed", "exit_code", "timed_out", "crash_files", "seconds")}
        | {"stage": outcome["maya"].get("stage"), "errors": outcome["maya"].get("errors")}
        for step, outcome in summary["steps"].items()}, ensure_ascii=False, indent=1))
    return 0 if summary["passed"] else 1


if __name__ == "mtou_window_step":
    _in_maya()
elif __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    sys.exit(main(sys.argv[1:]))
