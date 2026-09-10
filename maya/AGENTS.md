# Maya Development Instructions

These host conventions also apply when referenced by composite projects or
templates. Composite roots own their shared metadata.

## Project shape and installation

- Use `maya/scripts/<ToolName>/<ToolName>.py` for a self-contained Script Editor
  or shelf script, with its README pair beside it. Expose `run()` or `main()` and
  invoke it under `__main__`; do not depend on the repository being on PYTHONPATH.
- Use `maya/tools/<ToolName>/` for structured tools. Choose modules, packages,
  resources, and UI structure according to maintainability and runtime needs.
  A small tool can still use one entry file; do not force complex tools into one.
- Put standard runtime folders (`scripts/`, `plug-ins/`, `icons/`, `presets/`)
  directly at the tool root, creating only those needed. The repository validator
  requires at least one of scripts, plug-ins, or icons. Avoid `package/<ToolName>/`
  nesting. Standalone tools also own README pairs, CHANGELOG.md, and LICENSE.
- Prefer copying runtime files for simple installs. Use Maya module deployment
  (`.mod`) when isolation, resources, or deployment needs justify it; document
  installation without hardcoded machine paths and include the descriptor in
  project packaging. The shared packager currently includes runtime folders and
  metadata, so extend it or use project packaging when introducing a descriptor.
- Declare one consistent runtime version through `__version__` or
  `PLUGIN_VERSION`. Keep tests beside the owning implementation; shelf script
  tests may use `tests/` without changing the runtime classification.

## Naming and compatibility

- Project directories and user-run Python files use PascalCase, such as
  `FlattenMeshToUV/FlattenMeshToUV.py`. Internal modules, functions, and variables
  use snake_case; classes use PascalCase.
- Target requested or declared Maya versions and preserve existing support.
  New tools default to a supported Python 3 host; add Python 2 support only for a
  required legacy host. Distinguish intended compatibility from versions tested.
- Use Maya's bundled Python and Qt. Guard version-specific APIs and add PyMEL
  only when justified by the tool's needs.
- For Python 2 targets, avoid unsupported syntax/APIs and verify both generations
  before claiming dual support. Directly executed Maya 2020/Python 2 source stays
  ASCII-only with Unicode escapes for localized text to avoid Script Editor
  encoding corruption.

## Scene safety and verification

- Validate selections and provide actionable Maya warnings/errors. Group scene
  edits into an undo operation where practical.
- Repeated launches must not duplicate windows or callbacks. Clean up temporary
  nodes, contexts, Script Jobs, event handlers, and UI.
- Avoid Maya commands during import except required plug-in registration.
- Test pure Python outside Maya where practical. Use supported Maya or `mayapy`
  for host behavior, with disposable scenes and isolated preferences.
