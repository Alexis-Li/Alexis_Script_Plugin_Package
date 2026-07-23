# Maya Development Instructions

## Default to One File

Start with one Python file that runs directly in Maya's Python Script Editor.
Do not add a package, bootstrap module, installer, icon, configuration, or
resource directory unless the requested behavior needs it.

Place a self-contained script at:

```text
maya/scripts/<ToolName>/
├─ <ToolName>.py
├─ README.md
└─ README_CN.md
```

The file must expose `run()` or `main()` and execute that entry point when run
as `__main__`. It must not depend on the repository being on `PYTHONPATH`.

Move a project to `maya/tools/<ToolName>/` only when it needs a Maya plug-in,
multiple functional files, resources, installation, tests, persistent UI,
long-lived callbacks, custom nodes or commands, or a reusable API.

## Tool Layout

Structured Maya tools use Maya's standard runtime directory names directly at
the project root:

```text
maya/tools/<ToolName>/
├─ scripts/       # optional
├─ plug-ins/      # optional
├─ icons/         # optional
├─ presets/       # optional
├─ README.md
├─ README_CN.md
├─ CHANGELOG.md
├─ LICENSE
└─ tests/         # when automated checks are useful
```

Create only the runtime directories the tool uses. Do not add
`package/<ToolName>/` nesting. Keep a single user-facing entry file when that
is enough; add Python packages or separate modules only when the implementation
requires them.

Install small and medium tools by copying runtime files to Maya's matching
directories. Do not add a `.mod` file unless module-based deployment is an
explicit requirement. Declare the version once as `__version__` or
`PLUGIN_VERSION` in a runtime Python file.

## Naming

- Maya project directories use PascalCase: `FlattenMeshToUV`.
- User-run Python files use PascalCase without hyphens or underscores:
  `FlattenMeshToUV.py` or `RunFlattenMeshToUV.py`.
- Internal Python packages and modules may use snake_case when multiple modules
  are genuinely required.
- Classes use PascalCase; functions and variables use snake_case.

## Python Compatibility

- Make Maya runtime code compatible with both Python 2 and Python 3 when
  practical. If one implementation cannot support both, prioritize Python 3
  and state the supported Maya versions in the project README.
- For dual-compatible files, avoid Python-3-only syntax and APIs, use explicit
  compatibility shims only where needed, and test in both host generations
  before claiming support.
- Keep directly executed Maya 2020/Python 2 source ASCII-only; encode localized
  UI text with Unicode escapes so the Script Editor cannot corrupt the source.
- Use the Python and Qt versions bundled with Maya. Do not assume a system
  Python installation, hardcode Maya paths, or add PyMEL unless required.
- Guard Maya- or Qt-version-specific APIs explicitly.

## Runtime Safety

- Validate selections and external input.
- Use actionable Maya warnings and errors.
- Group multi-step scene edits into one undo operation where practical.
- Repeated launches must not create duplicate windows or callbacks.
- Clean up temporary nodes, contexts, Script Jobs, event handlers, and UI.
- Avoid Maya commands during import unless Maya plug-in registration requires it.

## Documentation and Tests

Project `README.md` and `README_CN.md` contain only an introduction, supported
versions, installation, and usage, with matching information in both languages.

Use the smallest runnable check for new logic. Pure Python logic should run
outside Maya where practical; Maya integration checks must use a supported
Maya or `mayapy` and must not modify user preferences or production scenes.
