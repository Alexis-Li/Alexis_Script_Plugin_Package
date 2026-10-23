# ruff: noqa: E402
"""Opt-in standalone-install check in real Maya GUI sessions.

Run with a host Python (not mayapy):

    python maya_gui_install_check.py --maya <Maya2024>/bin/maya.exe
        --old-entry <0.9.0 MtoULiveLink.py> --evidence <scratch dir>

Each step starts its own Maya GUI process with a disposable ``MAYA_APP_DIR``
under the evidence directory, so the user's preferences, shelves and running
Maya sessions are never touched. The tool is loaded only from that app
directory's ``scripts`` folder, never from this repository:

1. ``install_old``: the previous single-file release is installed; a saved
   shelf button runs ``import MtoULiveLink; MtoULiveLink.run()``.
2. ``upgrade``: Maya restarts after the whole current ``scripts`` directory
   replaces it. The saved shelf button opens the new release from the app
   directory; ``run()`` and ``main()`` reuse one panel, closing and reopening
   the panel and the scene-reference window leaves one window each.
3. ``missing_module`` and ``missing_package``: one module, then the whole
   package, is removed; the shelf button must report what is missing.
4. ``rollback``: the previous single file is restored and the package
   removed; the shelf button opens the previous release again.

Inside Maya the same file runs one step, chosen by ``MTOU_INSTALL_STEP``, and
writes ``<step>.json`` and window images into the evidence directory.
"""

import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

SHELF = "MtoU"
SHELF_COMMAND = "import MtoULiveLink\nMtoULiveLink.run()"
WINDOWS = ("MtoULiveLinkWindow", "MtoULiveLinkReferenceWindow")
STEPS = ("install_old", "upgrade", "missing_module", "missing_package", "rollback")


# ------------------------------------------------------------------ in Maya
def _in_maya():
    import traceback

    import maya.cmds as cmds
    import maya.utils

    step = os.environ["MTOU_INSTALL_STEP"]
    evidence = Path(os.environ["MTOU_INSTALL_EVIDENCE"])
    scripts = Path(os.environ["MTOU_INSTALL_SCRIPTS"])
    result = {"step": step, "maya_version": cmds.about(version=True), "pid": os.getpid(),
              "errors": []}

    def grab(name):
        from PySide2.QtWidgets import QWidget
        from shiboken2 import wrapInstance

        from maya import OpenMayaUI as omui
        shots = {}
        for window in WINDOWS:
            pointer = omui.MQtUtil.findWindow(window)
            if pointer:
                widget = wrapInstance(int(pointer), QWidget)
                path = evidence / "{0}-{1}-{2}.png".format(step, name, window)
                widget.grab().save(str(path))
                shots[window] = [widget.width(), widget.height()]
        result.setdefault("shots", {})[name] = shots

    def windows():
        return {window: bool(cmds.window(window, exists=True)) for window in WINDOWS}

    def shelf_button():
        if not cmds.shelfLayout(SHELF, exists=True):
            return None
        for child in cmds.shelfLayout(SHELF, query=True, childArray=True) or []:
            if cmds.shelfButton(child, exists=True):
                return child
        return None

    def press(button):
        """Run the button's own saved command, as a click does."""
        command = cmds.shelfButton(button, query=True, command=True)
        language = cmds.shelfButton(button, query=True, sourceType=True)
        namespace = {}
        if language == "python":
            exec(command, namespace)  # noqa: S102 - the shelf command under test
        else:
            maya.mel.eval(command)

    def loaded():
        module = sys.modules.get("MtoULiveLink")
        package = sys.modules.get("mtou_livelink")
        return {
            "entry_file": getattr(module, "__file__", None),
            "package_file": getattr(package, "__file__", None),
            "version": getattr(module, "__version__", None)
            or getattr(sys.modules.get("mtou_livelink.config"), "__version__", None),
            "protocol": getattr(module, "PROTOCOL_VERSION", None),
            "repository_on_path": [entry for entry in sys.path
                                   if "Alexis_Script_Plugin_Package" in entry],
        }

    def run_step():
        if step == "install_old":
            if cmds.shelfLayout(SHELF, exists=True):
                cmds.deleteUI(SHELF, layout=True)
            top = maya.mel.eval("$tmp = $gShelfTopLevel")
            cmds.shelfLayout(SHELF, parent=top)
            cmds.shelfButton(parent=SHELF, label="MtoU", annotation="MtoU_LiveLink",
                             image="pythonFamily.png", sourceType="python",
                             command=SHELF_COMMAND)
            maya.mel.eval("saveAllShelves $gShelfTopLevel")
            cmds.savePrefs(general=True)
            press(shelf_button())
            result["loaded"] = loaded()
            result["windows"] = windows()
            grab("open")
        elif step in ("upgrade", "rollback"):
            button = shelf_button()
            result["shelf_command"] = (cmds.shelfButton(button, query=True, command=True)
                                       if button else None)
            press(button)
            first = sys.modules["MtoULiveLink"]
            result["loaded"] = loaded()
            result["windows"] = windows()
            grab("open")
            if step == "upgrade":
                controller = first.run()
                result["run_again_same_controller"] = controller is first.run()
                result["main_same_controller"] = first.main() is controller
                controller.open_reference_window()
                controller.open_reference_window()
                result["detail_windows_open"] = windows()
                grab("detail-windows")
                for window in WINDOWS:
                    if cmds.window(window, exists=True):
                        cmds.deleteUI(window)
                result["after_close"] = windows()
                press(button)
                reopened = sys.modules["MtoULiveLink"].run()
                result["reopened_new_controller"] = reopened is not controller
                reopened.open_reference_window()
                result["reopened"] = windows()
                result["window_count"] = {
                    window: sum(1 for name in cmds.lsUI(windows=True) or [] if name == window)
                    for window in WINDOWS}
                grab("reopened")
        else:
            try:
                press(shelf_button())
                result["opened"] = True
            except Exception as error:  # the missing-part report under test
                result["opened"] = False
                result["error_type"] = type(error).__name__
                result["error"] = str(error)
            result["windows"] = windows()
        result["scripts_listing"] = sorted(
            str(path.relative_to(scripts)) for path in scripts.rglob("*.py"))

    def finish():
        try:
            run_step()
        except Exception:
            result["errors"].append(traceback.format_exc()[-1500:])
        (evidence / (step + ".json")).write_text(
            json.dumps(result, ensure_ascii=False, indent=1, default=str), encoding="utf-8")
        cmds.file(new=True, force=True)
        cmds.quit(force=True)

    import maya.mel  # noqa: F401 - used by the step through maya.mel.eval
    from PySide2 import QtCore

    waited = [0]

    def when_ready():
        # Maya builds its shelves after the startup command; a later step's
        # saved button only exists once they are loaded.
        if step != "install_old" and shelf_button() is None and waited[0] < 120:
            waited[0] += 1
            if cmds.shelfLayout(SHELF, exists=True):
                # Maya fills a shelf tab only when it is shown, as a click does.
                maya.mel.eval('shelfTabLayout -edit -selectTab "{0}" $gShelfTopLevel'.format(SHELF))
            QtCore.QTimer.singleShot(500, when_ready)
            return
        result["shelf_wait_seconds"] = waited[0] * 0.5
        finish()

    maya.utils.executeDeferred(when_ready)


