# Alexis Script & Plugin Package

[中文说明](README_CN.md)

## What This Repository Is

This repository is a collection of independently usable Autodesk Maya scripts
and tools for 3D art production. It also provides a workspace for standalone
Unreal Engine plug-ins.

Maya projects favor one directly runnable Python file. A project only adds
`scripts/`, `plug-ins/`, `icons/`, or other files when the feature requires
them. Small and medium tools install by copying those files to Maya's matching
directories and do not require a `.mod` file. Maya runtime code should support
both Python 2 and Python 3 where practical; Python 3 takes priority when one
implementation cannot support both.

## Tools

| Tool | Purpose | Location |
| --- | --- | --- |
| Flatten Mesh To UV | Converts a mesh's UV layout into flat polygon geometry. | [`maya/tools/FlattenMeshToUV`](maya/tools/FlattenMeshToUV/) |
| NitroPoly | Provides polygon selection, topology, pivot, connection, and edge-loop workflows. | [`maya/tools/NitroPoly`](maya/tools/NitroPoly/) |
| ZiSpread | Evenly distributes selected edge loops through an interactive drag operation. | [`maya/scripts/ZiSpread`](maya/scripts/ZiSpread/) |

## Tool Documentation

- [Flatten Mesh To UV](maya/tools/FlattenMeshToUV/README.md)
- [NitroPoly](maya/tools/NitroPoly/README.md)
- [ZiSpread](maya/scripts/ZiSpread/README.md)
- [Maya development rules](docs/maya-development.md)
- [Naming conventions](docs/naming-conventions.md)
- [Release process](docs/release-process.md)

## Start Developing

1. Install Python 3.10 or newer for the repository maintenance commands.
2. Read [`AGENTS.md`](AGENTS.md) and the platform-specific `AGENTS.md` before editing.
3. Preview a project from a template; add `--apply` only when the preview is correct:

   ```powershell
   python tools/create_project.py maya-script SampleTool --json
   ```

4. Keep Maya user-run project and entry-file names in PascalCase, such as
   `FlattenMeshToUV` and `FlattenMeshToUV.py`.
5. Run the repository checks before handing off a change:

   ```powershell
   python -m unittest discover -s tests
   python tools/validate_repository.py
   ```

See [development conventions](docs/development-conventions.md) for the shared
workflow and [Unreal development](docs/unreal-development.md) for Unreal rules.
