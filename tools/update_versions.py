"""Update one project's declared version; dry-run unless --apply is used."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from _repo_tools import (
    MAYA_VERSION_RE,
    ROOT,
    emit,
    maya_runtime_files,
    require_pascal_name,
    require_semver,
)


def _maya_changes(name: str, version: str) -> list[tuple[Path, str]]:
    project = ROOT / "maya" / "tools" / require_pascal_name(name)
    changes = []
    for runtime_file in maya_runtime_files(project):
        runtime_text = runtime_file.read_text(encoding="utf-8-sig")
        replaced, count = MAYA_VERSION_RE.subn(
            lambda match: match.group(1) + version + match.group(3),
            runtime_text,
        )
        if count:
            changes.append((runtime_file, replaced))
    if not changes:
        raise ValueError("Maya tool must declare __version__ or PLUGIN_VERSION")
    return changes


def _unreal_changes(name: str, version: str) -> list[tuple[Path, str]]:
    if not name or not name[0].isupper() or not name.isalnum():
        raise ValueError("Unreal plugin name must be PascalCase letters and numbers")
    descriptor = ROOT / "unreal" / "Plugins" / name / (name + ".uplugin")
    if not descriptor.is_file():
        raise ValueError("Unreal plugin descriptor not found")
    metadata = json.loads(descriptor.read_text(encoding="utf-8-sig"))
    metadata["VersionName"] = version
    metadata["Version"] = int(metadata.get("Version", 0)) + 1
    return [(descriptor, json.dumps(metadata, ensure_ascii=False, indent=2) + "\n")]


def update(kind: str, name: str, version: str, apply: bool = False) -> dict:
    require_semver(version)
    changes = _maya_changes(name, version) if kind == "maya" else _unreal_changes(name, version)
    if apply:
        for path, text in changes:
            path.write_text(text, encoding="utf-8", newline="\n")
    return {
        "ok": True,
        "mode": "apply" if apply else "dry-run",
        "version": version,
        "files": [path.relative_to(ROOT).as_posix() for path, _ in changes],
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=("maya", "unreal"))
    parser.add_argument("name", help="Maya or Unreal project name in PascalCase.")
    parser.add_argument("version", help="Semantic version, for example 1.2.3.")
    parser.add_argument("--apply", action="store_true", help="Write changes; otherwise preview only.")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        result = update(args.kind, args.name, args.version, args.apply)
    except (ValueError, OSError, json.JSONDecodeError) as exc:
        emit({"ok": False, "error": str(exc)}, args.json)
        return 2
    emit(result, args.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
