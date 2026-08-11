# Composite Project Rules

## Project Boundary

Each direct child of `composite/` is one cross-host product and uses a
PascalCase directory name. Host directories below it use lowercase names such
as `maya/` and `unreal/`. Each host directory contains a complete,
independently installable component root.

Host components may cooperate through a documented protocol, but they must not
load source, configuration, or runtime files from sibling host directories.
Keep component versions, dependencies, packaging, tests, and cleanup behavior
independently verifiable.

## Required Files

Every composite project owns `README.md`, `README_CN.md`, `CHANGELOG.md`, and
`LICENSE`. The root README pair explains the combined workflow and links to
host-specific installation instructions. Keep both languages aligned.

Maya component roots follow the Maya tool runtime layout and include at least
one of `scripts/`, `plug-ins/`, or `icons/`. Unreal component roots contain
exactly one root `.uplugin` descriptor and the paired user READMEs. Preserve
the nearest host or component `AGENTS.md` rules when they are stricter.

## Validation

Test and package every changed host component independently. For a cross-host
behavior change, also run the product's end-to-end or production acceptance
checks. Never commit generated host output, caches, IDE files, or release
archives.
