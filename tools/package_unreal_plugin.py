"""Build a deterministic Unreal plugin archive; dry-run unless --apply is used."""

from __future__ import annotations

import argparse
import json
import zipfile
from pathlib import Path

from _repo_tools import ROOT, emit, unreal_plugin_path


EXCLUDED_DIRS = {"Binaries", "DerivedDataCache", "Intermediate", "Saved", ".vs", ".git"}


def package(plugin_name: str, engine: str, output_dir: Path, apply: bool = False) -> dict:
    project = unreal_plugin_path(plugin_name)
    descriptor = project / (plugin_name + ".uplugin")
    if not descriptor.is_file():
        raise ValueError(
            "missing plugin descriptor: {0}".format(
                descriptor.relative_to(ROOT).as_posix()
            )
        )
    metadata = json.loads(descriptor.read_text(encoding="utf-8-sig"))
    version = str(metadata.get("VersionName") or "0.0.0")
    files = [
        path for path in project.rglob("*")
        if path.is_file() and not any(part in EXCLUDED_DIRS for part in path.parts)
    ]
    archive = output_dir / "{0}-{1}-UE{2}.zip".format(plugin_name, version, engine)
    if apply:
        archive.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as output:
            for source in sorted(files):
                relative = Path(plugin_name) / source.relative_to(project)
                info = zipfile.ZipInfo(relative.as_posix(), date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                output.writestr(info, source.read_bytes())
    return {
        "ok": True,
        "mode": "apply" if apply else "dry-run",
        "archive": archive.relative_to(ROOT).as_posix() if archive.is_relative_to(ROOT) else archive.name,
        "file_count": len(files),
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plugin_name", help="Plugin directory and descriptor name in PascalCase.")
    parser.add_argument("--engine", default="5.7", help="Engine version used in the archive name.")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "releases")
    parser.add_argument("--apply", action="store_true", help="Write the archive; otherwise preview only.")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    output_dir = args.output_dir if args.output_dir.is_absolute() else ROOT / args.output_dir
    try:
        result = package(args.plugin_name, args.engine, output_dir.resolve(), args.apply)
    except (ValueError, OSError, json.JSONDecodeError) as exc:
        emit({"ok": False, "error": str(exc)}, args.json)
        return 2
    emit(result, args.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
