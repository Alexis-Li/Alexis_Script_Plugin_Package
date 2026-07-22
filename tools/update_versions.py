"""Update one project's declared version; dry-run unless --apply is used."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

from _repo_tools import ROOT, emit, require_kebab_name, require_semver


def _maya_changes(name: str, version: str) -> list[tuple[Path, str]]:
    project = ROOT / "maya" / "tools" / require_kebab_name(name)
    module_files = list((project / "package").glob("*.mod")) if project.is_dir() else []
    if len(module_files) != 1:
        raise ValueError("Maya tool must contain exactly one package/*.mod")
    path = module_files[0]
    text = path.read_text(encoding="utf-8-sig")
    updated, count = re.subn(r"^(\+\s+\S+\s+)\S+", r"\g<1>" + version, text, count=1, flags=re.MULTILINE)
    if count != 1:
        raise ValueError("invalid Maya module file")
    changes = [(path, updated)]
    for init_file in (project / "package").rglob("__init__.py"):
        init_text = init_file.read_text(encoding="utf-8-sig")
        replaced, count = re.subn(
            r'(__version__\s*=\s*["\'])[^"\']+(["\'])',
            r"\g<1>" + version + r"\g<2>",
            init_text,
            count=1,
        )
        if count:
            changes.append((init_file, replaced))
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
    parser.add_argument("name", help="Maya kebab-case name or Unreal PascalCase name.")
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
