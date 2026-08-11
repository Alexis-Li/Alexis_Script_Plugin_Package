# Repository Map

This document routes developers to the right project location and rule file. It
is not a second copy of the repository rules.

## Project locations

| Project type | Location |
| --- | --- |
| Direct Maya Script Editor tool | `maya/scripts/<ToolName>/` |
| Structured Maya tool or plug-in | `maya/tools/<ToolName>/` |
| Standalone Unreal plug-in | `unreal/Plugins/<PluginName>/` |
| Cross-host product | `composite/<ProjectName>/<host>/<ComponentName>/` |
| Reusable project template | `templates/<template-name>/` |
| Repository tooling and tests | `tools/` and `tests/` |

The repository root is the only Git root. Each project or host component keeps
the installation boundary defined by its nearest `AGENTS.md`.

## Rule ownership

- [`AGENTS.md`](../AGENTS.md): repository-wide development and Git rules.
- [`maya/AGENTS.md`](../maya/AGENTS.md): Maya classification, layout, Python
  compatibility, and runtime safety.
- [`unreal/AGENTS.md`](../unreal/AGENTS.md): Unreal plug-in rules.
- [`composite/AGENTS.md`](../composite/AGENTS.md): cross-host product and
  component-boundary rules.

Read the repository rule file and every nearer platform rule before editing a
project. When a guide and an `AGENTS.md` disagree, follow `AGENTS.md` and fix the
guide.

## Supporting documentation

- [`development-conventions.md`](development-conventions.md): change workflow
  and documentation ownership.
- [`naming-conventions.md`](naming-conventions.md): naming and release formats.
- [`release-process.md`](release-process.md): release checklist.
- [`agents/`](agents/): issue-tracker and domain-documentation integration for
  engineering skills.
- [`project-history/`](project-history/): stable records from completed work.
