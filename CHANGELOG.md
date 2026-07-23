# Changelog

All notable repository-wide changes are documented here. Individual structured
tools and plugins maintain their own changelogs.

## Unreleased

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
