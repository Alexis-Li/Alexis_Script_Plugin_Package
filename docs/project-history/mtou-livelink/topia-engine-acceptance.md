# MtoU_LiveLink Topia-Engine Build and Load Acceptance

Date: 2026-08-28
Revalidated: 2026-09-03 by project-owner manual acceptance after the final
0.4.0 Unreal changes

Status: Passed for Win64 plugin compilation and Athena editor loading with
Topia Engine 5.7.4. Full functional and production-scene acceptance remains
owned by the stock-engine acceptance scope.

## Scope

This gate verifies that the two MtoU_LiveLink editor modules can be compiled
against the company's installed Topia Engine 5.7.4 and loaded by the Athena
editor without changing engine files or host-project source/configuration
files. Only generated files under the target plugin's `Binaries/Win64` are
installed.

It does not claim that every modified Unreal Engine 5.7 build is compatible,
and it does not replace the stock-engine automation or production-character
acceptance records.

## Installed-engine constraints

Three unrelated installed-build conditions prevent the ordinary project and
`BuildPlugin` paths from reaching the MtoU_LiveLink compiler:

- the host project's UnLua UBT plugin contains a read-only generated NuGet
  `obj` file;
- the engine's all-modules editor target discovers an engine test plugin whose
  `RuntimeTests` module is unavailable;
- the installed engine's Verse generated header is read-only under
  `Engine/Intermediate`.

The accepted build does not change permissions or contents at any of those
locations. It copies the project plugin into a temporary HostProject, creates a
temporary writable copy of the small generated-header/build-rules surface,
references the remaining installed-engine directories read-only, disables the
temporary target's all-modules behavior, and builds with the installed
precompiled engine.

## Evidence

- UnrealHeaderTool processed the staged plugin headers successfully.
- UnrealBuildTool completed all 22 actions and linked
  `UnrealEditor-MtoULiveLink.dll` and
  `UnrealEditor-MtoULiveLinkEditor.dll`.
- The generated plugin manifest BuildId matched both the selected engine and
  Athena's existing editor manifest.
- Athena mounted the project plugin without an explicit enable override and
  reached editor initialization without a missing, incompatible, or
  uncompiled MtoU_LiveLink module error.
- Temporary HostProject, engine view, logs, and redirected editor state were
  removed after acceptance.

## Repeatable command

Set `TOPIA_ENGINE_ROOT` to the directory containing `Engine` and
`ATHENA_UPROJECT` to the target `.uproject`. Run a read-only validation first:

```powershell
pwsh ./tools/build_mtou_topia.ps1 `
  -EngineRoot $env:TOPIA_ENGINE_ROOT `
  -ProjectFile $env:ATHENA_UPROJECT `
  -Json
```

Close the target Unreal Editor, then build and install:

```powershell
pwsh ./tools/build_mtou_topia.ps1 `
  -EngineRoot $env:TOPIA_ENGINE_ROOT `
  -ProjectFile $env:ATHENA_UPROJECT `
  -Apply
```

Exit codes are stable: `0` success, `2` invalid input, `3` build failure, and
`4` install failure. A failed build keeps its temporary directory and log for
diagnosis; a successful build removes them.

## Remaining boundary

Generated plugin binaries are local build output. They should not be submitted
to source control unless the project team explicitly adopts a shared binary
distribution policy. A team-wide workflow should move this same command to a
controlled build machine or CI runner with access to the pinned Topia engine.
