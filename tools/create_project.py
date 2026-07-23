"""Create a project from a repository template; dry-run unless --apply is used."""

from __future__ import annotations

import argparse
from pathlib import Path

from _repo_tools import ROOT, emit, pascal_case, require_kebab_name, require_pascal_name, snake_case


KINDS = {
    "maya-script": ("maya-shelf-script", "maya/scripts"),
    "maya-tool": ("maya-tool", "maya/tools"),
    "unreal-plugin": ("unreal-plugin", "unreal/Plugins"),
    "standalone-python-tool": ("standalone-python-tool", "standalone"),
}


def _renamed(relative_path: Path, pascal: str, snake: str, kind: str) -> Path:
    parts = []
    for part in relative_path.parts:
        if kind == "maya-script" and part == "script_name.py":
            part = pascal + ".py"
        elif part == "ToolName.py":
            part = pascal + ".py"
        elif part == "ToolName":
            part = pascal
        elif part == "tool_name":
            part = snake
        elif part == "PluginName.uplugin":
            part = pascal + ".uplugin"
        elif part == "PluginNameEditor":
            part = pascal + "Editor"
        elif part == "PluginNameEditor.Build.cs":
            part = pascal + "Editor.Build.cs"
        elif part == "PluginNameEditorModule.h":
            part = pascal + "EditorModule.h"
        elif part == "PluginNameEditorModule.cpp":
            part = pascal + "EditorModule.cpp"
        elif part == "project_name":
            part = snake
        parts.append(part)
    return Path(*parts)


def create(kind: str, name: str, apply: bool = False) -> dict:
    if kind in {"maya-script", "maya-tool"}:
        pascal = require_pascal_name(name)
        snake = ""
    else:
        name = require_kebab_name(name)
        pascal = pascal_case(name)
        snake = snake_case(name)
    template_name, destination_parent = KINDS[kind]
    source = ROOT / "templates" / template_name
    destination_name = pascal if kind in {"maya-script", "maya-tool", "unreal-plugin"} else name
    destination = ROOT / destination_parent / destination_name
    if destination.exists():
        raise FileExistsError("target already exists: {0}/{1}".format(destination_parent, destination_name))

    planned = []
    for source_path in sorted(
        path
        for path in source.rglob("*")
        if path.is_file()
        and "__pycache__" not in path.parts
        and path.suffix not in {".pyc", ".pyo"}
    ):
        relative_path = _renamed(source_path.relative_to(source), pascal, snake, kind)
        planned.append((source_path, destination / relative_path))
    if kind in {"maya-tool", "unreal-plugin"}:
        planned.append((ROOT / "LICENSE", destination / "LICENSE"))

    if apply:
        for source_path, destination_path in planned:
            destination_path.parent.mkdir(parents=True, exist_ok=True)
            text = source_path.read_text(encoding="utf-8")
            text = text.replace("{{TOOL_NAME}}", pascal)
            text = text.replace("{{PLUGIN_NAME}}", pascal)
            text = text.replace("{{PYTHON_PACKAGE}}", snake)
            text = text.replace("project-name", name)
            text = text.replace("project_name", snake)
            text = text.replace("script_name.py", pascal + ".py")
            text = text.replace("Script Name", pascal)
            text = text.replace("脚本名称", pascal)
            destination_path.write_text(text, encoding="utf-8", newline="\n")
    return {
        "ok": True,
        "mode": "apply" if apply else "dry-run",
        "target": "{0}/{1}".format(destination_parent, destination_name),
        "files": [path.relative_to(ROOT).as_posix() for _, path in planned],
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=sorted(KINDS))
    parser.add_argument("name", help="PascalCase for Maya projects; lowercase kebab-case otherwise.")
    parser.add_argument("--apply", action="store_true", help="Create files; otherwise preview only.")
    parser.add_argument("--json", action="store_true", help="Emit machine-readable output.")
    args = parser.parse_args(argv)
    try:
        result = create(args.kind, args.name, args.apply)
    except (ValueError, FileExistsError, OSError) as exc:
        result = {"ok": False, "error": str(exc)}
        emit(result, args.json)
        return 2
    emit(result, args.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
