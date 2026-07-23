# Repository Instructions

## Repository Purpose

This repository is a monorepo containing independent Autodesk Maya scripts,
Maya tools, and Unreal Engine plugins for 3D art production.

The repository root is the only Git repository. Do not create nested Git
repositories under any project directory.

Every structured tool or plugin must remain independently installable,
testable, versioned, and packageable.

## Repository Layout

- `maya/scripts/<ToolName>/`: self-contained Maya shelf scripts
- `maya/tools/<ToolName>/`: Maya tools and plug-ins using standard runtime folders
- `unreal/Plugins/`: standalone Unreal Engine plugins
- `templates/`: project templates
- `tools/`: repository-level validation and packaging scripts
- `docs/`: shared development documentation

## General Rules

- Make the smallest coherent change required by the task.
- Do not refactor unrelated code.
- Do not rename public APIs without explicit approval.
- Do not add machine-specific absolute paths.
- Do not commit secrets, credentials, personal paths, caches, or generated files.
- Do not modify third-party code unless explicitly required.
- Preserve backward compatibility unless a breaking change is requested.
- Update documentation when installation, public APIs, supported versions,
  directory structures, or user-visible behavior change.
- Keep user-facing `README.md` files in English and pair them with a Chinese
  `README_CN.md` containing the same information.
- Keep the repository README comprehensive: repository purpose, tool index,
  documentation links, and development setup.
- Limit project READMEs to introduction, supported versions, installation, and
  usage. Put development rules and internal details in `AGENTS.md` or `docs/`.
- Keep platform-specific development and naming rules in the nearest platform
  `AGENTS.md`.

## Naming Conventions

- General repository directories: lowercase kebab-case
- Python packages and modules: lowercase snake_case
- Python classes: PascalCase
- Public versions: Semantic Versioning where practical

## Project Boundaries

Each structured project owns its README, changelog, version information,
runtime dependencies, and any project-specific tests or documentation it needs.
Repository-level scripts may provide shared validation and packaging behavior.

Sibling projects must not depend on each other unless the dependency is
explicitly documented and independently packageable.

## Generated Files

Never commit Python caches, Maya temporary files, Unreal generated folders,
Visual Studio generated files, or packaged release archives. Release archives
belong in GitHub Releases, not Git history.

## Validation

Before considering a task complete:

1. Run the narrowest relevant tests.
2. Run repository structural validation.
3. Confirm no generated files were added.
4. Confirm documentation remains accurate.
5. Report tests that could not be run and why.

## Git Rules

- Do not initialize nested repositories.
- Do not force-push or use destructive reset operations.
- Do not rewrite unrelated history.
- Keep commits scoped to one logical change.
- Use project-prefixed release tags such as `maya-mesh-normal-tool-v0.2.0`
  and `ue-asset-audit-v1.1.0`.
