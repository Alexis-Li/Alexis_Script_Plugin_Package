# Development Conventions

## Scope and boundaries

The repository is a monorepo, but every structured Maya tool and Unreal plugin
is an independently versioned project. Keep runtime code, assets, tests,
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

Structured projects own their README, changelog, version, tests, installation
instructions, and packaging behavior. Release archives go to GitHub Releases,
not Git history.

User-facing projects provide an English `README.md` and matching Chinese
`README_CN.md`. Keep both focused on introduction, installation, and usage;
development and repository-maintenance details belong in `docs/` or
`AGENTS.md`.
