"""Validate collection-owned submodule registrations without traversing products."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path, PurePosixPath
from urllib.parse import urlsplit

PROJECT_PATH = re.compile(
    r"(?:(?:maya/(?:scripts|tools)|unreal/Plugins|substance-painter/plugins)/"
    r"|composite/)[A-Z][A-Za-z0-9]*"
)


def _git(root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, check=True
    )
    return result.stdout.decode("utf-8")


def registered_submodules(
    root: Path, errors: list[str], warnings: list[str]
) -> frozenset[Path]:
    """Return only valid registered gitlink roots, initialized or otherwise."""
    root = root.resolve()
    config = root / ".gitmodules"
    try:
        git_root = Path(_git(root, "rev-parse", "--show-toplevel").strip()).resolve()
    except (OSError, subprocess.CalledProcessError, UnicodeError):
        git_root = None
    if git_root != root:
        if config.exists():
            errors.append("submodule registration requires a Git root")
        return frozenset()

    links = {}
    try:
        for record in _git(root, "ls-files", "--stage", "-z").split("\0"):
            if not record:
                continue
            header, name = record.split("\t", 1)
            mode, oid, stage = header.split()
            if mode == "160000":
                if stage != "0":
                    errors.append(f"unmerged submodule: {name}")
                else:
                    links[name] = oid
    except (OSError, subprocess.CalledProcessError, UnicodeError, ValueError) as exc:
        errors.append(f"cannot read submodule index: {exc}")
        return frozenset()

    entries = {}
    if config.exists():
        try:
            records = _git(root, "config", "--file", ".gitmodules", "--list", "--null")
            for record in records.split("\0"):
                if not record:
                    continue
                key, _, value = record.partition("\n")
                match = re.fullmatch(r"submodule\.(.+)\.(path|url)", key)
                if not match:
                    errors.append(f"unsupported .gitmodules key: {key}")
                    continue
                name, field = match.groups()
                entry = entries.setdefault(name, {})
                if field in entry:
                    errors.append(f"duplicate submodule {field}: {name}")
                entry[field] = value
        except (OSError, subprocess.CalledProcessError, UnicodeError) as exc:
            errors.append(f"cannot read .gitmodules: {exc}")

    declared = set()
    valid = set()
    for name, entry in entries.items():
        path = entry.get("path", "")
        url = entry.get("url", "")
        if not PROJECT_PATH.fullmatch(path) or PurePosixPath(path).as_posix() != path:
            errors.append(f"submodule must occupy a complete project path: {name}")
            continue
        if path.casefold() in declared:
            errors.append(f"duplicate submodule path: {path}")
            continue
        declared.add(path.casefold())
        try:
            parsed = urlsplit(url)
        except ValueError:
            errors.append(f"invalid submodule URL: {name}")
            continue
        if (parsed.scheme != "https" or not parsed.hostname or parsed.username
                or parsed.password or parsed.query or parsed.fragment
                or any(char.isspace() for char in url)):
            errors.append(f"submodule URL must be a credential-free HTTPS clone URL: {name}")
            continue
        if path not in links:
            errors.append(f"submodule registration has no staged gitlink: {path}")
            continue
        project = root / path
        if project.is_symlink() or not project.resolve().is_relative_to(root):
            errors.append(f"submodule path escapes its registered location: {path}")
            continue
        if not project.exists() or (project.is_dir() and not any(project.iterdir())):
            warnings.append(f"submodule not initialized: {path}")
            valid.add(project)
            continue
        try:
            actual_root = Path(_git(project, "rev-parse", "--show-toplevel").strip()).resolve()
            actual_head = _git(project, "rev-parse", "HEAD").strip()
            if actual_root != project.resolve():
                raise ValueError("directory is not an independent checkout")
            if actual_head != links[path]:
                errors.append(f"submodule HEAD differs from staged pin: {path}")
            valid.add(project)
        except (OSError, subprocess.CalledProcessError, UnicodeError, ValueError):
            errors.append(f"invalid submodule checkout: {path}")

    for path in links:
        if path.casefold() not in declared:
            errors.append(f"gitlink missing .gitmodules registration: {path}")
    return frozenset(valid)
