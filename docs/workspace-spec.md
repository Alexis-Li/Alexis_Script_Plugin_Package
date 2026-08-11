# Workspace Specification

This repository is the only Git root. Its independent projects live under:

```text
maya/scripts/<ToolName>/
maya/tools/<ToolName>/
unreal/Plugins/<PluginName>/
composite/<ProjectName>/<host>/<ComponentName>/
```

Maya names use PascalCase. A direct Script Editor tool is one Python file under
`maya/scripts`; a structured Maya project exposes only the standard runtime
folders it needs (`scripts`, `plug-ins`, `icons`, or `presets`) at its project
root. Small and medium Maya tools do not require `.mod` files and install by
copying runtime files to Maya's matching directories. Unreal plug-ins keep
their `.uplugin` at the plug-in root. Composite projects use a PascalCase
product directory, lowercase host directories, and a complete PascalCase
component root per host. Users install the host component, not the composite
project directory.

The authoritative rules are:

- [`AGENTS.md`](../AGENTS.md): repository-wide development and Git rules.
- [`maya/AGENTS.md`](../maya/AGENTS.md): Maya classification, layout, Python
  compatibility, and runtime safety.
- [`unreal/AGENTS.md`](../unreal/AGENTS.md): Unreal plug-in rules.
- [`composite/AGENTS.md`](../composite/AGENTS.md): cross-host product and
  component-boundary rules.
- [`development-conventions.md`](development-conventions.md): shared workflow.
- [`naming-conventions.md`](naming-conventions.md): naming and release formats.

Project READMEs are user-facing and contain only introduction, supported
versions, installation, and usage. Root READMEs contain the broader repository
overview and developer onboarding.
