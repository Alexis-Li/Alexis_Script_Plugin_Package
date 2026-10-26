# ruff: noqa: E402
"""Opt-in drag-and-drop install check in real Maya GUI sessions.

Run with a host Python (not mayapy):

    python maya_gui_install_check.py --maya <Maya2024>/bin/maya.exe
        --old-entry <0.9.0 MtoULiveLink.py> --evidence <scratch dir>

The check builds the Maya component archive with `tools/package_maya_tool.py`
and extracts it twice: under a path with a space and Chinese characters (which
the installer copies into the Maya modules folder) and under an ASCII path
with a space (which the `.mod` names directly). Each step starts its own Maya
GUI process with a disposable ``MAYA_APP_DIR`` under the evidence directory,
so the user's preferences, shelves and running Maya sessions are never
touched. ``install.mel`` is sourced as a viewport drop sources it, and its
dialogs are answered with their first button and recorded.

1. ``install_old``: the previous single-file release is installed; a saved
   shelf button runs ``import MtoULiveLink; MtoULiveLink.run()``.
2. ``drag_install``: the saved button opens the old release, then the
   Chinese-path package's ``install.mel`` is dropped. The old file is deleted
   without a backup, the copy and ``.mod`` are written, the existing button is
   kept and a restart is requested because the old release is loaded.
3. ``upgrade``: after the restart the saved button opens the new release from
   the module; ``run()`` and ``main()`` reuse one panel, closing and reopening
   the panel and the scene-reference window leaves one window each.
4. ``reinstall``: dropping the same ``install.mel`` again changes nothing
   that matters: no conflict, no restart, one ``.mod``, one button.
5. ``same_session``: with the tool open, dropping the same package needs no
   restart; dropping a package whose version marker differs (written into the
   same module copy) requests a restart and the session keeps the old code.
6. ``broken_package`` and ``partial_package``: dropping ``install.mel`` from a
   package without its ``mtou_livelink`` folder, or without one module the
   entry imports, reports it and leaves the installed copy and ``.mod`` as
   they were.
7. ``missing_module`` and ``missing_package``: one module, then the whole
   package, is removed from the installed copy; the shelf button must report
   what is missing.
8. ``rollback``: the README rollback (remove ``MtoULiveLink.mod`` and the
   copy, restore the previous single file) opens the previous release again.
9. ``clean_install`` and ``clean_restart`` (a second, empty app directory):
   the ASCII-path package's ``install.mel`` adds a button to the current shelf
   and opens the new release without a restart; after a restart the module
   loads from the extracted folder.

Inside Maya the same file runs one step, chosen by ``MTOU_INSTALL_STEP``, and
writes ``<step>.json`` and window images into the evidence directory.
"""

import json
import os
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

SHELF = "MtoU"
SHELF_COMMAND = "import MtoULiveLink\nMtoULiveLink.run()"
WINDOWS = ("MtoULiveLinkWindow", "MtoULiveLinkReferenceWindow")
STEPS = ("install_old", "drag_install", "upgrade", "reinstall", "same_session",
         "broken_package", "partial_package", "missing_module", "missing_package", "rollback",
         "clean_install", "clean_restart")
CLEAN_STEPS = ("clean_install", "clean_restart")
# Steps that start by pressing a button saved by an earlier step.
SAVED_BUTTON_STEPS = ("drag_install", "upgrade", "same_session", "missing_module",
                      "missing_package", "rollback", "clean_restart")
# Version marker of the package `same_session` writes over the installed copy.
CHANGED_VERSION = "0.10.0+install-check"


