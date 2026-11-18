# Agent Project Template

A host-independent documentation and rules skeleton for a new independent
project that is developed by AI agents for a user who is a 3D artist and does
not write code. It carries the agent workflow shared by the existing
independent projects (LoomlineRetopo, MtoULiveLink, StoUTextureSync): rules for
agents, a requirements-to-roadmap structure, an implementation/review split,
Issue and PR conventions, session handoff and changelog conventions.

The template is a starting point. `skeleton/` is copied to the root of the new
repository, and the new project then edits it to fit its own product.

## What the skeleton contains

Names are those in the new repository. Files saved with a `.tmpl` suffix
in `skeleton/` are renamed on creation, because a real `AGENTS.md`, `.gitignore`,
`.gitattributes` or `.editorconfig` inside this collection would act as that
directory's live rule file.

| File | Responsibility |
| --- | --- |
| `AGENTS.md` (from `AGENTS.md.tmpl`) | Rules for agents: reading map, working with the user, architecture rules reviews enforce, environment, default checks, data and privacy, session handoff, documentation and delivery, versions |
| `CONTEXT.md` | Glossary: Chinese term, code name, meaning |
| `docs/workflow.md` | Delivery lifecycle: implementation/review split, what to ask the user, branches, PRs, reviews, experiments, visual acceptance, architecture changes, samples, Issue comments, releases |
| `docs/requirements.md` | Requirements baseline with `G-`, `F-`, `NG-`, `Q-` IDs |
| `docs/roadmap.md` | Stages and tasks (`S<stage>-T<n>`) with scope, completion criteria and whether the user is needed |
| `docs/architecture.md` | Structure, module boundaries and architecture decisions (`AD-n`) |
| `docs/adr/README.md` | When and how to write an ADR, with a blank template |
| `README.md`, `README_CN.md` | The new project's user manual, English and Chinese |
| `CHANGELOG.md` | User-facing changes under `## Unreleased` |
| `.github/pull_request_template.md` | PR description: scope, SHAs, validation, remaining work |
| `.github/ISSUE_TEMPLATE/` | Bug report, task (sub-issue) and stage (umbrella issue) forms |
| `.gitignore`, `.gitattributes`, `.editorconfig` (from `*.tmpl`) | Common ignore, line-ending and editor rules |

The skeleton's `tools/` (project validation, status-comment helper), `tests/`
and `.github/workflows/` (CI) are described in the new project's own
documents.

## Creating a project

An agent generates the project with the collection's creation script. It
previews first; nothing is written until `--apply`:

```powershell
python tools/create_agent_project.py <ProjectName> --destination <path-to-new-repo>
python tools/create_agent_project.py <ProjectName> --destination <path-to-new-repo> --apply
```

`<ProjectName>` is PascalCase (for example `LoomlineRetopo`) and replaces every
`{{PROJECT_NAME}}`. After generation, in the new repository: run `git init`,
commit the skeleton, and create the first development branch `dev/<topic>`.

## First setup

Work through these before the first feature task:

1. Resolve every `待定制：` marker. Each marker says what to fill in and gives
   examples from the existing projects; delete the marker with it. A marker
   whose section does not apply is removed together with the section.
   `python tools/validate_project.py` fails while any marker remains.
2. Fill `CONTEXT.md`, `docs/architecture.md`, `docs/requirements.md` and
   `docs/roadmap.md` with the product's real content, with the user's direction.
3. Decide the check commands and write them into the Default checks table in
   `AGENTS.md`; make CI run the ones it can.
4. On GitHub, enable **Automatically delete head branches**. Whether the
   repository is public and which license it carries are the user's decisions;
   do not choose them.
5. Run `python tools/validate_project.py` and
   `python -m unittest discover -s tests` until both pass.

## What to keep and what to adapt

Keep as the common core unless the user decides otherwise: the
implementation/review split and the PR rules, session handoff, the comment and
status-comment rules, the branch model, the information-owner table, the data
and privacy rules, and the changelog conventions.

Adapt per project: the product description, the reading-map rows, the
architecture rules, environment and host versions, the check commands, the
sample definition, the visual-acceptance package (delete section 7 of
`docs/workflow.md` when the product has nothing for the user to look at), the
release and packaging commands.

Two optional variants:

- Requirements in an Issue instead of `docs/requirements.md`: one product Issue
  owns the requirements and sub-issues cite its IDs (StoUTextureSync's
  approach). Delete `docs/requirements.md` and point the reading map,
  `docs/roadmap.md` and the task template at that Issue.
- Durable history: a project that needs to keep selected decisions and evidence
  beyond ADRs adds `docs/history/` with its own retention rules, as MtoULiveLink
  and StoUTextureSync do. Add it only when a record has no better owner.

## Maintaining the template

A change to the template does not reach projects already created from it.
When a project proves an improvement to the shared core, write it back to
`skeleton/` and keep the project-specific parts as markers. Keep the skeleton
free of any real project's terms, paths and sample names.
