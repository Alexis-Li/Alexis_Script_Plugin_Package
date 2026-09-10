# Unreal Plugin Guide

Use this guide for repository navigation. The normative Unreal module, asset,
dependency, generated-file, and validation rules live in
[`unreal/AGENTS.md`](../unreal/AGENTS.md).

## Plugin locations

| Plugin type | Location |
| --- | --- |
| Standalone plug-in | `unreal/Plugins/<PluginName>/` |
| Unreal component of a cross-host product | `composite/<ProjectName>/unreal/<PluginName>/` |

Both locations contain a complete, independently installable plug-in with its
`.uplugin` descriptor at the plug-in root. Composite product metadata remains
at `composite/<ProjectName>/`.

## Design guide

Runtime features belong in Runtime modules. Menus, editor UI, asset actions,
and editor utilities belong in Editor modules. `Public/` contains intentional
APIs; implementation details stay in `Private/`.

Assets belong in the plugin's own `Content/`. When there are no assets, omit the
directory and set `CanContainContent` to `false`. The ToolsLab project only hosts
and validates plug-ins; reusable plug-in code stays in its owning plug-in.

## Verification path

For code/build changes, compile affected modules, inspect new warnings, and run
relevant Automation tests. Verify changed host behavior in ToolsLab or the
project's designated acceptance host. Documentation-only changes need no engine
build; repository gates follow the scope in the root AGENTS.md. Finish by checking
`git status` against the generated-file rules in `unreal/AGENTS.md`.
