# Composite Project Rules

## Product and component boundaries

- Each direct child of `composite/` is one cross-host product with a PascalCase
  name. Host directories use the registered host names from the root AGENTS.md
  (`maya`, `unreal`, `substance-painter`) and contain independently installable
  components: `composite/<ProjectName>/<host>/<ComponentName>/`.
- A product contains at least two host directories. Other lowercase kebab-case
  directories, such as `docs/`, `protocol/`, or `prototypes/`, hold shared
  product material and are not installed.
- Components cooperate through documented protocols or file contracts; they
  must not load source, configuration, or runtime files from sibling host
  directories. Keep versions, dependencies, packaging, tests, and lifecycle
  cleanup independently verifiable.
- Apply each host's rule file (`<host>/AGENTS.md`) to that host's components,
  using paths relative to the repository root. These sibling files need explicit
  reading; their standalone metadata layouts do not apply here.

## Ownership

- The product root alone owns `README.md`, `README_CN.md`, `CHANGELOG.md`, and
  `LICENSE`. The README pair covers installation and usage for each host.
- Components own implementation, host-required descriptors/assets, and relevant
  tests. A local AGENTS.md may capture component-specific contracts or commands;
  do not duplicate inherited rules or product metadata.
- Components follow their host's runtime layout: Maya components use the
  standard tool runtime folders, including at least one of `scripts/`,
  `plug-ins/`, or `icons/`; Unreal code components contain one root `.uplugin`
  and `Source/`, keeping Automation tests with their modules; Substance Painter
  components contain `plugins/` or `startup/`.

## Acceptance

Test changed hosts independently and run applicable package checks. Add product
end-to-end checks for changes to cross-host behavior or contracts. Follow the
repository validation scope; keep standalone and composite metadata ownership
distinct in repository tooling.
