# Unreal Engine Development Instructions

These host conventions cover the ToolsLab test project and plugins, including
composite components and templates that reference this file.

## Plugin and module boundaries

- Keep each plugin complete inside its root with its `.uplugin` descriptor.
  Declare applicable plugin/module dependencies in `.uplugin` and `.Build.cs`.
  Document external dependencies; reusable code belongs in its plugin, not ToolsLab.
- Put runtime behavior in Runtime modules and editor-only behavior in Editor
  modules. Public headers expose intentional APIs; implementation stays in
  `Private/`. Use public module dependencies only when the public API needs them.
- Plugin/module names use PascalCase; C++ follows Unreal naming conventions.
- Keep plugin-owned assets in its `Content/`, use Git LFS for binary assets, and
  document substantial samples. Do not commit `Binaries/`, `Intermediate/`,
  `Saved/`, `DerivedDataCache/`, `.vs/`, or generated IDE projects.

## Verification

- For code/build changes, compile affected modules with the supported engine,
  inspect new warnings, and run relevant Automation tests.
- Verify changed host behavior in ToolsLab or the project's designated acceptance
  host. Use the project's documented build/package checks when applicable.
- Capture automated viewport screenshots for visual asset acceptance in Buffer
  Visualization > Base Color (`BaseColor`). Record the viewport mode with the
  evidence; use other view modes for their specific diagnostic purpose.
- Documentation-only changes do not require an engine build. If a required host
  is unavailable, report that limit and use useful available checks without
  claiming host verification.
