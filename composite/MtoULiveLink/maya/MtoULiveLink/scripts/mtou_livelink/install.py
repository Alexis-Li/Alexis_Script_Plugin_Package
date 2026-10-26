"""Register an extracted Maya component as the MtoULiveLink Maya module.

`install.mel` at the component root loads this file by path and calls
`install_from_maya()`. The file uses only the standard library and Maya, never
the rest of `mtou_livelink`, so it also runs when another MtoU version is
already imported in the session.

Maya 2024 ignores a `.mod` entry whose path contains non-ASCII characters, so
a component extracted under such a path is copied into the Maya `modules`
folder and registered there by a relative path.
"""

import datetime
import importlib
import os
import re
import shutil
import sys

MODULE_NAME = "MtoULiveLink"
ENTRY_FILE = "MtoULiveLink.py"
PACKAGE_DIR = "mtou_livelink"
MOD_FILE = MODULE_NAME + ".mod"
COPY_MARKER = ".mtou_module_copy"
BACKUP_DIR = MODULE_NAME + "_backup"
SHELF_COMMAND = "import MtoULiveLink\nMtoULiveLink.run()"
SHELF_LABEL = "MtoU"
TITLE = "MtoU_LiveLink"

_VERSION_RE = re.compile(r'^__version__\s*=\s*["\']([^"\']+)["\']', re.MULTILINE)
_OPENS_TOOL_RE = re.compile(r"MtoULiveLink\.(run|main)\s*\(")


class InstallError(Exception):
    """The component cannot be installed; nothing was written."""


class InstallPlan(object):
    """What one installation writes, moves and removes."""

    def __init__(self, root, version, modules_dir, module_path, copy_dir, conflicts,
                 remove_copy):
        self.root = root
        self.version = version
        self.mod_path = os.path.join(modules_dir, MOD_FILE)
        self.mod_text = "+ {0} {1} {2}\n".format(MODULE_NAME, version, module_path)
        # Folder whose `scripts` Maya loads: the extracted component or its copy.
        self.module_dir = os.path.normpath(copy_dir or os.path.join(modules_dir, module_path))
        self.scripts_dir = os.path.join(self.module_dir, "scripts")
        self.copy_dir = copy_dir
        self.conflicts = conflicts
        self.remove_copy = remove_copy


def _norm(path):
    return os.path.normcase(os.path.normpath(os.path.abspath(path)))


def _inside(path, folder):
    path, folder = _norm(path), _norm(folder)
    return path == folder or path.startswith(folder.rstrip(os.sep) + os.sep)


def _unique(paths):
    seen = set()
    result = []
    for path in paths:
        if _norm(path) not in seen:
            seen.add(_norm(path))
            result.append(path)
    return result


def _is_ascii(text):
    try:
        text.encode("ascii")
    except UnicodeEncodeError:
        return False
    return True


def component_version(root):
    """Return the version of a complete extracted component or raise InstallError."""
    scripts = os.path.join(root, "scripts")
    required = (os.path.join(scripts, ENTRY_FILE),
                os.path.join(scripts, PACKAGE_DIR, "__init__.py"),
                os.path.join(scripts, PACKAGE_DIR, "config.py"))
    missing = [path for path in required if not os.path.isfile(path)]
    if missing:
        raise InstallError(
            "安装包不完整：找不到 {0}。请重新完整解压 Maya 组件后再拖入 install.mel。\n"
            "Incomplete package: {0} is missing. Extract the whole Maya component "
            "again, then drag install.mel in.".format(
                os.path.relpath(missing[0], root).replace(os.sep, "/")))
    with open(required[2], encoding="utf-8") as handle:
        match = _VERSION_RE.search(handle.read())
    if not match:
        raise InstallError("无法读取 MtoU_LiveLink 版本号。\n"
                           "The MtoU_LiveLink version cannot be read.")
    return match.group(1)


def declared_module(mod_text):
    """Names of the modules a `.mod` file declares."""
    names = []
    for line in mod_text.splitlines():
        tokens = line.split()
        if not tokens or tokens[0] != "+":
            continue
        # Conditions such as MAYAVERSION:2024 precede the module name.
        name = next((token for token in tokens[1:] if ":" not in token), None)
        if name:
            names.append(name)
    return names


def _is_managed_copy(folder):
    return os.path.isfile(os.path.join(folder, COPY_MARKER))


