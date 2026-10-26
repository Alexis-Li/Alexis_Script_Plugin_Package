"""Maya preference templates for the Maya GUI acceptance tests.

Maya's linkage throughput depends on which plug-ins its preferences load
automatically (``prefs/pluginPrefs.mel``). On a first start Maya 2024 loads its
default set of about 40 plug-ins and then saves the list, so a reused
``MAYA_APP_DIR`` changes between runs. Maya also loads that default set on
every start whose ``userPrefs.mel`` lacks its one-time plug-in load markers
(``oneTimeDefaultPluginLoad<version>`` and related optionVars), whatever
``pluginPrefs.mel`` says. A template therefore holds ``pluginPrefs.mel`` and a
``userPrefs.mel`` with only those markers; the Unreal test copies it into each
run's evidence directory.

Build a template with a host Python (not mayapy):

    python maya_prefs_template.py --maya <Maya2024>/bin/maya.exe
        --kind minimal --out <scratch>/maya-prefs-minimal

``minimal`` loads only ``modelingToolkit`` automatically; ``maya-default``
keeps the list a first Maya start saves. Maya runs once with a disposable
preference directory to place the files under the language folder this Maya
uses (for example ``2024/zh_CN/prefs``); nothing else from that run is kept.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PLUGIN_PREFS = "pluginPrefs.mel"
USER_PREFS = "userPrefs.mel"
MINIMAL_PLUGINS = ("modelingToolkit",)
KINDS = ("minimal", "maya-default")
_AUTOLOAD = re.compile(r'autoLoadPlugin\(\\"[^"\\]*\\", \\"[^"\\]+\\", \\"([^"\\]+)\\"')
_HEADER = "//Maya Preference 2024 (Release 1)\n//\n//\n"
_ONE_TIME = re.compile(r'^ -iv "(?:oneTime\w+|isFirstTimeRunningMaya)" -?\d+$', re.MULTILINE)
_IN_MAYA = "MTOU_PREFS_TEMPLATE_IN_MAYA"
# Maya saves only the plug-ins loaded when it quits, and autoloading continues
# well after startup, so the in-Maya step waits for the list to stay unchanged.
_SETTLE_SECONDS = 10.0
_SETTLE_LIMIT_SECONDS = 300.0


def find_saved_plugin_prefs(root):
    """The one ``pluginPrefs.mel`` a single Maya start saved under ``root``.

    Only for a disposable directory one Maya used: None when there is none,
    ValueError when several versions or languages leave the choice open.
    """
    found = sorted(Path(root).glob("*/**/prefs/" + PLUGIN_PREFS))
    if len(found) > 1:
        raise ValueError("several {0} under {1}: {2}".format(
            PLUGIN_PREFS, root, ", ".join(p.relative_to(root).as_posix() for p in found)))
    return found[0] if found else None


def autoload_plugins(text):
    """Plug-in names a ``pluginPrefs.mel`` loads automatically, in file order."""
    return _AUTOLOAD.findall(text)


def describe(pref_dir, app_dir=""):
    """What ``pref_dir`` (the running Maya's ``internalVar(userPrefDir=True)``) loads.

    Reads only ``pref_dir/pluginPrefs.mel``, never a sibling version or
    language folder; ``plugin_prefs`` is relative to ``app_dir`` when inside it.
    """
    path = Path(pref_dir) / PLUGIN_PREFS
    shown = path.as_posix()
    if app_dir:
        try:
            shown = path.resolve().relative_to(Path(app_dir).resolve()).as_posix()
        except ValueError:
            pass
    if not path.is_file():
        return {"pref_dir": Path(pref_dir).as_posix(), "plugin_prefs": None, "sha256": None,
                "autoload": None}
    data = path.read_bytes()
    return {"pref_dir": Path(pref_dir).as_posix(),
            "plugin_prefs": shown,
            "sha256": hashlib.sha256(data).hexdigest(),
            "autoload": autoload_plugins(data.decode("utf-8", "replace"))}


def record_plugin_sample(result, label, stage, loaded, elapsed):
    """Keep the plug-ins loaded at one named moment in ``result["plugin_samples"]``.

    The first sample under a label stays: later stages, snapshots or a repeated
    command never replace what was loaded at that moment. Returns whether it was
    stored.
    """
    samples = result.setdefault("plugin_samples", {})
    if label in samples:
        return False
    samples[label] = {"stage": stage, "loaded": sorted(loaded), "elapsed_s": round(elapsed, 2)}
    return True


def minimal_text(default_text):
    """Keep the default file's autoload lines for the minimal plug-ins only."""
    lines = [line for line in default_text.splitlines()
             if autoload_plugins(line) and autoload_plugins(line)[0] in MINIMAL_PLUGINS]
    missing = set(MINIMAL_PLUGINS) - {autoload_plugins(line)[0] for line in lines}
    if missing:
        raise ValueError("the default list has no " + ", ".join(sorted(missing)))
    return _HEADER + "".join(line + "\n" for line in lines)


def marker_text(user_prefs_text):
    """A userPrefs.mel holding only Maya's one-time plug-in load markers."""
    lines = _ONE_TIME.findall(user_prefs_text)
    if not any("oneTimeDefaultPluginLoad" in line for line in lines):
        raise ValueError("the saved userPrefs.mel has no one-time plug-in load markers")
    return (_HEADER + "\noptionVar -version 3;\n\noptionVar\n"
            + "".join(line + "\n" for line in lines) + " ;\n")


def write_template(default_prefs, relative, kind, out):
    """Write ``relative`` (the pluginPrefs.mel path) and its userPrefs.mel under ``out``.

    ``default_prefs`` is the pluginPrefs.mel a first Maya start saved; the
    userPrefs.mel beside it supplies the markers.
    """
    if kind not in KINDS:
        raise ValueError("unknown kind " + kind)
    default_prefs = Path(default_prefs)
    text = default_prefs.read_text(encoding="utf-8")
    markers = marker_text((default_prefs.parent / USER_PREFS).read_text(
        encoding="utf-8", errors="replace"))
    target = Path(out) / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text if kind == "maya-default" else minimal_text(text),
                      encoding="utf-8", newline="\n")
    (target.parent / USER_PREFS).write_text(markers, encoding="utf-8", newline="\n")
    return target


