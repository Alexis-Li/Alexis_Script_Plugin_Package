# Maya Development Instructions

## Project Classification

Always start with the smallest structure that safely satisfies the requirement.

### Shelf scripts

Place a shelf script in `maya/scripts/<script-name>/` only when all of the
following are true:

- The implementation reasonably remains one Python file.
- The complete file can run from Maya's Python Script Editor or a shelf button.
- It needs no install process, external package, icon, configuration, UI file,
  asset, public API, test suite, startup hook, background service, persistent
  callback, or Script Job.
- It does not register a Maya node, command, deformer, translator, or compiled
  plug-in. A context implemented entirely by the single script is acceptable
  when running the script directly is the intended shelf workflow.
- The single-file form remains readable and maintainable.

Shelf scripts may use `maya.cmds`, `maya.mel`, `maya.api.OpenMaya`, and small
temporary windows. They must expose a clear `run()` or `main()` entry point and
may invoke it directly when the entire file is intended to be pasted into the
Script Editor or saved to a shelf. They must not depend on the repository being
on `PYTHONPATH`.

Each shelf-script directory contains only `<script_name>.py`, `README.md`, and
`README_CN.md`.
Do not add package, src, core, ui, tests, docs, icons, `.mod`, `pyproject.toml`,
installation scripts, or a project-level `AGENTS.md`.

Validate inputs, show actionable Maya warnings or errors, group multi-step scene
edits into an undo chunk where practical, restore selection when appropriate,
and never leave temporary nodes, namespaces, callbacks, or preference changes.
Document scene changes, undo behavior, and non-undoable operations.

### Structured tools and plug-ins

Place a project in `maya/tools/<tool-name>/` when any of these applies:

- It needs multiple functional Python modules or a persistent/complex UI.
- It separates business logic, scene operations, and UI.
- It needs assets, presets, configuration, installation, tests, packaging,
  versioning, upgrades, or uninstall behavior.
- It registers nodes, commands, deformers, translators, long-lived callbacks,
  menus, shelf items, startup hooks, or event listeners.
- It registers a context that requires installation, external resources, or
  multiple functional modules instead of a self-contained shelf script.
- It exposes a reusable API or is no longer safe to maintain as one file.

A structured project normally owns README, changelog, license, package files,
tests, documentation, and packaging tools. Omit directories that have no actual
purpose. Place Maya module files at `package/<ToolName>.mod` and runtime content
under `package/<ToolName>/`.

Use `core/` for UI-independent business logic, `maya/` for scene and selection
access, `ui/` for PySide interaction, `resources/` for configuration and style,
and `bootstrap.py` as the single public launch entry. Users should not need to
instantiate an internal window class.

### Reclassification

Move a shelf script to `maya/tools/` as soon as it gains a second functional
module, persistent UI, external resource, installer, reusable API, long-lived
callback, custom plug-in behavior, a context that no longer fits one file,
tests, or an architecture boundary. Preserve the original user workflow where
practical, document one public entry point, and add installation and usage
instructions.

## Python Structure

The following rules apply to structured Maya tools. Self-contained shelf
scripts follow the direct-execution rules above.

- Keep business logic separate from Maya scene access and UI code.
- `core/` must not import UI modules.
- `ui/` may call `core/`, but `core/` must not call `ui/`.
- Maya-specific scene access belongs in `maya/`.
- Provide one documented public entry point through `bootstrap.py`.
- Avoid executing Maya commands during module import.

## Maya Compatibility

- Do not assume a system Python installation.
- Use the Python and Qt versions shipped with supported Maya versions.
- Do not introduce PyMEL unless explicitly required.
- Do not hardcode Maya installation paths.
- Guard version-specific APIs explicitly.

## UI

- Parent Maya windows correctly.
- Repeated launches must not create duplicate windows.
- Clean up callbacks, Script Jobs, and event handlers when windows close.
- Keep user-visible errors actionable and concise.

## Tests

- Pure Python logic should be testable outside Maya where practical.
- Maya integration tests must be separate from unit tests.
- Tests must not modify user preferences or production scenes.
