# Alexis Script & Plugin Package

[中文说明](README_CN.md)

## What This Repository Is

This repository is a collection of independently usable scripts, tools, and
plug-ins for 3D game-art production. It covers Autodesk Maya, Unreal Engine, and
Adobe Substance 3D Painter, as well as composite products that cooperate across
several of these applications.

Choose a tool below and follow its manual for supported application versions,
installation and usage. Each tool or host component can be installed separately.
The collection contains directly owned projects and independent submodules;
independent projects own their source, Issues and releases in their own repository.

## Tools

| Tool | Purpose | Location |
| --- | --- | --- |
| Flatten Mesh To UV | Converts a mesh's UV layout into flat polygon geometry. | [`maya/tools/FlattenMeshToUV`](maya/tools/FlattenMeshToUV/) |
| MtoU Live Link | Previews Maya character animation in Unreal Editor in real time or from a cache, with camera linkage, a second object and scene reference, through independently installable Maya and Unreal components. | [`composite/MtoULiveLink`](composite/MtoULiveLink/) |
| NitroPoly | Provides polygon selection, topology, pivot, connection, and edge-loop workflows. | [`maya/tools/NitroPoly`](maya/tools/NitroPoly/) |
| StoUTextureSync | Painter-to-Unreal texture synchronization, in prototyping. | [Independent repository](https://github.com/Alexis-Li/StoUTextureSync) · submodule `substance-painter/plugins/StoUTextureSync` |
| ZiSpread | Evenly distributes selected edge loops through an interactive drag operation. | [`maya/scripts/ZiSpread`](maya/scripts/ZiSpread/) |

## Tool Documentation

- [Flatten Mesh To UV](maya/tools/FlattenMeshToUV/README.md)
- [MtoU Live Link](composite/MtoULiveLink/README.md)
- [NitroPoly](maya/tools/NitroPoly/README.md)
- [ZiSpread](maya/scripts/ZiSpread/README.md)
- [Repository map and supported hosts](docs/workspace-spec.md)
- [Maya project guide](docs/maya-development.md)
- [Naming conventions](docs/naming-conventions.md)
- [Release process](docs/release-process.md)
- [Products in design](docs/proposals/README.md)
- [Project development history](docs/project-history/README.md)

## Get Independent Projects

Clone with `git clone --recurse-submodules <repository-URL>`. In an existing
checkout, preserve local child changes before running
`git submodule update --init --recursive`. See the [submodule workflow](docs/submodules.md)
for development, pin updates and migration.

## Start Developing

1. Install Python 3.10 or newer for the repository maintenance commands.
2. Read [`AGENTS.md`](AGENTS.md) and the host rule file (`maya/`, `unreal/`, or
   `substance-painter/AGENTS.md`) before editing.
3. Preview a project from a template; add `--apply` only when the preview is correct:

   ```powershell
   python tools/create_project.py maya-script SampleTool --json
   ```

4. Keep Maya user-run project and entry-file names in PascalCase, such as
   `FlattenMeshToUV` and `FlattenMeshToUV.py`.
5. For repository tooling or structural changes, run the repository checks:

   ```powershell
   python -m unittest discover -s tests
   python tools/validate_repository.py
   ```

   For a project change, run only that project's tests, lint, build, and
   packaging checks.

See [development conventions](docs/development-conventions.md) for the shared
workflow and the [Unreal plugin guide](docs/unreal-development.md) for project
navigation. Use the [Issue/PR workflow](docs/agents/issue-tracker.md) to choose
a delivery path and track review and integration. Consult the
[history retention rules](docs/project-history/README.md) before adding records.
