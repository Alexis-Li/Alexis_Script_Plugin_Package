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
- `composite/<ProjectName>/<host>/<ComponentName>/`: independently installable
  host components that form one cross-application product
- `templates/`: project templates
- `tools/`: repository-level validation and packaging scripts
- `docs/`: shared development documentation
- `docs/project-history/<project-name>/`: stable architecture, migration, and
  acceptance history for completed projects

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
- After a project is complete, integrate durable design and acceptance records
  under `docs/project-history/<project-name>/` with stable purpose-based names.
  Do not retain completed implementation plans as active documentation.

## Naming Conventions

- General repository directories: lowercase kebab-case
- Python packages and modules: lowercase snake_case
- Python classes: PascalCase
- Public versions: Semantic Versioning where practical

## Project Boundaries

Each independent project under `maya/` or `unreal/` owns its README, changelog,
version information, runtime dependencies, and any project-specific tests or
documentation it needs. These existing single-host ownership rules do not
change for legacy or future standalone Maya and Unreal projects.

For a project under `composite/`, the composite project root is the sole owner
of `README.md`, `README_CN.md`, `CHANGELOG.md`, and `LICENSE`. Host component
roots contain only their implementation, host-required descriptors or assets,
and tests that must stay beside that implementation. Do not duplicate project
metadata inside a composite host component.

Every standalone or composite project defines its own acceptance scope through
colocated tests, build and package commands, applicable platform rules, and any
stable acceptance record under `docs/project-history/<project-name>/`.

Repository-level scripts may provide shared validation and packaging behavior.

Sibling projects must not depend on each other unless the dependency is
explicitly documented and independently packageable.

Composite products group cooperating host components without merging their
installation boundaries. Each host component must remain independently
installable, testable, versioned, and packageable; follow `composite/AGENTS.md`
and the applicable host development conventions.

## Generated Files

Never commit Python caches, Maya temporary files, Unreal generated folders,
Visual Studio generated files, or packaged release archives. Release archives
belong in GitHub Releases, not Git history.

## Validation

Before considering a task complete:

1. Run only the owning project's relevant tests, lint, build, host, and package
   checks. Scope generic tools such as Ruff to that project's files.
2. For a composite project, test each changed host component independently;
   run cross-host acceptance only for cross-host behavior changes.
3. Run repository tests and structural validation only for changes to
   repository tooling, templates, shared rules or workflows, root metadata, or
   project layout, or when explicitly requested.
4. Confirm no generated files were added and documentation remains accurate.
5. Report commands run, pass/fail status, skipped checks, and remaining risks.

Sibling-project failures are outside the acceptance scope of a project change.
Do not run repository-wide lint or tests as a fallback for missing project
checks.

## Git Rules

- Do not initialize nested repositories.
- Do not force-push or use destructive reset operations.
- Do not rewrite unrelated history.
- Keep commits scoped to one logical change.
- Use project-prefixed release tags such as `maya-mesh-normal-tool-v0.2.0`
  and `ue-asset-audit-v1.1.0`.

## Agent skills

### Issue tracker

Track issues and specs in GitHub Issues using the `gh` CLI. See
`docs/agents/issue-tracker.md`.

### Triage labels

Use the canonical triage label mapping when classifying or updating issues.
See `docs/agents/triage-labels.md`.

### Domain docs

Use the multi-context domain layout when exploring terminology or architectural
decisions. See `docs/agents/domain.md`.
