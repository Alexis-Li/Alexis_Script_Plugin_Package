# Maya Development

## Choose the Smallest Project Shape

Prefer `maya/scripts/<ToolName>/<ToolName>.py`: one PascalCase file that exposes
`run()` or `main()` and runs directly in Maya's Python Script Editor. Add no
installer, package, bootstrap layer, or resource folder unless the feature
needs it.

Use `maya/tools/<ToolName>/` when the tool requires plug-in registration,
multiple functional files, resources, persistent UI, tests, installation, or a
reusable API. Put Maya runtime folders directly under the project:

```text
<ToolName>/
├─ scripts/       # optional
├─ plug-ins/      # optional
├─ icons/         # optional
└─ presets/       # optional
```

Do not wrap this structure in `package/<ToolName>/`. Omit unused directories
and install by copying runtime files to Maya's matching directories. Do not add
a `.mod` file unless module-based deployment is an explicit project requirement.

Declare the project version once as `__version__` or `PLUGIN_VERSION` in a
runtime Python file. Repository version and packaging commands use that value.

## Compatibility

Support Python 2 and Python 3 in Maya runtime code where practical. Avoid
Python-3-only syntax in dual-compatible files and verify both host generations
before documenting compatibility. If the implementations cannot be shared,
prioritize Python 3 and document the exact supported Maya versions.

Use Maya's bundled Python and Qt, avoid hardcoded install paths, and guard
version-specific APIs. Do not introduce PyMEL unless the feature requires it.

## Safety and Verification

Validate selection and input, keep errors actionable, preserve undo behavior,
and clean up temporary nodes, windows, callbacks, Script Jobs, and contexts.
Test pure logic outside Maya where practical and keep Maya integration checks
separate from repository-level tests.
