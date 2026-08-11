# Release Process

1. Update the project's version and changelog.
2. Run project tests and `python tools/validate_repository.py`.
3. Build or package with the target Maya or Unreal runtime.
4. Inspect the archive contents and perform an installation smoke test.
5. Create a project-prefixed tag: `maya-<name>-v<version>` or
   `ue-<name>-v<version>`. Use `composite-<name>-v<version>` when one release
   coordinates multiple host components.
6. Upload the archive to GitHub Releases. Do not commit it.

Maya archive names use `<ToolName>-<version>.zip`. Unreal archive names include
the engine version, for example `<PluginName>-<version>-UE5.7.zip`. Composite
projects publish those host-specific archives separately; do not wrap them in
one combined archive. Composite component archives contain only the files
required by that host; publish the shared README, changelog, and license once
from the composite project root.
