# Development Conventions

This is the human-facing change workflow. Normative boundaries and platform
requirements live in the repository and nearest platform `AGENTS.md`; use the
[repository map](workspace-spec.md) to find them.

## Change workflow

1. Read the applicable `AGENTS.md` chain and classify the project before
   choosing a directory.
2. Inspect the owning project, dependency declarations, tests, and packaging
   commands before editing.
3. Make the smallest coherent change and preserve public entry points unless a
   breaking change is explicitly required.
4. Update the owning user documentation and changelog when behavior changes.
5. Run the narrowest project checks, then repository validation and tests.
6. Inspect `git status` for generated or unrelated files before handoff.

The repository-wide gates are:

```powershell
python tools/validate_repository.py
python -m unittest discover -s tests -v
```

Host-dependent Maya and Unreal checks remain project-specific and run in the
supported host runtime.

## Repository tools

Repository Python tools must use the standard library unless a dependency is
explicitly approved. Write commands default to dry-run and require `--apply`.
Maya and Unreal runtime code must use the versions shipped by the target host.

## Documentation ownership

| Information | Owner |
| --- | --- |
| User installation and usage | Project `README.md` and `README_CN.md` |
| Released and unreleased behavior changes | Owning `CHANGELOG.md` |
| Normative development constraints | Applicable `AGENTS.md` |
| Stable completed architecture and acceptance evidence | `docs/project-history/<project>/` |
| Active domain vocabulary and decisions | Context and ADR locations described in `docs/agents/domain.md` |

Link to the owner instead of copying its full content into another document.