def plan_install(root, modules_dir, script_dirs, module_dirs):
    """Decide the `.mod`, any copy and the conflicting earlier installs."""
    root = os.path.abspath(root)
    version = component_version(root)
    copy_target = os.path.join(modules_dir, MODULE_NAME)
    copy_dir = None
    if _is_ascii(root):
        module_path = root.replace(os.sep, "/")
    elif _norm(os.path.dirname(root)) == _norm(modules_dir) and _is_ascii(
            os.path.basename(root)):
        module_path = os.path.basename(root)
    else:
        if _inside(root, copy_target):
            raise InstallError(
                "请把 Maya 组件解压到 {0} 以外的文件夹后再拖入 install.mel。\n"
                "Extract the Maya component outside {0}, then drag install.mel in.".format(
                    copy_target))
        copy_dir = copy_target
        module_path = MODULE_NAME
    plan =InstallPlan(root, version, modules_dir, module_path, copy_dir, [], False)
    module_dir = plan.module_dir
    plan.remove_copy = (copy_dir is None and os.path.isdir(copy_target)
                        and _is_managed_copy(copy_target) and not _inside(module_dir, copy_target))

    conflicts = []
    for folder in script_dirs:
        for name in (ENTRY_FILE, PACKAGE_DIR):
            path = os.path.join(folder, name)
            if os.path.exists(path) and not _inside(path, root) and not _inside(path, module_dir):
                conflicts.append(path)
    mod_path = os.path.join(modules_dir, MOD_FILE)
    for folder in module_dirs:
        if not os.path.isdir(folder):
            continue
        for name in sorted(os.listdir(folder)):
            path = os.path.join(folder, name)
            if not name.lower().endswith(".mod") or _norm(path) == _norm(mod_path):
                continue
            try:
                with open(path, encoding="utf-8", errors="replace") as handle:
                    declared = declared_module(handle.read())
            except OSError:
                continue
            if MODULE_NAME in declared:
                conflicts.append(path)
    if copy_dir and os.path.exists(copy_dir) and not _is_managed_copy(copy_dir):
        conflicts.append(copy_dir)
    plan.conflicts = _unique(conflicts)
    return plan