def _in_maya():
    """Quit once the loaded plug-ins have stayed the same for a while; Maya then saves them."""
    import time

    import maya.cmds as cmds
    from PySide2.QtCore import QTimer

    import __main__

    state = {"names": None, "since": time.time(), "start": time.time()}

    def poll():
        names = sorted(cmds.pluginInfo(query=True, listPlugins=True) or [])
        now = time.time()
        if names != state["names"]:
            state["names"], state["since"] = names, now
        if now - state["since"] >= _SETTLE_SECONDS or now - state["start"] >= _SETTLE_LIMIT_SECONDS:
            timer.stop()
            cmds.quit(force=True)

    timer = QTimer()
    timer.timeout.connect(poll)
    timer.start(500)
    __main__._mtou_prefs_template_timer = timer  # outlives runpy's namespace


def first_start_prefs(maya, work, timeout):
    """Start Maya once with an empty preference directory and return its saved list."""
    env = dict(os.environ, MAYA_APP_DIR=str(work))
    env[_IN_MAYA] = "1"
    command = "python(\"import runpy; runpy.run_path(r'{0}')\")".format(
        Path(__file__).resolve().as_posix())
    process = subprocess.Popen([str(maya), "-command", command], env=env)
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()  # only the process started here
        raise RuntimeError("Maya did not quit within {0} s".format(timeout))
    path = find_saved_plugin_prefs(work)
    if path is None:
        raise RuntimeError("Maya saved no " + PLUGIN_PREFS)
    return path


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--maya", required=True, help="Maya 2024 bin/maya.exe")
    parser.add_argument("--kind", choices=KINDS, required=True)
    parser.add_argument("--out", required=True, help="new template directory")
    parser.add_argument("--timeout", type=float, default=600.0)
    args = parser.parse_args(argv)
    out = Path(args.out)
    if out.exists():
        parser.error("{0} exists; templates are never overwritten".format(out))
    work = Path(tempfile.mkdtemp(prefix="mtou-maya-prefs-", dir=str(out.parent)))
    try:
        default = first_start_prefs(args.maya, work, args.timeout)
        target = write_template(default, default.relative_to(work), args.kind, out)
    finally:
        shutil.rmtree(str(work), ignore_errors=True)
    info = describe(target.parent, out)
    print("{0}: {1} ({2} autoload plug-ins, sha256 {3})".format(
        args.kind, target, len(info["autoload"]), info["sha256"]))
    return 0


if os.environ.get(_IN_MAYA) and __name__ == "<run_path>":
    _in_maya()
elif __name__ == "__main__":
    sys.exit(main())
