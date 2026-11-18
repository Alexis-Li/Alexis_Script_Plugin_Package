"""Create an independent agent-workflow project from templates/agent-project; dry-run unless --apply is used."""

from __future__ import annotations

import argparse
from pathlib import Path

from _repo_tools import ROOT, emit, require_pascal_name

SKELETON = ROOT / "templates" / "agent-project" / "skeleton"
# Skeleton file names that would act as live rule files here; renamed on creation.
RENAMES = {
    "AGENTS.md.tmpl": "AGENTS.md",
    "gitignore.tmpl": ".gitignore",
    "gitattributes.tmpl": ".gitattributes",
    "editorconfig.tmpl": ".editorconfig",
}
PLACEHOLDER = "{{PROJECT_NAME}}"
CUSTOMIZE_MARKER = "待定制："


def _is_git_root(directory: Path) -> bool:
    """A real repository has a .git file (worktree, submodule) or a directory with HEAD."""
    marker = directory / ".git"
    return marker.is_file() or (marker.is_dir() and (marker / "HEAD").is_file())


def _destination_path(value: str) -> Path:
    destination = Path(value).expanduser().resolve()
    root = ROOT.resolve()
    if destination == root or root in destination.parents:
        raise ValueError(
            "destination must be outside this collection repository; "
            "an independent project is its own repository"
        )
    for directory in (destination, *destination.parents):
        if _is_git_root(directory):
            raise ValueError(
                "destination is inside another Git working tree ({0}); "
                "choose a directory outside every repository".format(directory)
            )
    if destination.exists() and (not destination.is_dir() or any(destination.iterdir())):
        raise ValueError("destination must not exist or must be an empty directory")
    return destination


def _planned_files() -> list[tuple[Path, str]]:
    planned = []
    for source in sorted(path for path in SKELETON.rglob("*") if path.is_file()):
        relative = source.relative_to(SKELETON)
        if "__pycache__" in relative.parts or source.suffix in {".pyc", ".pyo"}:
            continue
        name = RENAMES.get(relative.name, relative.name)
        planned.append((source, relative.with_name(name).as_posix()))
    return planned


def _rendered(source: Path, project: str) -> bytes:
    data = source.read_bytes()
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return data
    # Stored with LF; never let a Windows checkout turn the new project into CRLF.
    return text.replace("\r\n", "\n").replace(PLACEHOLDER, project).encode("utf-8")


def create(name: str, destination: str, apply: bool = False) -> dict:
    project = require_pascal_name(name)
    target = _destination_path(destination)
    planned = _planned_files()
    if not planned:
        raise ValueError("template skeleton is empty: templates/agent-project/skeleton")
    if apply:
        for source, relative in planned:
            path = target / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(_rendered(source, project))
    return {
        "ok": True,
        "mode": "apply" if apply else "dry-run",
        "project": project,
        "destination": str(target),
        "files": [relative for _, relative in planned],
        "next_steps": [
            "cd into the destination, run git init, and commit the skeleton",
            "create the first development branch: git switch -c dev/<topic>",
            "resolve every '{0}' marker (fill it in or delete it) until "
            "python tools/validate_project.py passes".format(CUSTOMIZE_MARKER),
            "run python -m unittest discover -s tests",
        ],
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name", help="PascalCase project name, for example SampleProject.")
    parser.add_argument(
        "--destination", required=True,
        help="New repository root; must be absent or empty and outside every Git repository.",
    )
    parser.add_argument("--apply", action="store_true", help="Create files; otherwise preview only.")
    parser.add_argument("--json", action="store_true", help="Emit machine-readable output.")
    args = parser.parse_args(argv)
    try:
        result = create(args.name, args.destination, args.apply)
    except (ValueError, OSError) as exc:
        result = {"ok": False, "error": str(exc)}
        emit(result, args.json)
        return 1
    emit(result, args.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