# ------------------------------------------------------------------ in Maya
def _in_maya():
    import traceback

    import maya.cmds as cmds
    import maya.utils

    step = os.environ["MTOU_INSTALL_STEP"]
    evidence = Path(os.environ["MTOU_INSTALL_EVIDENCE"])
    release = os.environ.get("MTOU_INSTALL_RELEASE", "")
    result = {"step": step, "maya_version": cmds.about(version=True), "pid": os.getpid(),
              "errors": [], "dialogs": []}

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

    def shelf_top():
        return maya.mel.eval("$tmp = $gShelfTopLevel")

    def tool_buttons():
        """Every loaded shelf button whose command opens the tool."""
        buttons = []
        for shelf in cmds.tabLayout(shelf_top(), query=True, childArray=True) or []:
            for child in cmds.shelfLayout(shelf, query=True, childArray=True) or []:
                if not cmds.shelfButton(child, exists=True):
                    continue
                command = cmds.shelfButton(child, query=True, command=True) or ""
                if "MtoULiveLink.run()" in command or "MtoULiveLink.main()" in command:
                    buttons.append((shelf, child))
        return buttons

    def show_saved_shelves():
        """Maya fills a shelf tab only when it is shown, as a click does."""
        shelf_dir = Path(cmds.internalVar(userShelfDir=True))
        for path in shelf_dir.glob("shelf_*.mel"):
            if "MtoULiveLink.run()" in path.read_text(encoding="utf-8", errors="replace"):
                name = path.stem[len("shelf_"):]
                if cmds.shelfLayout(name, exists=True):
                    maya.mel.eval('shelfTabLayout -edit -selectTab "{0}" $gShelfTopLevel'
                                  .format(name))

    def press(button):
        """Run the button's own saved command, as a click does."""
        command = cmds.shelfButton(button, query=True, command=True)
        language = cmds.shelfButton(button, query=True, sourceType=True)
        namespace = {}
        if language == "python":
            exec(command, namespace)  # noqa: S102 - the shelf command under test
        else:
            maya.mel.eval(command)

    def press_saved():
        buttons = tool_buttons()
        result["tool_buttons"] = [shelf for shelf, _ in buttons]
        press(buttons[0][1])

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

    def drop_install_mel(source=None):
        """Source install.mel as a viewport drop does; answer dialogs with their first button."""
        original = cmds.confirmDialog

        def answer(*args, **kwargs):
            buttons = kwargs.get("button") or ["OK"]
            result["dialogs"].append({"message": kwargs.get("message"), "buttons": buttons})
            return buttons[0]

        cmds.confirmDialog = answer
        try:
            maya.mel.eval('source "{0}"'.format(
                (Path(source or release) / "install.mel").as_posix()))
        finally:
            cmds.confirmDialog = original
        import __main__
        install_result = getattr(__main__, "MTOU_INSTALL_RESULT", None)
        result["install"] = install_result
        result["modules_listing"] = sorted(
            path.name for path in (Path(cmds.internalVar(userAppDir=True)) / "modules").glob("*"))
        mod = Path(cmds.internalVar(userAppDir=True)) / "modules" / "MtoULiveLink.mod"
        result["mod_text"] = mod.read_text(encoding="ascii") if mod.exists() else None
        return install_result

    def module_info():
        return {name: cmds.moduleInfo(moduleName=name, path=True)
                for name in cmds.moduleInfo(listModules=True) or [] if name == "MtoULiveLink"}

    def check_upgrade(first):
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
        press_saved()
        reopened = sys.modules["MtoULiveLink"].run()
        result["reopened_new_controller"] = reopened is not controller
        reopened.open_reference_window()
        result["reopened"] = windows()
        result["window_count"] = {
            window: sum(1 for name in cmds.lsUI(windows=True) or [] if name == window)
            for window in WINDOWS}
        grab("reopened")

    def run_step():
        result["module"] = module_info()
        if step == "install_old":
            if cmds.shelfLayout(SHELF, exists=True):
                cmds.deleteUI(SHELF, layout=True)
            cmds.shelfLayout(SHELF, parent=shelf_top())
            cmds.shelfButton(parent=SHELF, label="MtoU", annotation="MtoU_LiveLink",
                             image="pythonFamily.png", sourceType="python",
                             command=SHELF_COMMAND)
            maya.mel.eval("saveAllShelves $gShelfTopLevel")
            cmds.savePrefs(general=True)
            press_saved()
            result["loaded"] = loaded()
            result["windows"] = windows()
            grab("open")
        elif step == "drag_install":
            press_saved()
            result["loaded_before"] = loaded()
            drop_install_mel()
            result["loaded"] = loaded()
            result["tool_buttons_after"] = [shelf for shelf, _ in tool_buttons()]
            app_dir = Path(cmds.internalVar(userAppDir=True))
            result["legacy_entry_left"] = (app_dir / "scripts" / "MtoULiveLink.py").exists()
            result["app_dir_listing"] = sorted(path.name for path in app_dir.iterdir())
        elif step in ("upgrade", "rollback", "clean_restart"):
            press_saved()
            result["loaded"] = loaded()
            result["windows"] = windows()
            grab("open")
            if step == "upgrade":
                check_upgrade(sys.modules["MtoULiveLink"])
        elif step == "same_session":
            press_saved()
            result["loaded_before"] = loaded()
            result["same_package"] = drop_install_mel()
            result["changed_package"] = drop_install_mel(os.environ["MTOU_INSTALL_RELEASE2"])
            result["loaded"] = loaded()
            result["imported_version_after"] = __import__("importlib").import_module(
                "mtou_livelink.config").__version__
        elif step == "partial_package":
            drop_install_mel()
            copy = Path(cmds.internalVar(userAppDir=True)) / "modules" / "MtoULiveLink"
            result["copy_linkage_kept"] = (copy / "scripts" / "mtou_livelink"
                                           / "linkage.py").exists()
        elif step in ("reinstall", "broken_package", "clean_install"):
            drop_install_mel()
            show_saved_shelves()
            result["tool_buttons_after"] = [shelf for shelf, _ in tool_buttons()]
            result["current_shelf"] = cmds.tabLayout(shelf_top(), query=True, selectTab=True)
            if step != "broken_package":
                press_saved()
                result["loaded"] = loaded()
                result["windows"] = windows()
                grab("open")
        else:
            try:
                press_saved()
                result["opened"] = True
            except Exception as error:  # the missing-part report under test
                result["opened"] = False
                result["error_type"] = type(error).__name__
                result["error"] = str(error)
            result["windows"] = windows()

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
        # Maya builds its shelves after the startup command; a saved button
        # only exists once they are loaded and its tab has been shown.
        if step in SAVED_BUTTON_STEPS and not tool_buttons() and waited[0] < 120:
            waited[0] += 1
            show_saved_shelves()
            QtCore.QTimer.singleShot(500, when_ready)
            return
        if step not in SAVED_BUTTON_STEPS and waited[0] < 6:
            waited[0] += 1  # let Maya finish building its shelves
            QtCore.QTimer.singleShot(500, when_ready)
            return
        result["shelf_wait_seconds"] = waited[0] * 0.5
        finish()

    maya.utils.executeDeferred(when_ready)


