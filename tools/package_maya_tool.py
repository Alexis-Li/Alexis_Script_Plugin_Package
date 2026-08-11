"""Build a deterministic Maya tool archive; dry-run unless --apply is used."""

from __future__ import annotations

import argparse
import zipfile
from pathlib import Path

from _repo_tools import ROOT, emit, maya_tool_path, maya_version


def _write_archive(archive: Path, project: Path, files: list[Path]) -> None:
    archive.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        for source in files:
            relative = source.relative_to(project).as_posix()
            info = zipfile.ZipInfo(relative, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            output.writestr(info, source.read_bytes())


def package(name: str, output_dir: Path, apply: bool = False) -> dict:
    project = maya_tool_path(name)
    version = maya_version(project)
    included_roots = [
        project / "scripts",
        project / "plug-ins",
        project / "icons",
        project / "presets",
        project / "README.md",
        project / "README_CN.md",
        project / "CHANGELOG.md",
        project / "LICENSE",
    ]
    files = []
    for item in included_roots:
        if item.is_file():
            files.append(item)
        elif item.is_dir():
            files.extend(
                path
                for path in item.rglob("*")
                if path.is_file() and "__pycache__" not in path.parts
            )
    archive = output_dir / "{0}-{1}.zip".format(name, version)
    if apply:
        _write_archive(archive, project, sorted(files))
    return {
        "ok": True,
        "mode": "apply" if apply else "dry-run",
        "archive": archive.relative_to(ROOT).as_posix()
        if archive.is_relative_to(ROOT)
        else archive.name,
        "file_count": len(files),
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name", help="Maya tool directory name in PascalCase.")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "releases")
    parser.add_argument(
        "--apply", action="store_true", help="Write the archive; otherwise preview only."
    )
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    output_dir = args.output_dir if args.output_dir.is_absolute() else ROOT / args.output_dir
    try:
        result = package(args.name, output_dir.resolve(), args.apply)
    except (ValueError, OSError) as exc:
        emit({"ok": False, "error": str(exc)}, args.json)
        return 2
    emit(result, args.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
