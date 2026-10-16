"""Validate monorepo structure, project classification, and forbidden files."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
import subprocess
from pathlib import Path

from _repo_tools import ROOT, emit, maya_version, substance_painter_version
from issue_report import validate_archive
from submodule_registry import linked_worktrees, registered_submodules

REQUIRED_PATHS = (
    ".github/workflows",
    ".github/ISSUE_TEMPLATE",
    "docs/development-conventions.md",
    "docs/naming-conventions.md",
    "docs/maya-development.md",
    "docs/unreal-development.md",
    "docs/release-process.md",
    "docs/project-history/README.md",
    "docs/proposals/README.md",
    "docs/agents/issue-tracker.md",
    "tools/issue_report.py",
    "maya/AGENTS.md",
    "maya/scripts",
    "maya/tools",
    "unreal/AGENTS.md",
    "unreal/ToolsLab.uproject",
    "unreal/Config",
    "unreal/Content",
    "unreal/Source",
    "unreal/Plugins",
    "substance-painter/AGENTS.md",
    "substance-painter/plugins",
    "composite/AGENTS.md",
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
FORBIDDEN_SUFFIXES = {
    ".pyc",
    ".pyo",
    ".sln",
    ".suo",
    ".opensdf",
    ".sdf",
    ".vc.db",
    ".vc.opendb",
    ".zip",
    ".7z",
}
ABSOLUTE_PATH_RE = re.compile(r"(?<![A-Za-z0-9_])[A-Za-z]:[\\/]")
TEXT_SUFFIXES = {
    ".py",
    ".md",
    ".json",
    ".ini",
    ".uplugin",
    ".uproject",
    ".cs",
    ".cpp",
    ".h",
    ".toml",
    ".yml",
    ".yaml",
}
PASCAL_RE = re.compile(r"^[A-Z][A-Za-z0-9]*$")
KEBAB_RE = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")
STANDALONE_METADATA = ("README.md", "README_CN.md", "CHANGELOG.md", "LICENSE")
COMPOSITE_ROOT_METADATA = (
    "README.md",
    "README_CN.md",
    "CHANGELOG.md",
    "LICENSE",
)
COMPOSITE_COMPONENT_METADATA = COMPOSITE_ROOT_METADATA


def _iter_files(root: Path, excluded: frozenset[Path] = frozenset()):
    for current, directories, files in os.walk(root):
        base = Path(current)
        directories[:] = [name for name in directories
                          if name not in {".git", ".tmp", ".worktrees"}
                          and name.lower() not in FORBIDDEN_DIRS
                          and base / name not in excluded]
        for name in directories + files:
            path = base / name
            if name != ".git" and path not in excluded:
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
    return [
        root / value.decode("utf-8", errors="surrogateescape")
        for value in completed.stdout.split(b"\0")
        if value
    ]


def _validate_json(path: Path, errors: list[str], root: Path) -> None:
    try:
        json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        errors.append("invalid JSON {0}: {1}".format(path.relative_to(root).as_posix(), exc))


def _validate_shelf_scripts(
    root: Path, errors: list[str], excluded: frozenset[Path] = frozenset()
) -> None:
    scripts_root = root / "maya" / "scripts"
    for project in sorted(path for path in scripts_root.iterdir() if path.is_dir()):
        if project in excluded:
            continue
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
            if path.is_dir() and path.name != "tests" and path.name.lower() not in FORBIDDEN_DIRS
        ]
        if unexpected:
            errors.append(
                "shelf script {0} contains directories: {1}".format(
                    project.name, ", ".join(unexpected)
                )
            )
        source = python_files[0].read_text(encoding="utf-8-sig")
        if "def run(" not in source and "def main(" not in source:
            errors.append("shelf script {0} must expose run() or main()".format(project.name))


def _validate_maya_project(
    project: Path, errors: list[str], root: Path, require_metadata: bool = True
) -> None:
    if not PASCAL_RE.fullmatch(project.name):
        errors.append("Maya project directory must use PascalCase: {0}".format(project.name))
    if require_metadata:
        for required in STANDALONE_METADATA:
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
                errors.append(
                    "Maya runtime file must use PascalCase: {0}".format(runtime_file.name)
                )
    try:
        maya_version(project)
    except (OSError, UnicodeError, ValueError) as exc:
        errors.append("Maya tool {0}: {1}".format(project.name, exc))


def _validate_maya_tools(
    root: Path, errors: list[str], excluded: frozenset[Path] = frozenset()
) -> None:
    tools_root = root / "maya" / "tools"
    for project in sorted(path for path in tools_root.iterdir() if path.is_dir()):
        if project in excluded:
            continue
        _validate_maya_project(project, errors, root)


def _validate_unreal_plugin(
    plugin: Path, errors: list[str], root: Path, require_metadata: bool = True
) -> None:
    if not PASCAL_RE.fullmatch(plugin.name):
        errors.append("Unreal plugin directory must use PascalCase: {0}".format(plugin.name))
    if require_metadata:
        for required in ("README.md", "README_CN.md"):
            if not (plugin / required).is_file():
                errors.append("Unreal plugin {0} is missing {1}".format(plugin.name, required))
    descriptors = list(plugin.glob("*.uplugin"))
    if len(descriptors) != 1:
        errors.append(
            "Unreal plugin {0} must contain exactly one root .uplugin".format(plugin.name)
        )
        return
    _validate_json(descriptors[0], errors, root)


def _validate_unreal(
    root: Path, errors: list[str], excluded: frozenset[Path] = frozenset()
) -> None:
    project_file = root / "unreal" / "ToolsLab.uproject"
    if project_file.is_file():
        _validate_json(project_file, errors, root)
    plugins_root = root / "unreal" / "Plugins"
    for plugin in sorted(path for path in plugins_root.iterdir() if path.is_dir()):
        if plugin in excluded:
            continue
        _validate_unreal_plugin(plugin, errors, root)


def _validate_substance_painter_project(
    project: Path, errors: list[str], root: Path, require_metadata: bool = True
) -> None:
    if not PASCAL_RE.fullmatch(project.name):
        errors.append(
            "Substance Painter project directory must use PascalCase: {0}".format(project.name)
        )
    if require_metadata:
        for required in STANDALONE_METADATA:
            if not (project / required).is_file():
                errors.append(
                    "Substance Painter plug-in {0} is missing {1}".format(project.name, required)
                )
    if not any((project / name).is_dir() for name in ("plugins", "startup")):
        errors.append(
            "Substance Painter plug-in {0} needs plugins/ or startup/".format(project.name)
        )
        return
    try:
        substance_painter_version(project)
    except (OSError, UnicodeError, ValueError) as exc:
        errors.append("Substance Painter plug-in {0}: {1}".format(project.name, exc))


def _validate_substance_painter(
    root: Path, errors: list[str], excluded: frozenset[Path] = frozenset()
) -> None:
    plugins_root = root / "substance-painter" / "plugins"
    for project in sorted(path for path in plugins_root.iterdir() if path.is_dir()):
        if project in excluded:
            continue
        _validate_substance_painter_project(project, errors, root)


def _validate_proposals(root: Path, errors: list[str]) -> None:
    proposals_root = root / "docs" / "proposals"
    if not proposals_root.is_dir():
        return
    for proposal in sorted(path for path in proposals_root.iterdir() if path.is_dir()):
        if not KEBAB_RE.fullmatch(proposal.name):
            errors.append(
                "proposal directory must use lowercase kebab-case: {0}".format(proposal.name)
            )
        if not (proposal / "requirements.md").is_file():
            errors.append("proposal {0} is missing requirements.md".format(proposal.name))


def _validate_composite_component_metadata(
    project: Path, host: Path, component: Path, errors: list[str]
) -> None:
    for metadata in COMPOSITE_COMPONENT_METADATA:
        if (component / metadata).exists():
            errors.append(
                "Composite component {0}/{1}/{2} must not contain {3}; "
                "metadata belongs at the project root".format(
                    project.name, host.name, component.name, metadata
                )
            )


def _validate_composite(
    root: Path, errors: list[str], excluded: frozenset[Path] = frozenset()
) -> None:
    composite_root = root / "composite"
    if not composite_root.is_dir():
        return
    for project in sorted(path for path in composite_root.iterdir() if path.is_dir()):
        if project in excluded:
            continue
        if not PASCAL_RE.fullmatch(project.name):
            errors.append(
                "Composite project directory must use PascalCase: {0}".format(project.name)
            )
        for required in COMPOSITE_ROOT_METADATA:
            if not (project / required).is_file():
                errors.append("Composite project {0} is missing {1}".format(project.name, required))
        directories = sorted(path for path in project.iterdir() if path.is_dir())
        for directory in directories:
            if not KEBAB_RE.fullmatch(directory.name):
                errors.append(
                    "Composite directory must use lowercase kebab-case: {0}/{1}".format(
                        project.name, directory.name
                    )
                )
        hosts = [path for path in directories if path.name in HOST_COMPONENT_VALIDATORS]
        if len(hosts) < 2:
            errors.append(
                "Composite project {0} must contain at least two host directories ({1})".format(
                    project.name, ", ".join(sorted(HOST_COMPONENT_VALIDATORS))
                )
            )
        for host in hosts:
            components = sorted(path for path in host.iterdir() if path.is_dir())
            if not components:
                errors.append(
                    "Composite host {0}/{1} must contain a component root".format(
                        project.name, host.name
                    )
                )
            for component in components:
                _validate_composite_component_metadata(project, host, component, errors)
                HOST_COMPONENT_VALIDATORS[host.name](
                    component, errors, root, require_metadata=False
                )


HOST_COMPONENT_VALIDATORS = {
    "maya": _validate_maya_project,
    "unreal": _validate_unreal_plugin,
    "substance-painter": _validate_substance_painter_project,
}


def _validate_mtou_conformance(root: Path, errors: list[str]) -> None:
    protocol_root = root / "composite" / "MtoULiveLink" / "protocol"
    generator_path = protocol_root / "generate_unreal_corpus.py"
    if not generator_path.is_file():
        return
    spec = importlib.util.spec_from_file_location("generate_mtou_unreal_corpus", generator_path)
    if spec is None or spec.loader is None:
        errors.append("cannot load MtoU conformance generator")
        return
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
        errors.extend(module.check_generated(
            protocol_root / "conformance-v10.json",
            root / "composite" / "MtoULiveLink" / "unreal" / "MtoULiveLink"
            / "Source" / "MtoULiveLink" / "Private" / "Tests"
            / "MtoUConformanceCorpus.inl",
        ))
    except (OSError, UnicodeError, ValueError) as exc:
        errors.append("invalid MtoU conformance corpus: {0}".format(exc))


def _validate_nested_git(
    root: Path, errors: list[str], excluded: frozenset[Path] = frozenset()
) -> None:
    for current, directories, files in os.walk(root):
        base = Path(current)
        if base != root and ".git" in directories + files:
            errors.append("nested Git repository: {0}".format(
                (base / ".git").relative_to(root).as_posix()))
        directories[:] = [name for name in directories
                          if name not in {".git", ".tmp", ".worktrees"}
                          and base / name not in excluded]


def _validate_issue_archives(root: Path, errors: list[str]) -> None:
    for path in sorted((root / "docs" / "project-history").rglob("*comment-archive.json")):
        try:
            archive = json.loads(path.read_text(encoding="utf-8-sig"))
            problems = validate_archive(archive)
        except (OSError, UnicodeError, ValueError) as exc:
            problems = [str(exc)]
        errors.extend(f"{path.relative_to(root).as_posix()}: {problem}" for problem in problems)


def _path_policy_text(relative_path: str, text: str) -> str:
    """Exclude only preserved source bodies in structurally valid issue archives."""
    if relative_path.startswith("docs/project-history/") and relative_path.endswith(
        "comment-archive.json"
    ):
        try:
            archive = json.loads(text)
        except ValueError:
            return text
        if not validate_archive(archive):
            # Historical comments must remain exact, including old machine paths.
            # Metadata and all other files still obey the portable-path policy.
            for comment in archive["comments"]:
                comment.pop("body")
            return json.dumps(archive, ensure_ascii=False)
    return text


def validate(root: Path = ROOT) -> dict:
    root = root.resolve()
    errors: list[str] = []
    warnings: list[str] = []
    for relative_path in REQUIRED_PATHS:
        if not (root / relative_path).exists():
            errors.append("missing required path: {0}".format(relative_path))

    excluded = registered_submodules(root, errors, warnings) | linked_worktrees(root)

    for path in _iter_files(root, excluded):
        relative_path = path.relative_to(root).as_posix()
        if (
            path.is_file()
            and path.suffix.lower() in TEXT_SUFFIXES
            and "workspace-spec.md" not in relative_path
        ):
            try:
                text = path.read_text(encoding="utf-8-sig")
            except UnicodeError:
                errors.append("text file is not UTF-8: {0}".format(relative_path))
            else:
                if ABSOLUTE_PATH_RE.search(_path_policy_text(relative_path, text)):
                    errors.append("machine-specific absolute path in {0}".format(relative_path))

    for path in _version_control_paths(root):
        relative_parts = path.relative_to(root).parts
        relative_path = path.relative_to(root).as_posix()
        if any(part.lower() in FORBIDDEN_DIRS for part in relative_parts):
            errors.append("version-control-visible generated path: {0}".format(relative_path))
        if path.name.lower().endswith(tuple(FORBIDDEN_SUFFIXES)):
            errors.append("version-control-visible generated file: {0}".format(relative_path))

    _validate_nested_git(root, errors, excluded)
    _validate_issue_archives(root, errors)

    if (root / "maya" / "scripts").is_dir():
        _validate_shelf_scripts(root, errors, excluded)
    if (root / "maya" / "tools").is_dir():
        _validate_maya_tools(root, errors, excluded)
    if (root / "unreal" / "Plugins").is_dir():
        _validate_unreal(root, errors, excluded)
    if (root / "substance-painter" / "plugins").is_dir():
        _validate_substance_painter(root, errors, excluded)
    _validate_composite(root, errors, excluded)
    _validate_proposals(root, errors)
    if root / "composite" / "MtoULiveLink" not in excluded:
        _validate_mtou_conformance(root, errors)

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