# ------------------------------------------------------------- host driver
def _build_package(evidence):
    """Build the component archive with the repository packager; return its path."""
    tools = Path(__file__).resolve().parents[5] / "tools"
    sys.path.insert(0, str(tools))
    import package_maya_tool
    result = package_maya_tool.package("MtoULiveLink", evidence / "package", apply=True)
    return evidence / "package" / Path(result["archive"]).name


def _extract(archive, folder):
    folder.mkdir(parents=True)
    with zipfile.ZipFile(archive) as source:
        source.extractall(folder)
    return folder


def _run_maya(maya, app_dir, evidence, step, release, timeout, release2=None):
    env = dict(os.environ)
    env.update({"MAYA_APP_DIR": str(app_dir), "MTOU_INSTALL_STEP": step,
                "MTOU_INSTALL_EVIDENCE": str(evidence), "MTOU_INSTALL_RELEASE": str(release),
                "MTOU_INSTALL_RELEASE2": str(release2 or "")})
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
    evidence.mkdir(parents=True)
    archive = _build_package(evidence)
    chinese = _extract(archive, evidence / "发布 目录" / "MtoULiveLink")
    ascii_release = _extract(archive, evidence / "release ascii" / "MtoULiveLink")
    broken = _extract(archive, evidence / "broken" / "MtoULiveLink")
    shutil.rmtree(broken / "scripts" / "mtou_livelink")
    partial = _extract(archive, evidence / "partial" / "MtoULiveLink")
    (partial / "scripts" / "mtou_livelink" / "linkage.py").unlink()
    changed = _extract(archive, evidence / "发布 目录2" / "MtoULiveLink")
    config = changed / "scripts" / "mtou_livelink" / "config.py"
    text = config.read_text(encoding="utf-8")
    marker = '__version__ = "{0}"'
    version = text.split('__version__ = "', 1)[1].split('"', 1)[0]
    config.write_text(text.replace(marker.format(version), marker.format(CHANGED_VERSION)),
                      encoding="utf-8")
    app_dir = evidence / "maya-app"
    clean_app_dir = evidence / "maya-app-clean"
    scripts = app_dir / "scripts"
    scripts.mkdir(parents=True)
    copy = app_dir / "modules" / "MtoULiveLink"
    summary = {"archive": archive.name, "steps": {}, "pids": {}}

    for step in STEPS:
        release = chinese
        if step == "install_old":
            shutil.copy2(args.old_entry, scripts / "MtoULiveLink.py")
        elif step == "broken_package":
            release = broken
        elif step == "partial_package":
            release = partial
        elif step == "missing_module":
            (copy / "scripts" / "mtou_livelink" / "linkage.py").unlink()
        elif step == "missing_package":
            shutil.rmtree(copy / "scripts" / "mtou_livelink")
        elif step == "rollback":
            (app_dir / "modules" / "MtoULiveLink.mod").unlink()
            shutil.rmtree(copy)
            shutil.copy2(args.old_entry, scripts / "MtoULiveLink.py")
        elif step in CLEAN_STEPS:
            release = ascii_release
        started = time.time()
        summary["pids"][step] = _run_maya(
            args.maya, clean_app_dir if step in CLEAN_STEPS else app_dir, evidence, step,
            release, args.timeout, release2=changed if step == "same_session" else None)
        path = evidence / (step + ".json")
        summary["steps"][step] = json.loads(path.read_text(encoding="utf-8")) if path.exists() \
            else {"errors": ["no result"]}
        summary["steps"][step]["seconds"] = round(time.time() - started, 1)
    (evidence / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1),
                                           encoding="utf-8")
    print(json.dumps({step: {key: value for key, value in result.items()
                             if key not in ("shots",)}
                      for step, result in summary["steps"].items()}, ensure_ascii=False, indent=1))
    return 0


if __name__ == "mtou_install_step":
    _in_maya()
elif __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
