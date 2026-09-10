# Repository Instructions

This monorepo contains independent Autodesk Maya scripts/tools and Unreal Engine
plugins for 3D art production. The repository root is the only Git repository.

## Layout and rule scope

- `maya/scripts/<ToolName>/`: self-contained Maya shelf scripts.
- `maya/tools/<ToolName>/`: structured Maya tools and plug-ins.
- `unreal/Plugins/<PluginName>/`: standalone Unreal plugins.
- `composite/<ProjectName>/<host>/<ComponentName>/`: cross-host products with
  independently installable components.
- `templates/`, `tools/`, `tests/`: project templates and repository tooling.
- `docs/`: shared guides; `docs/project-history/<project-name>/`: durable design
  and acceptance records.

Read the applicable nested AGENTS.md before editing. For Maya or Unreal code,
including composite components and templates, also read `maya/AGENTS.md` or
`unreal/AGENTS.md`; sibling rule files are not inherited automatically.
Composite metadata ownership takes precedence over standalone layout examples.
Keep nested rules focused on local exceptions, contracts, and useful commands.

## Project contracts

- Each structured tool/plugin remains independently installable, testable,
  versioned, and packageable. Document and package any sibling dependency.
- Standalone projects own their README pair, changelog, version, dependencies,
  and relevant tests. Composite ownership is defined in `composite/AGENTS.md`.
- Preserve public APIs and supported versions unless the task authorizes a
  breaking migration. Avoid unrelated refactoring and third-party edits;
  task-required changes to either should have a clear reason.
- Do not check in secrets, machine-specific paths, caches, host/IDE build output,
  or release archives. Intentional source assets and generated source required
  by the project are distinct from disposable output.
- General directories use lowercase kebab-case; Maya/Unreal project names follow
  their platform rules. Python internals use snake_case and classes PascalCase.
  Use Semantic Versioning where practical.
- Do not create nested repositories, force-push, or discard unrelated history.
  Keep commits coherent and release tags project-prefixed, for example
  `maya-mesh-normal-tool-v0.2.0` or `ue-asset-audit-v1.1.0`.

## Documentation

- Update affected documentation when installation, public contracts, supported
  versions, or user-visible behavior changes. Keep English `README.md` and
  Chinese `README_CN.md` aligned.
- Project READMEs focus on introduction, compatibility, installation, and usage;
  include troubleshooting or limitations when useful. Put internal development
  detail in AGENTS.md or docs. The root README also owns the tool index and
  development setup.
- Keep reusable architecture and acceptance conclusions in existing domain or
  history records; use `docs/project-history/<project-name>/` for substantial
  completed work. Small fixes do not need new documents. Replace completed plans
  with durable conclusions when needed, rather than leaving active-looking plans.

## Validation

- Start with the owning project's relevant checks and scope generic linters to
  its files. Test changed composite components independently; include cross-host
  acceptance when shared behavior or contracts are affected.
- Run repository gates for tooling, templates, shared rules/workflows, root
  metadata, or layout changes:

  ```powershell
  python tools/validate_repository.py
  python -m unittest discover -s tests -v
  ```

- Broaden checks when a concrete dependency or regression risk justifies it.
  Unrelated sibling failures do not redefine the task's acceptance scope; do not
  use repository-wide tests as a substitute for missing project coverage.
- Check the final diff for unrelated/generated files and documentation accuracy.
  Report commands, results, skipped checks, and remaining risks.

## Task-specific workflows

- For requested issue/spec tracking, use GitHub Issues through `gh` and read
  `docs/agents/issue-tracker.md`. Routine implementation does not require a ticket.
- For issue classification, read `docs/agents/triage-labels.md`.
- For terminology or architecture work, read `docs/agents/domain.md` and relevant
  context/decision records. These workflows do not mandate extra artifacts for
  every task.
