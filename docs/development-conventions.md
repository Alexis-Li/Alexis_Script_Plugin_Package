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
   small documentation and rule edits go on the current working branch when
   they change no code, tests, tools, CI, packaging, version or product decision.
   Other work uses a `feature/<topic>` or `fix/<topic>` branch and a PR to the
   intended integration branch. Use a PR when size or impact is uncertain.
4. Make the smallest coherent change and preserve public entry points unless a
   breaking change is explicitly required.
5. Update the owning user documentation and changelog when behavior changes.
6. Run the owning project's acceptance checks, broadening for concrete dependency
   or regression risks as described in the root AGENTS.md.
7. Inspect `git status` for generated or unrelated files before review. Review
   the recorded diff and publish the review against the latest PR head SHA before
   every merge. Record the integration SHA on the issue. Close against agreed
   acceptance and consolidate comments under the linked tracker workflow.

For changes to repository tooling, templates, shared rules or workflows, root
metadata, or project layout, also run the repository gates:

```powershell
python tools/validate_repository.py
python -m unittest discover -s tests -v
```

Host-dependent checks (Maya, Unreal, Substance 3D Painter, ...) remain
project-specific and run in the supported host runtime. Scope generic linters and test discovery to the owning
project; sibling projects are not part of its acceptance result.

## Design before implementation

| Change | State before implementing | Record and decision |
| --- | --- | --- |
| Local fix or small feature; interfaces and state ownership unchanged | Real call chain, change location, directly affected behavior and how it will be verified | A short note in the task; no separate document |
| Cross-module feature, or a change to a public interface or to state/resource ownership | Reused capabilities, module responsibilities, data/state flow, failure recovery, compatibility and behavior that must be preserved | The existing issue or design record; one review of the key design before implementing |
| The approach depends on an unproven host capability | The open question that decides the approach | Settle it with a minimal prototype or experiment before building further |

Make ordinary implementation choices without asking. Changing agreed product
semantics or an important trade-off is the user's decision. Cite an existing
design when it suffices; write an ADR only for a new long-lived decision. Once
responsibilities, contracts and acceptance are clear and no open question
affects the approach, start implementing.

## Review findings and fixes

- Confirm a finding through the code path, a contract or a reproduction before
  changing the implementation.
- Before editing, check the actual callers and the owner of the affected state
  or resource, and fix the root cause there. Recheck the original problem and
  the existing behavior the fix touches; reuse evidence that remains valid.
- Do not make a new failure pass by swallowing errors, deleting assertions,
  loosening thresholds or rewriting acceptance. Correcting a wrong check needs
  independent evidence. A fix that changes an interface or state ownership goes
  back to the design step.
- When the same kind of problem keeps returning in a third review round, stop
  stacking patches: diagnose the root cause, narrow the change or redesign it.
  Confirmed blockers stay blocking.
- Style preferences and improvements unrelated to the change do not drive
  rework; record them as follow-up candidates.

## Verification scope

- At the start, state briefly what behavior must be shown and the smallest
  check that shows it. Keep the project's required checks; add more only for
  uncovered acceptance, an actual failure or evidenced regression risk.
- Do not add tests or defensive branches (retries, compatibility layers,
  settings, fallbacks) for cases with no real trigger. Keep targeted checks for
  reachable paths that could delete assets or damage a project.
- Stop when the required checks pass, acceptance is covered and no blocker
  remains. After a fix, rerun only what it affects.

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
| Delivery, code review and routine checks | PR or issue comments |
| User installation and usage | Project `README.md` and `README_CN.md` |
| Released and unreleased behavior changes | Owning `CHANGELOG.md` |
| Normative development constraints | Applicable `AGENTS.md` |
| Selected completed decisions and essential host evidence | Existing topic in `docs/project-history/<project>/` |
| Active domain vocabulary and decisions | Context and ADR locations described in `docs/agents/domain.md` |

Link to the owner instead of copying its full content into another document.
Use the [history retention rules](project-history/README.md) to select durable
knowledge before adding a record.
