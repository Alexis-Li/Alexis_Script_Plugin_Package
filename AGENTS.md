# Repository Instructions

This monorepo contains independent scripts, tools, and plug-ins for 3D game-art
production across several host applications (DCC tools and game engines). The
repository root is the only Git repository.

## Hosts

A host is an application that loads the repository's code. Each supported host
has a lowercase kebab-case top-level directory whose `AGENTS.md` (the host rule
file) defines its standalone project locations, runtime layout, compatibility,
safety, and verification rules.

| Host | Directory | Standalone projects |
| --- | --- | --- |
| Autodesk Maya | `maya/` | `maya/scripts/<ToolName>/`, `maya/tools/<ToolName>/` |
| Unreal Engine | `unreal/` | `unreal/Plugins/<PluginName>/` |
| Adobe Substance 3D Painter | `substance-painter/` | `substance-painter/plugins/<PluginName>/` |

To support another host, add its directory and host rule file, register it in
`tools/validate_repository.py`, and update `docs/workspace-spec.md`,
`docs/naming-conventions.md`, and the root README pair in the same change. Add
templates, packaging, and version commands when its first project needs them.

## Layout and rule scope

- `<host>/...`: standalone projects in the locations listed above.
- `composite/<ProjectName>/<host>/<ComponentName>/`: cross-host products with
  independently installable components.
- `templates/`, `tools/`, `tests/`: project templates and repository tooling.
- `docs/`: shared guides; `docs/proposals/<project-name>/`: requirement
  baselines for products still in design; `docs/project-history/<project-name>/`:
  durable design and acceptance records.

Read the applicable nested AGENTS.md before editing. For host code, including
composite components and templates, also read that host's rule file; sibling
rule files are not inherited automatically. Composite metadata ownership takes
precedence over standalone layout examples. Keep nested rules focused on local
exceptions, contracts, and useful commands.

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
- General directories use lowercase kebab-case; host project names follow their
  host rule file. Python internals use snake_case and classes PascalCase.
  Use Semantic Versioning where practical.
- Do not create nested repositories, force-push, or discard unrelated history.
  Keep commits coherent and release tags prefixed as defined in
  `docs/naming-conventions.md`, for example `maya-mesh-normal-tool-v0.2.0` or
  `ue-asset-audit-v1.1.0`.

## Scratch files

- For agent-created temporary files, first use the `.tmp` directory beside this
  repository's root. Resolve it from the actual Git root (its parent directory
  plus `.tmp`), rather than hard-coding a drive letter or the current shell
  directory. The directory for this checkout is `../.tmp` relative to the root.
- If that directory is absent or cannot be written under the current sandbox,
  use the ignored `.tmp/` directory inside the repository. Keep scratch files
  out of tracked source directories and remove them when they are no longer
  needed. Do not choose the operating system's default temporary directory for
  agent-created scratch files when either repository location is available.
- A tool or host may create its own internal temporary files through `TEMP`,
  `TMP`, or equivalent settings. This rule does not redirect those processes;
  set their temporary-directory environment explicitly when a task requires it.
- Clean issue-owned scratch, including closeout helpers and drafts, under the
  completion checks in `docs/agents/issue-tracker.md`.

## Documentation

- Update affected documentation when installation, public contracts, supported
  versions, or user-visible behavior changes. Keep English `README.md` and
  Chinese `README_CN.md` aligned.
- Project READMEs are current user manuals: introduction, compatibility,
  installation, common workflows, and actionable troubleshooting or limitations.
  Include a detail only when it helps users choose, install, operate, or recover
  the product. The root README also owns the tool index and development setup.
- Update the relevant README section in place when behavior changes; consolidate
  overlapping guidance and remove obsolete text. Do not append a feature or fix
  narrative for each development task. Write for a first-time user who has not
  seen the development conversation. When a requirement is removed, delete the
  obsolete instruction rather than adding reassurance about its removal (for
  example, "no need to install X") unless existing users need a migration step.
  Internal-only changes need no README edit.
- Put version-by-version changes in CHANGELOG.md, implementation rationale and
  acceptance evidence in existing docs/domain/history records, and reusable agent
  instructions in AGENTS.md. Keep issue timelines, test counts, benchmark corpora,
  internal algorithms, and debugging narratives out of READMEs. Retain concise
  user-facing limits and recovery steps; link to deeper records when useful
  instead of copying them. Small fixes do not require new documents.
- Before delivering a README change, review the affected section as a complete
  user workflow, not just the added lines: keep required steps easy to find,
  remove duplication and development residue, and align both language versions.
  For each added or revised statement, identify the user decision, action, or
  recovery it supports. If removing it leaves those unchanged and it only
  explains a past fix or an implementation choice, omit it. Apply this check to
  user-facing UI text and usage guides as well.
  Split genuinely needed advanced usage into a linked user guide when it obscures
  the common workflow; do not use an arbitrary line limit or move the same bloat
  into another document.
- A product in design without any installable component keeps its requirement
  baseline in `docs/proposals/<project-name>/` (see `docs/proposals/README.md`).
  When development creates the owning project, move the surviving content into
  that project and delete the proposal directory.
- Use `docs/project-history/README.md` to decide what needs a durable record.
  Routine delivery and review stay in Issues/PRs; extend an existing topic record
  for reusable decisions or essential host evidence. Replace completed plans
  with durable conclusions when needed, rather than leaving active-looking plans.
- Write acceptance records and issue comments as current conclusions, supporting
  evidence, and material limits. Update an incorrect conclusion in place; keep
  superseded debugging and correction narratives out of the final account.

## Validation

- Start with the owning project's relevant checks and scope generic linters to
  its files. Test changed composite components independently; include cross-host
  acceptance when shared behavior or contracts are affected.
- During iterative development, rerun failed tests or the smallest affected test
  selection after a change, resuming from the failure when the runner supports it.
  Do not restart the full suite after every edit. Run required broader checks
  when the implementation stabilizes, and repeat them only if later changes could
  invalidate their results.
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

- For Issue/PR work, read `docs/agents/issue-tracker.md` and use `gh` from this
  clone. The issue owns scope and acceptance; the delivery owns its exact diff,
  review and validation evidence. Generate and validate formal issue reports
  with `tools/issue_report.py` before publishing.
- Follow that protocol for current handoffs, source preservation, publication
  and requested closeout. Preserve acceptance scope, human decisions and
  unresolved findings through consolidation. When delegating, supply the
  protocol and let one coordinating agent publish the consolidated result.
- For issue classification, read `docs/agents/triage-labels.md`.
- For terminology or architecture work, read `docs/agents/domain.md` and relevant
  context/decision records. These workflows do not mandate extra artifacts for
  every task.
