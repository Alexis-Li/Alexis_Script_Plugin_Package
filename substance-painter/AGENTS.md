# Substance 3D Painter Development Instructions

These host conventions cover Adobe Substance 3D Painter Python plug-ins,
including composite components and templates that reference this file.
Composite roots own their shared metadata.

## Project shape and installation

- Use `substance-painter/plugins/<PluginName>/` for a standalone plug-in and
  `composite/<ProjectName>/substance-painter/<PluginName>/` for a component of a
  cross-host product. Standalone plug-ins also own README pairs, CHANGELOG.md,
  and LICENSE.
- Mirror Painter's user Python directory at the project root and create only
  the folders used: `plugins/` for plug-in modules or packages listed in
  Painter's Python menu, `modules/` for shared modules imported by plug-ins, and
  `startup/` only for behavior that must run at launch without user control.
  The repository validator requires `plugins/` or `startup/`. Installation
  copies these folders into the matching folders of Painter's user Python
  directory; document it without machine-specific paths.
- Plug-in modules and packages use lowercase snake_case and expose
  `start_plugin()` and `close_plugin()`. Declare one consistent `__version__`
  across runtime Python files.
- Keep tests in the project's `tests/` directory; they are not installed.

## Compatibility

- Target the declared Painter versions. Use the Python interpreter and Qt
  binding that Painter ships; do not depend on packages installed into a system
  Python. Painter releases have changed the bundled Python and Qt binding, so
  guard version-specific imports or declare a single supported release line.
- Use the documented `substance_painter` API. Record which API behaviors a
  feature relies on and verify them on each claimed version. Distinguish
  declared compatibility from versions actually tested.

## Runtime safety

- `close_plugin()` reverses everything `start_plugin()` set up: event
  connections, docks, menus, toolbars, timers, and worker threads. Reloading a
  plug-in must not duplicate UI or handlers.
- Event callbacks run on Painter's main thread. Keep them short: record state
  and schedule work instead of exporting or doing disk I/O inside a callback.
- Call the Painter API from the main thread. Worker threads may do pure Python
  work such as hashing or copying files and hand results back to the main thread.
- Change the user's project (layers, texture sets, export presets, metadata)
  only as the feature documents. Store plug-in data under a plug-in-owned
  metadata key, write files only to configured or plug-in-owned locations, and
  never delete files the plug-in did not create.

## Verification

- Keep host-independent logic (scheduling, configuration validation, file
  publishing) in modules that do not import `substance_painter`, and test it
  with plain Python outside Painter.
- Verify host behavior in a supported Painter with a disposable test project
  and record the Painter version used. If Painter is unavailable, report that
  limit and do not claim host verification.
- Repository packaging, version-update, and project-template commands do not
  cover Painter yet. The first Painter project adds them as part of its delivery,
  using archive name `<PluginName>-<version>-SP.zip`.