# ------------------------------------------------------------- host driver
def _copy_release(source, scripts):
    shutil.copy2(source / "MtoULiveLink.py", scripts / "MtoULiveLink.py")
    shutil.copytree(source / "mtou_livelink", scripts / "mtou_livelink",
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))


def _run_maya(maya, app_dir, evidence, scripts, step, timeout):
    env = dict(os.environ)
    env.update({"MAYA_APP_DIR": str(app_dir), "MTOU_INSTALL_STEP": step,
                "MTOU_INSTALL_EVIDENCE": str(evidence), "MTOU_INSTALL_SCRIPTS": str(scripts)})
    env.pop("PYTHONPATH", None)
    command = ('python("import runpy; runpy.run_path(r\'{0}\', '
               'run_name=\'mtou_install_step\')")').format(Path(__file__).resolve().as_posix())
    # Maya starts in the evidence directory so neither this repository nor
    # the caller's directory is on its sys.path.
    process = subprocess.Popen([maya, "-command", command], env=env, cwd=str(evidence))
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()  # only the process this check started
        raise
    return process.pid


def main(argv):
    import argparse
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--maya", required=True)
    parser.add_argument("--old-entry", required=True)
    parser.add_argument("--evidence", required=True)
    parser.add_argument("--timeout", type=float, default=300.0)
    args = parser.parse_args(argv)

    evidence = Path(args.evidence).resolve()
    if evidence.exists():
        shutil.rmtree(evidence)
    app_dir = evidence / "maya-app"
    scripts = app_dir / "scripts"
    scripts.mkdir(parents=True)
    current = Path(__file__).resolve().parents[1] / "scripts"
    summary = {"steps": {}, "pids": {}}

    for step in STEPS:
        if step == "install_old":
            shutil.copy2(args.old_entry, scripts / "MtoULiveLink.py")
        elif step == "upgrade":
            (scripts / "MtoULiveLink.py").unlink()
            _copy_release(current, scripts)
        elif step == "missing_module":
            (scripts / "mtou_livelink" / "linkage.py").unlink()
        elif step == "missing_package":
            shutil.rmtree(scripts / "mtou_livelink")
        elif step == "rollback":
            (scripts / "MtoULiveLink.py").unlink()
            shutil.copy2(args.old_entry, scripts / "MtoULiveLink.py")
        started = time.time()
        summary["pids"][step] = _run_maya(args.maya, app_dir, evidence, scripts, step, args.timeout)
        path = evidence / (step + ".json")
        summary["steps"][step] = json.loads(path.read_text(encoding="utf-8")) if path.exists() \
            else {"errors": ["no result"]}
        summary["steps"][step]["seconds"] = round(time.time() - started, 1)
    (evidence / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1),
                                           encoding="utf-8")
    print(json.dumps({step: {key: value for key, value in result.items()
                             if key not in ("scripts_listing", "shots")}
                      for step, result in summary["steps"].items()}, ensure_ascii=False, indent=1))
    return 0


if __name__ == "mtou_install_step":
    _in_maya()
elif __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
