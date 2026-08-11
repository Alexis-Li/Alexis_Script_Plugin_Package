# Naming Conventions

This is the consolidated human reference for repository names. The applicable
`AGENTS.md` remains normative for agent work and may impose additional
platform-specific constraints.

- Maya project directories use PascalCase without separators:
  `FlattenMeshToUV`, `NitroPoly`, `ZiSpread`.
- Maya user-run tool files also use PascalCase: `FlattenMeshToUV.py`.
- Internal Python packages and modules use lowercase snake_case only when a
  multi-file implementation is needed.
- Python classes use PascalCase; functions and variables use snake_case.
- General repository directories use lowercase kebab-case.
- Composite product directories use PascalCase; their host directories use
  lowercase kebab-case, and installable component roots use the public
  PascalCase component name.
- Unreal plugins and modules use PascalCase and Unreal C++ types follow Epic's
  naming conventions.
- Public versions use Semantic Versioning where practical.

Release tags use `maya-<project-name>-v<version>`,
`ue-<plugin-name>-v<version>`, or `composite-<project-name>-v<version>`.
Composite components keep their host-specific archive formats. Archives use
readable project names such as `FlattenMeshToUV-2.0.1.zip` or
`AssetAudit-0.4.0-UE5.7.zip`.
