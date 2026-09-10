# Composite Project Rules

## Product and component boundaries

- Each direct child of `composite/` is one cross-host product with a PascalCase
  name. Lowercase host directories contain independently installable components:
  `composite/<ProjectName>/<host>/<ComponentName>/`.
- Components cooperate through documented protocols; they must not load source,
  configuration, or runtime files from sibling host directories. Keep versions,
  dependencies, packaging, tests, and lifecycle cleanup independently verifiable.
- Apply `maya/AGENTS.md` to Maya components and `unreal/AGENTS.md` to Unreal
  components, using paths relative to the repository root. These sibling files
  need explicit reading; their standalone metadata layouts do not apply here.

## Ownership

- The product root alone owns `README.md`, `README_CN.md`, `CHANGELOG.md`, and
  `LICENSE`. The README pair covers installation and usage for each host.
- Components own implementation, host-required descriptors/assets, and relevant
  tests. A local AGENTS.md may capture component-specific contracts or commands;
  do not duplicate inherited rules or product metadata.
- Maya components use the standard tool runtime folders, including at least one
  of `scripts/`, `plug-ins/`, or `icons/`. Unreal code components contain one
  root `.uplugin` and `Source/`; keep Automation tests with their modules.

## Acceptance

Test changed hosts independently and run applicable package checks. Add product
end-to-end checks for changes to cross-host behavior or contracts. Follow the
repository validation scope; keep standalone and composite metadata ownership
distinct in repository tooling.
