# Development Conventions

This is the human-facing change workflow. Normative boundaries and platform
requirements live in the repository and nearest platform `AGENTS.md`; use the
[repository map](workspace-spec.md) to find them.

## Change workflow

1. Read the applicable `AGENTS.md` chain and classify the project before
   choosing a directory.
2. Inspect the owning project, dependency declarations, tests, and packaging
   commands before editing.
3. Choose a delivery boundary under the [Issue/PR workflow](agents/issue-tracker.md):
   task branch and PR for substantial or overlapping work; an explicit commit
   range for a small isolated change on the current branch.
4. Make the smallest coherent change and preserve public entry points unless a
   breaking change is explicitly required.
5. Update the owning user documentation and changelog when behavior changes.
6. Run the owning project's acceptance checks, broadening for concrete dependency
   or regression risks as described in the root AGENTS.md.
7. Inspect `git status` for generated or unrelated files before review. Review
   the recorded diff, verify changes since review before integration, and close
   the task against its agreed acceptance and integration target.

For changes to repository tooling, templates, shared rules or workflows, root
metadata, or project layout, also run the repository gates:

```powershell
python tools/validate_repository.py
python -m unittest discover -s tests -v
```

Host-dependent checks (Maya, Unreal, Substance 3D Painter, ...) remain
project-specific and run in the supported host runtime. Scope generic linters and test discovery to the owning
project; sibling projects are not part of its acceptance result.

## Repository tools

For an independent project submodule, run Git, gh and project commands inside
that repository. Collection checks validate its registration and pin; product
checks and delivery run in its own CI. See [submodules](submodules.md).

Prefer the standard library for repository Python tools; justify and declare
necessary dependencies under the applicable dependency rules. Existing write
commands default to dry-run and require `--apply`; preserve that CLI contract.
Host runtime code must use the Python, Qt, and engine versions shipped by the
target host.

## Documentation ownership

| Information | Owner |
| --- | --- |
| Requirements, acceptance scope and task decisions | Issue |
| Independent product requirements, code and delivery | That submodule's repository and tracker |
| Requirement baseline of a product still in design | `docs/proposals/<project>/`, linked from its umbrella issue |
| Delivery, code review and routine checks | PR or direct-branch issue reports |
| User installation and usage | Project `README.md` and `README_CN.md` |
| Released and unreleased behavior changes | Owning `CHANGELOG.md` |
| Normative development constraints | Applicable `AGENTS.md` |
| Selected completed decisions and essential host evidence | Existing topic in `docs/project-history/<project>/` |
| Active domain vocabulary and decisions | Context and ADR locations described in `docs/agents/domain.md` |

Link to the owner instead of copying its full content into another document.
Use the [history retention rules](project-history/README.md) to select durable
knowledge before adding a record.
