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
`LICENSE` exactly once at its project root. The root README pair contains the
host-specific installation and usage instructions. Keep both languages aligned.
Host component roots contain implementation and colocated tests only; do not
duplicate project metadata or add component-level `AGENTS.md` files there.

Maya component roots follow the Maya tool runtime layout and include at least
one of `scripts/`, `plug-ins/`, or `icons/`; keep Maya tests in that component
when they require its source layout or host runtime. Unreal component roots
contain exactly one root `.uplugin` descriptor and their runtime `Source/`;
keep Unreal Automation tests embedded with the module source they exercise.

## Validation

Test and package every changed host component independently. For a cross-host
behavior change, also run the product's end-to-end or production acceptance
checks. Repository validation must apply the root metadata contract to composite
projects without weakening the existing metadata requirements for standalone
projects under `maya/` and `unreal/`. Never commit generated host output, caches,
IDE files, or release archives.
