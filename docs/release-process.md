# Release Process

1. Update the project's version and changelog.
2. Run project tests and `python tools/validate_repository.py`.
3. Build or package with the target Maya or Unreal runtime.
4. Inspect the archive contents and perform an installation smoke test.
5. Create a project-prefixed tag: `maya-<name>-v<version>` or
   `ue-<name>-v<version>`.
6. Upload the archive to GitHub Releases. Do not commit it.

Maya archive names use `<ToolName>-<version>.zip`. Unreal archive names include
the engine version, for example `<PluginName>-<version>-UE5.7.zip`.
