# Unreal Engine Development Instructions

These instructions apply to the Unreal Engine test project and all plugins
under `unreal/Plugins/`.

## Plugin Boundaries

- Every directory directly under `Plugins/` must be a valid standalone plugin.
- The `.uplugin` file must remain at the plugin root.
- Dependencies must be declared in `.uplugin` and `.Build.cs`.
- Do not depend on files outside the plugin unless explicitly documented.
- Do not place reusable plugin code in the ToolsLab host project.

## Module Structure

- Runtime code belongs in Runtime modules.
- Editor UI, menus, asset actions, and editor utilities belong in Editor modules.
- Public headers expose only intentional public APIs.
- Implementation details belong in `Private/`.
- Avoid unnecessary `PublicDependencyModuleNames`.

## Naming

- Unreal plugins and modules use PascalCase.
- Unreal C++ classes follow Unreal Engine naming conventions.

## Generated Files

Do not commit `Binaries/`, `Intermediate/`, `Saved/`, `DerivedDataCache/`,
`.vs/`, or generated solution and project files.

## Assets

- Keep plugin assets inside the plugin's own `Content/`.
- Do not place plugin assets in the host project's main `Content/`.
- Use Git LFS for Unreal binary assets.
- Do not add large samples without documenting their purpose.

## Validation

After code changes:

1. Compile affected modules.
2. Check for new warnings.
3. Run relevant automation tests.
4. Verify the plugin loads in ToolsLab.
5. Confirm no generated folders were staged.
