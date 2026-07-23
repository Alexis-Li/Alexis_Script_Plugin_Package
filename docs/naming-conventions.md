# Naming Conventions

- Maya project directories use PascalCase without separators:
  `FlattenMeshToUV`, `NitroPoly`, `ZiSpread`.
- Maya user-run tool files also use PascalCase: `FlattenMeshToUV.py`.
- Internal Python packages and modules use lowercase snake_case only when a
  multi-file implementation is needed.
- Python classes use PascalCase; functions and variables use snake_case.
- General repository directories use lowercase kebab-case.
- Unreal plugins and modules use PascalCase and Unreal C++ types follow Epic's
  naming conventions.
- Public versions use Semantic Versioning where practical.

Release tags use `maya-<project-name>-v<version>` or
`ue-<plugin-name>-v<version>`. Archives use readable project names such as
`FlattenMeshToUV-2.0.1.zip` or `AssetAudit-0.4.0-UE5.7.zip`.
