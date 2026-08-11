# Unreal Engine Development

Each direct child of `unreal/Plugins/` is a standalone plugin with a root
`.uplugin` file. A composite Unreal component at
`composite/<ProjectName>/unreal/<PluginName>/` follows the same standalone
plugin contract and must not load files from sibling host directories. Runtime features belong in Runtime modules; menus, editor UI,
asset actions, and editor utilities belong in Editor modules. Expose only
intentional APIs from `Public/` and keep implementation details in `Private/`.

Assets belong in the plugin's own `Content/`. When there are no assets, omit the
directory and set `CanContainContent` to `false`. The ToolsLab project only hosts
and validates plugins; it must not contain reusable plugin code.

After a change, compile affected modules, inspect new warnings, run relevant
automation tests, load the plugin in ToolsLab, and confirm generated directories
are not staged. Never commit `Binaries`, `Intermediate`, `Saved`,
`DerivedDataCache`, `.vs`, or generated solution/project files.
