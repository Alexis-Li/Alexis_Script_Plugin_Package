# Naming Conventions

This is the consolidated human reference for repository names. The applicable
`AGENTS.md` remains normative for agent work and may impose additional
platform-specific constraints.

- Host directories use lowercase kebab-case: `maya`, `unreal`,
  `substance-painter`.
- Maya project directories use PascalCase without separators:
  `FlattenMeshToUV`, `NitroPoly`, `ZiSpread`.
- Maya user-run tool files also use PascalCase: `FlattenMeshToUV.py`.
- Substance 3D Painter project directories use PascalCase; the plug-in modules
  and packages inside them use lowercase snake_case.
- Internal Python packages and modules use lowercase snake_case only when a
  multi-file implementation is needed.
- Python classes use PascalCase; functions and variables use snake_case.
- General repository directories use lowercase kebab-case, including proposal
  directories under `docs/proposals/`.
- Composite product directories use PascalCase; their host directories use the
  registered host names, and installable component roots use the public
  PascalCase component name.
- Unreal plugins and modules use PascalCase and Unreal C++ types follow Epic's
  naming conventions.
- Public versions use Semantic Versioning where practical.
- Registered submodule mount paths follow the project naming above. Independent
  repositories define their own internal names and release tags; collection tag
  prefixes below apply to releases owned by this repository.

## Release tags and archives

| Release | Tag | Archive |
| --- | --- | --- |
| Maya project | `maya-<project-name>-v<version>` | `<ToolName>-<version>.zip` |
| Unreal plug-in | `ue-<plugin-name>-v<version>` | `<PluginName>-<version>-UE<engine>.zip` |
| Substance 3D Painter plug-in | `sp-<plugin-name>-v<version>` | `<PluginName>-<version>-SP.zip` |
| Composite product | `composite-<project-name>-v<version>` | Each component keeps its host archive format |

Examples: `FlattenMeshToUV-2.0.1.zip`, `AssetAudit-0.4.0-UE5.7.zip`.
