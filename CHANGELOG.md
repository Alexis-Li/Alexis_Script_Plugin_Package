# Changelog

All notable repository-wide changes are documented here. Standalone structured
tools and plugins maintain their own changelogs; composite products maintain
one changelog at the composite project root.

## Unreleased

- Scope routine acceptance to the owning project and reserve repository-wide
  validation for repository tooling, shared rules, metadata, and layout changes.
- Move MtoU_LiveLink into the new `composite/` category with independently
  installable Maya and Unreal component roots.
- Standardize composite metadata ownership at the project root while preserving
  the existing standalone Maya and Unreal project contracts.
- Replace temporary agent implementation plans with a stable per-project
  development-history archive.
- Add the MtoU_LiveLink Maya sender and Unreal Live Link receiver.
- Establish the Maya and Unreal Engine monorepo workspace.
- Add repository validation, project creation, packaging, and version tools.
- Use PascalCase for Maya project directories and user-run tool files.
- Flatten structured Maya projects into root `scripts/`, `plug-ins/`, `icons/`,
  and `presets/` directories without `package/<ToolName>/` wrappers or required
  `.mod` files.
- Prefer directly runnable, Python 2/3-compatible Maya files; prioritize Python
  3 when one implementation cannot support both.
- Expand the bilingual repository READMEs and keep project READMEs limited to
  introduction, supported versions, installation, and usage.
- Update project templates and repository tools for the new Maya layout.
