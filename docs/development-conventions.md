# Development Conventions

## Scope and boundaries

The repository is a monorepo, but every structured Maya tool and Unreal plugin
is an independently versioned project. Composite products group two or more
independently installable host components under `composite/`; they may share a
documented protocol but must not load files from sibling host roots. Keep runtime code, assets, tests,
documentation, and package metadata inside the owning project. Do not create
nested Git repositories or undocumented dependencies between sibling projects.

## Change workflow

1. Classify the project before choosing a directory.
2. Make the smallest coherent change and preserve public entry points.
3. Update user documentation and the owning changelog for visible behavior.
4. Run the narrowest project tests, then `python tools/validate_repository.py`.
5. Inspect `git status` for generated or unrelated files.

Repository Python tools must use the standard library unless a dependency is
explicitly approved. Write commands default to dry-run and require `--apply`.
Maya and Unreal runtime code must use the versions shipped by the target host.

## Project ownership

Structured projects own their README, changelog, version, runtime dependencies,
installation metadata, and any project-specific tests or documentation they
need. Composite products additionally own a root README pair, changelog, and
license while each host component keeps its own installation metadata.
Repository tools provide shared validation and packaging behavior.
Release archives go to GitHub Releases, not Git history.

The root English and Chinese READMEs explain the repository, list its tools,
link documentation, and show how to start development. Project READMEs contain
only an introduction, supported versions, installation, and usage;
development and repository-maintenance details belong in `docs/` or `AGENTS.md`.
