# Repository Map

This document routes developers to the right project location and rule file. It
is not a second copy of the repository rules.

## Project locations

| Project type | Location |
| --- | --- |
| Direct Maya Script Editor tool | `maya/scripts/<ToolName>/` |
| Structured Maya tool or plug-in | `maya/tools/<ToolName>/` |
| Standalone Unreal plug-in | `unreal/Plugins/<PluginName>/` |
| Standalone Substance 3D Painter plug-in | `substance-painter/plugins/<PluginName>/` |
| Cross-host product | `composite/<ProjectName>/<host>/<ComponentName>/` |
| Product still in design (no installable component yet) | `docs/proposals/<project-name>/` |
| Reusable project template | `templates/<template-name>/` |
| Repository tooling and tests | `tools/` and `tests/` |

Complete standalone or composite project roots may be registered Git submodules.
The collection owns their index and pinned commit; each independent repository
owns its internal layout, rules and installation boundary. Other projects keep
the installation boundary defined by their nearest collection `AGENTS.md`.
See [submodule workflow](submodules.md) before changing repository ownership.

## Rule ownership

- [`AGENTS.md`](../AGENTS.md): repository-wide development and Git rules, and
  the list of supported hosts.
- [`maya/AGENTS.md`](../maya/AGENTS.md): Maya classification, layout, Python
  compatibility, and runtime safety.
- [`unreal/AGENTS.md`](../unreal/AGENTS.md): Unreal plug-in rules.
- [`substance-painter/AGENTS.md`](../substance-painter/AGENTS.md): Substance 3D
  Painter plug-in layout, compatibility, and runtime safety.
- [`composite/AGENTS.md`](../composite/AGENTS.md): cross-host product and
  component-boundary rules.

Read the repository rule file and every nearer platform rule before editing a
project. When a guide and an `AGENTS.md` disagree, follow `AGENTS.md` and fix the
guide.

## Adding a host

Supporting a new application is a repository change, delivered on its own:

1. Create `<host>/AGENTS.md` (lowercase kebab-case directory) covering project
   locations, runtime layout, compatibility, runtime safety, and verification,
   plus the standalone project directory it defines.
2. Register the host and its component checks in
   `tools/validate_repository.py`, with tests in `tests/`.
3. Add its release-tag prefix and archive name to
   [`naming-conventions.md`](naming-conventions.md), and update this map and the
   root README pair.
4. Add templates, packaging, and version commands when the first project for
   that host needs them, not before.

## Supporting documentation

- [`development-conventions.md`](development-conventions.md): change workflow
  and documentation ownership.
- [`naming-conventions.md`](naming-conventions.md): naming and release formats.
- [`release-process.md`](release-process.md): release checklist.
- [`agents/`](agents/): issue-tracker and domain-documentation integration for
  engineering skills.
- [`proposals/`](proposals/README.md): requirement baselines for products in
  design.
- [`project-history/`](project-history/): stable records from completed work.
