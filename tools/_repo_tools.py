"""Shared standard-library helpers for repository management commands."""

from __future__ import annotations

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEMVER_RE = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:[-+][0-9A-Za-z.-]+)?$")
KEBAB_RE = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")
PASCAL_RE = re.compile(r"^[A-Z][A-Za-z0-9]*$")
MAYA_VERSION_RE = re.compile(
    r'((?:__version__|PLUGIN_VERSION)\s*=\s*["\'])' r'([^"\']+)(["\'])'
)


def emit(payload: dict, as_json: bool) -> None:
    if as_json:
        print(json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True))
        return
    for key, value in payload.items():
        if isinstance(value, list):
            print("{0}:".format(key))
            for item in value:
                print("  - {0}".format(item))
        else:
            print("{0}: {1}".format(key, value))


def require_kebab_name(value: str) -> str:
    if not KEBAB_RE.fullmatch(value):
        raise ValueError("name must use lowercase kebab-case")
    return value


def require_pascal_name(value: str) -> str:
    if not PASCAL_RE.fullmatch(value):
        raise ValueError("name must use PascalCase letters and numbers")
    return value


def require_semver(value: str) -> str:
    if not SEMVER_RE.fullmatch(value):
        raise ValueError("version must be Semantic Versioning, for example 1.2.3")
    return value


def _component_location(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return path.name


def resolve_component(label: str, candidates: tuple[Path, ...]) -> Path:
    matches = [path for path in candidates if path.is_dir()]
    if not matches:
        raise ValueError("{0} not found".format(label))
    if len(matches) > 1:
        locations = ", ".join(_component_location(path) for path in matches)
        raise ValueError("ambiguous {0}: {1}".format(label, locations))
    return matches[0]


def maya_tool_path(name: str) -> Path:
    name = require_pascal_name(name)
    return resolve_component(
        "Maya tool",
        (
            ROOT / "maya" / "tools" / name,
            ROOT / "composite" / name / "maya" / name,
        ),
    )


def unreal_plugin_path(name: str) -> Path:
    name = require_pascal_name(name)
    return resolve_component(
        "Unreal plugin",
        (
            ROOT / "unreal" / "Plugins" / name,
            ROOT / "composite" / name / "unreal" / name,
        ),
    )


def maya_runtime_files(project: Path) -> list[Path]:
    files = []
    for directory in (project / "scripts", project / "plug-ins"):
        if directory.is_dir():
            files.extend(directory.rglob("*.py"))
    return sorted(files)


def maya_version(project: Path) -> str:
    versions = {
        match.group(2)
        for path in maya_runtime_files(project)
        for match in MAYA_VERSION_RE.finditer(path.read_text(encoding="utf-8-sig"))
    }
    if len(versions) != 1:
        raise ValueError("Maya tool must declare one consistent runtime version")
    return require_semver(versions.pop())


def pascal_case(kebab_name: str) -> str:
    return "".join(part[:1].upper() + part[1:] for part in kebab_name.split("-"))


def snake_case(kebab_name: str) -> str:
    return kebab_name.replace("-", "_")


def relative(path: Path) -> str:
    return path.resolve().relative_to(ROOT).as_posix()
