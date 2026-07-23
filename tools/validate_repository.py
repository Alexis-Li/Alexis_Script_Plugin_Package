"""Validate monorepo structure, project classification, and forbidden files."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path

from _repo_tools import ROOT, emit, maya_version


REQUIRED_PATHS = (
    ".github/workflows",
    ".github/ISSUE_TEMPLATE",
    "docs/development-conventions.md",
    "docs/naming-conventions.md",
    "docs/maya-development.md",
    "docs/unreal-development.md",
    "docs/release-process.md",
    "maya/AGENTS.md",
    "maya/scripts",
    "maya/tools",
    "unreal/AGENTS.md",
    "unreal/ToolsLab.uproject",
    "unreal/Config",
    "unreal/Content",
    "unreal/Source",
    "unreal/Plugins",
    "templates/maya-shelf-script",
    "templates/maya-tool",
    "templates/unreal-plugin",
    "templates/standalone-python-tool",
    "templates/maya-shelf-script/README_CN.md",
    "templates/maya-tool/README_CN.md",
    "templates/unreal-plugin/README_CN.md",
    "templates/standalone-python-tool/README_CN.md",
    "AGENTS.md",
    "README.md",
    "README_CN.md",
    "CHANGELOG.md",
    "LICENSE",
    ".gitignore",
    ".gitattributes",
    ".editorconfig",
    "pyproject.toml",
)
FORBIDDEN_DIRS = {
    "__pycache__",
    ".pytest_cache",
    ".mypy_cache",
    ".ruff_cache",
    ".venv",
    "binaries",
    "deriveddatacache",
    "intermediate",
    "saved",
    ".vs",
}
FORBIDDEN_SUFFIXES = {".pyc", ".pyo", ".sln", ".suo", ".opensdf", ".sdf", ".vc.db", ".vc.opendb", ".zip", ".7z"}
ABSOLUTE_PATH_RE = re.compile(r"(?<![A-Za-z0-9_])[A-Za-z]:[\\/]")
TEXT_SUFFIXES = {".py", ".md", ".json", ".ini", ".uplugin", ".uproject", ".cs", ".cpp", ".h", ".toml", ".yml", ".yaml"}
PASCAL_RE = re.compile(r"^[A-Z][A-Za-z0-9]*$")


def _iter_files(root: Path):
    for path in root.rglob("*"):
        if ".git" in path.parts:
            continue
        if any(part.lower() in FORBIDDEN_DIRS for part in path.relative_to(root).parts):
            continue
        yield path


def _version_control_paths(root: Path) -> list[Path]:
    """Return tracked and unignored paths, excluding ignored local outputs."""
    command = [
        "git",
        "-c",
        "safe.directory={0}".format(root.as_posix()),
        "-C",
        str(root),
        "ls-files",
        "--cached",
        "--others",
        "--exclude-standard",
        "-z",
    ]
    try:
        completed = subprocess.run(command, check=True, capture_output=True)
    except (OSError, subprocess.CalledProcessError):
        return []
    return [root / value.decode("utf-8", errors="surrogateescape") for value in completed.stdout.split(b"\0") if value]


def _validate_json(path: Path, errors: list[str], root: Path) -> None:
    try:
        json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        errors.append("invalid JSON {0}: {1}".format(path.relative_to(root).as_posix(), exc))


def _validate_shelf_scripts(root: Path, errors: list[str]) -> None:
    scripts_root = root / "maya" / "scripts"
    for project in sorted(path for path in scripts_root.iterdir() if path.is_dir()):
        if not PASCAL_RE.fullmatch(project.name):
            errors.append("Maya project directory must use PascalCase: {0}".format(project.name))
        files = [path for path in project.iterdir() if path.is_file() and path.name != ".gitkeep"]
        python_files = [path for path in files if path.suffix == ".py"]
        if (
            len(python_files) != 1
            or not (project / "README.md").is_file()
            or not (project / "README_CN.md").is_file()
        ):
            errors.append(
                "shelf script {0} must contain one .py file, README.md, and README_CN.md".format(
                    project.name
                )
            )
            continue
        if python_files[0].stem != project.name:
            errors.append("shelf script file must match project name: {0}.py".format(project.name))
        unexpected = [
            path.name
            for path in project.iterdir()
            if path.is_dir() and path.name.lower() not in FORBIDDEN_DIRS
        ]
        if unexpected:
            errors.append("shelf script {0} contains directories: {1}".format(project.name, ", ".join(unexpected)))
        source = python_files[0].read_text(encoding="utf-8-sig")
        if "def run(" not in source and "def main(" not in source:
            errors.append("shelf script {0} must expose run() or main()".format(project.name))


def _validate_maya_tools(root: Path, errors: list[str]) -> None:
    tools_root = root / "maya" / "tools"
    for project in sorted(path for path in tools_root.iterdir() if path.is_dir()):
        if not PASCAL_RE.fullmatch(project.name):
            errors.append("Maya project directory must use PascalCase: {0}".format(project.name))
        for required in ("README.md", "README_CN.md", "CHANGELOG.md", "LICENSE"):
            if not (project / required).exists():
                errors.append("Maya tool {0} is missing {1}".format(project.name, required))
        if (project / "package").exists():
            errors.append("Maya tool {0} contains obsolete package/ nesting".format(project.name))
        runtime_dirs = [project / name for name in ("scripts", "plug-ins", "icons")]
        if not any(path.is_dir() for path in runtime_dirs):
            errors.append("Maya tool {0} needs scripts/, plug-ins/, or icons/".format(project.name))
        for runtime_dir in runtime_dirs[:2]:
            if not runtime_dir.is_dir():
                continue
            for runtime_file in runtime_dir.glob("*.py"):
                if not PASCAL_RE.fullmatch(runtime_file.stem):
                    errors.append("Maya runtime file must use PascalCase: {0}".format(runtime_file.name))
        try:
            maya_version(project)
        except (OSError, UnicodeError, ValueError) as exc:
            errors.append("Maya tool {0}: {1}".format(project.name, exc))


def _validate_unreal(root: Path, errors: list[str]) -> None:
    project_file = root / "unreal" / "ToolsLab.uproject"
    if project_file.is_file():
        _validate_json(project_file, errors, root)
    plugins_root = root / "unreal" / "Plugins"
    for plugin in sorted(path for path in plugins_root.iterdir() if path.is_dir()):
        for required in ("README.md", "README_CN.md"):
            if not (plugin / required).is_file():
                errors.append("Unreal plugin {0} is missing {1}".format(plugin.name, required))
        descriptors = list(plugin.glob("*.uplugin"))
        if len(descriptors) != 1:
            errors.append("Unreal plugin {0} must contain exactly one root .uplugin".format(plugin.name))
            continue
        _validate_json(descriptors[0], errors, root)


def validate(root: Path = ROOT) -> dict:
    root = root.resolve()
    errors: list[str] = []
    warnings: list[str] = []
    for relative_path in REQUIRED_PATHS:
        if not (root / relative_path).exists():
            errors.append("missing required path: {0}".format(relative_path))

    for path in _iter_files(root):
        relative_path = path.relative_to(root).as_posix()
        if path.is_file() and path.suffix.lower() in TEXT_SUFFIXES and "workspace-spec.md" not in relative_path:
            try:
                text = path.read_text(encoding="utf-8-sig")
            except UnicodeError:
                errors.append("text file is not UTF-8: {0}".format(relative_path))
            else:
                if ABSOLUTE_PATH_RE.search(text):
                    errors.append("machine-specific absolute path in {0}".format(relative_path))

    for path in _version_control_paths(root):
        relative_parts = path.relative_to(root).parts
        relative_path = path.relative_to(root).as_posix()
        if any(part.lower() in FORBIDDEN_DIRS for part in relative_parts):
            errors.append("version-control-visible generated path: {0}".format(relative_path))
        if path.name.lower().endswith(tuple(FORBIDDEN_SUFFIXES)):
            errors.append("version-control-visible generated file: {0}".format(relative_path))

    root_git = root / ".git"
    for nested_git in root.rglob(".git"):
        if nested_git != root_git:
            errors.append("nested Git repository: {0}".format(nested_git.relative_to(root).as_posix()))

    if (root / "maya" / "scripts").is_dir():
        _validate_shelf_scripts(root, errors)
    if (root / "maya" / "tools").is_dir():
        _validate_maya_tools(root, errors)
    if (root / "unreal" / "Plugins").is_dir():
        _validate_unreal(root, errors)

    return {"ok": not errors, "errors": sorted(set(errors)), "warnings": sorted(set(warnings))}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT, help="Repository root to validate.")
    parser.add_argument("--json", action="store_true", help="Emit machine-readable output.")
    args = parser.parse_args(argv)
    result = validate(args.root)
    emit(result, args.json)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
