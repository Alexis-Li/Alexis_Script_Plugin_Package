# Release Process

Use this checklist with the owning project rules and
[`naming-conventions.md`](naming-conventions.md). A release is not complete
until the owning project's acceptance checks pass.

1. Update the project's version and changelog.
2. Run the project's tests, lint, host checks, and packaging checks that apply.
   Run `python tools/validate_repository.py` only when the release changes
   repository-owned structure or metadata.
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