def apply_install(plan, backup_root, now=None):
    """Move conflicts to a backup, write the copy and `.mod`; return the backup folder."""
    backup = None
    if plan.conflicts:
        stamp = (now or datetime.datetime.now()).strftime("%Y%m%d-%H%M%S")
        backup = os.path.join(backup_root, stamp)
        suffix = 1
        while os.path.exists(backup):
            suffix += 1
            backup = os.path.join(backup_root, "{0}-{1}".format(stamp, suffix))
        os.makedirs(backup)
        lines = []
        for index, path in enumerate(plan.conflicts, 1):
            target = os.path.join(backup, "{0:02d}_{1}".format(index, os.path.basename(path)))
            shutil.move(path, target)
            lines.append("{0}\t{1}".format(os.path.basename(target), path))
        with open(os.path.join(backup, "sources.txt"), "w", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")
    if plan.remove_copy:
        shutil.rmtree(os.path.join(os.path.dirname(plan.mod_path), MODULE_NAME))
    if plan.copy_dir:
        if os.path.isdir(plan.copy_dir):
            shutil.rmtree(plan.copy_dir)
        shutil.copytree(os.path.join(plan.root, "scripts"), plan.scripts_dir,
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        with open(os.path.join(plan.copy_dir, COPY_MARKER), "w", encoding="utf-8") as handle:
            handle.write(plan.root + "\n")
    os.makedirs(os.path.dirname(plan.mod_path), exist_ok=True)
    with open(plan.mod_path, "w", encoding="ascii", newline="\n") as handle:
        handle.write(plan.mod_text)
    return backup


def opens_tool(command):
    """True when a shelf command opens MtoU_LiveLink."""
    return bool(command) and "MtoULiveLink" in command and bool(_OPENS_TOOL_RE.search(command))


def saved_shelf_has_button(shelf_dir):
    """True when a saved shelf file already holds an MtoU button."""
    if not shelf_dir or not os.path.isdir(shelf_dir):
        return False
    for name in os.listdir(shelf_dir):
        if not name.lower().endswith(".mel"):
            continue
        try:
            with open(os.path.join(shelf_dir, name), encoding="utf-8", errors="replace") as handle:
                if opens_tool(handle.read()):
                    return True
        except OSError:
            continue
    return False


def loaded_elsewhere(scripts_dir, modules=None):
    """File of an MtoU module this session imported from another folder, if any."""
    modules = sys.modules if modules is None else modules
    for name in (MODULE_NAME, PACKAGE_DIR):
        loaded = getattr(modules.get(name), "__file__", None)
        if loaded and not _inside(loaded, scripts_dir):
            return loaded
    return None


def activate(plan, modules=None, path=None):
    """Make the module importable now; return the file that requires a restart, if any."""
    other = loaded_elsewhere(plan.scripts_dir, modules)
    if other:
        return other
    path = sys.path if path is None else path
    if _norm(plan.scripts_dir) not in [_norm(entry) for entry in path if entry]:
        path.insert(0, plan.scripts_dir)
    importlib.invalidate_caches()
    return None


# ---------------------------------------------------------------- Maya side
def _maya_locations(cmds):
    app_dir = os.path.normpath(cmds.internalVar(userAppDir=True))
    script_dirs = [os.path.join(app_dir, "scripts"),
                   os.path.normpath(cmds.internalVar(userScriptDir=True))]
    script_dirs += [entry for entry in sys.path if entry and _inside(entry, app_dir)]
    module_dirs = [os.path.join(app_dir, "modules")]
    module_dirs += [entry for entry in os.environ.get("MAYA_MODULE_PATH", "").split(os.pathsep)
                    if entry and _inside(entry, app_dir)]
    return app_dir, _unique(script_dirs), _unique(module_dirs)


def _ensure_shelf_button(cmds, mel):
    """Keep an existing MtoU button or add one to the current shelf; return its state."""
    top = mel.eval("$mtouShelfTop = $gShelfTopLevel")
    for shelf in cmds.tabLayout(top, query=True, childArray=True) or []:
        for button in cmds.shelfLayout(shelf, query=True, childArray=True) or []:
            if cmds.shelfButton(button, exists=True) and opens_tool(
                    cmds.shelfButton(button, query=True, command=True)):
                return "kept"
    if saved_shelf_has_button(cmds.internalVar(userShelfDir=True)):
        return "kept"
    shelf = cmds.tabLayout(top, query=True, selectTab=True)
    cmds.shelfButton(parent=shelf, label=SHELF_LABEL, imageOverlayLabel=SHELF_LABEL,
                     annotation=TITLE, image="pythonFamily.png", sourceType="python",
                     command=SHELF_COMMAND)
    mel.eval("saveAllShelves $gShelfTopLevel")
    return "added"


def _message(plan, restart_file, shelf, backup):
    lines = ["MtoU_LiveLink {0} 已安装。".format(plan.version)]
    if restart_file:
        lines.append("当前 Maya 已加载其他位置的 MtoU（{0}），请重启 Maya 后再打开。".format(
            restart_file))
    if shelf == "added":
        lines.append("已在当前工具架添加 MtoU 按钮。")
    elif shelf == "kept":
        lines.append("沿用工具架上已有的 MtoU 按钮。")
    if plan.copy_dir:
        lines.append("解压路径包含中文等非英文字符，Maya 无法从该路径加载，已将工具复制到 {0}；"
                     "解压目录可以删除。".format(plan.copy_dir))
    else:
        lines.append("请保留解压目录 {0}：移动或删除它会使工具无法加载。".format(plan.module_dir))
    if backup:
        lines.append("旧版安装已移到 {0}。".format(backup))
    return "\n".join(lines)


def install_from_maya(root):
    """Install from a Maya session; called by install.mel. Returns the result."""
    import maya.cmds as cmds
    import maya.mel as mel

    interactive = not cmds.about(batch=True)

    def tell(message):
        if interactive:
            cmds.confirmDialog(title=TITLE, message=message, button=["OK"])
        else:
            print(message)

    try:
        app_dir, script_dirs, module_dirs = _maya_locations(cmds)
        plan = plan_install(root, os.path.join(app_dir, "modules"), script_dirs, module_dirs)
    except InstallError as error:
        tell(str(error))
        return {"installed": False, "error": str(error)}

    if plan.conflicts and interactive:
        listed = "\n".join("  " + path for path in plan.conflicts)
        answer = cmds.confirmDialog(
            title=TITLE, button=["移到备份并继续", "取消"], defaultButton="移到备份并继续",
            cancelButton="取消", dismissString="取消",
            message="发现会与新版本冲突的旧安装：\n{0}\n\n继续会把它们移到 {1}，"
                    "之后可从那里恢复。".format(listed, os.path.join(app_dir, BACKUP_DIR)))
        if answer != "移到备份并继续":
            return {"installed": False, "cancelled": True, "conflicts": plan.conflicts}

    try:
        backup = apply_install(plan, os.path.join(app_dir, BACKUP_DIR))
    except OSError as error:
        message = "安装失败：{0}\nInstallation failed: {0}".format(error)
        tell(message)
        return {"installed": False, "error": message}
    restart_file = activate(plan)
    shelf = _ensure_shelf_button(cmds, mel) if interactive else None
    message = _message(plan, restart_file, shelf, backup)
    tell(message)
    return {"installed": True, "version": plan.version, "mod_path": plan.mod_path,
            "mod_text": plan.mod_text, "module_dir": plan.module_dir,
            "copied": bool(plan.copy_dir), "backup": backup, "conflicts": plan.conflicts,
            "restart_required": restart_file, "shelf": shelf, "message": message}
