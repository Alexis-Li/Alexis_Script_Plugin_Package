# Maya Development

## Classification

Use `maya/scripts/<script-name>/` only when the complete tool is one
self-contained Python file that can be pasted into Maya's Python Script Editor.
It must not need installation, resources, tests, a reusable API, persistent UI,
long-lived callbacks, custom commands, nodes, or plug-in registration. A
context may stay as a shelf script when it is fully contained in that one file
and direct Script Editor execution is the intended workflow.

Use `maya/tools/<tool-name>/` when any structured-tool condition applies. Keep
business logic in `core/`, Maya scene access in `maya/`, UI in `ui/`, and expose
one documented entry point through `bootstrap.py`. Avoid Maya commands at import
time and clean up windows, callbacks, Script Jobs, and contexts.

## Compatibility and testing

Do not assume system Python or hardcode a Maya install. Use Maya's bundled
Python and Qt bindings, guard version-specific APIs, and avoid PyMEL unless it
is explicitly required. Pure logic tests may run outside Maya; integration
tests must use `mayapy` and must not change user preferences or production scenes.

## Packaging

Maya module packages place a `.mod` file beside the module directory. Package
only project-owned runtime files and exclude caches, tests, and local settings.
