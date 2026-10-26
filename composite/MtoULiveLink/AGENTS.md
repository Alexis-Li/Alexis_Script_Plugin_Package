# MtoULiveLink Rules

Commands run from the repository root. `<mayapy>` is Maya 2024
`bin/mayapy.exe`; also use Maya 2022.4 when a change touches Maya API usage or
Python syntax that 2022 could reject.

## Default checks

Run the suites of the hosts a change touches. Run both hosts when the protocol,
shared fixtures, or cross-host behavior changes.

| Scope | Command |
| --- | --- |
| Maya, pure Python | `python -m unittest discover -s composite/MtoULiveLink/maya/MtoULiveLink/tests -p "test_*.py"` |
| Maya, host | `<mayapy> -m unittest discover -s composite/MtoULiveLink/maya/MtoULiveLink/tests -p maya_host_tests.py` |
| Protocol corpus | `python composite/MtoULiveLink/protocol/generate_unreal_corpus.py --check` (when `protocol/` or the corpus changes) |
| Unreal | Build stock UE 5.7.4, then `UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests MtoULiveLink; Quit" -unattended -nopause -nosplash -NullRHI -ReportExportPath=<scratch>` |

Run the repository gates only when the root rules require them; a change
confined to this product does not.

## Opt-in tests

`MtoUOptIn.*` tests are excluded from the default run because they need a real
Maya process, a production fixture, or a long measurement. Run only those the
task affects, with the arguments each test names in its log message. Without
those arguments a host test passes with only an info message; that pass is not
evidence.

| Test | Use |
| --- | --- |
| `MtoUOptIn.Source.MayaCacheReconnect`, `.MayaInteractivePlayback`, `.MayaCharacterPartsHost`, `.MayaLinkageHost` | Cross-host acceptance of cache, playback, character parts, and linkage |
| `MtoUOptIn.Editor.Preview.MayaRefreshPeer`, `.CharacterAcceptance` | Real Maya Preview refresh and complete-character acceptance |
| `MtoUOptIn.Editor.Linkage.ProductionSequence` | Linkage in a production Level Sequence copy through a real Maya GUI (`maya_gui_linkage_driver.py`): writer takeover, anchor world/screen, bone-rotation and Morph oracle, playback under lag and every exit. The oracle frames must show different keyed poses: their Maya times must not lie a whole number of `2 * maya.key_step` apart (the driver default `key_step` 42 suits oracle frames 300/900/1500) |
| `MtoUOptIn.Editor.Workflow.GuiRegression` | Real-time, cached playback, Model/Refresh/garment comparison and Animation after Model through the same Maya GUI driver; start the editor on `/Engine/Maps/Entry` |
| `MtoUOptIn.Editor.Preview.RefreshBenchmark` | Preview refresh performance changes; `-MtoUBenchmarkCase=<case>` measures one case in a fresh process |

The two Maya GUI tests render the Binding Actor's Details at the fixture's
`details_widths` only when the editor can render: use `-RenderOffscreen`
instead of `-NullRHI`. Every MtoU group is expanded and the render grows until
all rows fit, so collapsed groups and rows below the fold are checked too.

Both tests also fail on any Maya dialog the scenario did not expect. The driver
records each dialog with the stage (`stage` command) that was current; the test
lists what each stage may show: `BLENDSHAPE_MISMATCH` on every connect of the
fixture character, and `STREAM_INTERRUPTED` where the exit ends the session
(`destroy_target`, `close_world`, and Refresh Preview in `GuiRegression`). A new
expected dialog is added to that list, not skipped.

Maya's linkage throughput depends on the plug-ins its preferences load
automatically, so give both tests `maya.app_dir_template` instead of a reused
`maya.app_dir`. The test copies the template to `<evidence>/mayaapp` for every
run and records in its log the source, the SHA-256 and autoload count of the
`pluginPrefs.mel` in the running Maya's own preference folder, and the plug-ins
loaded during playback (`ProductionSequence` also in `ue-run.json` as
`maya_prefs`). The driver's `sample_plugins` command takes that sample a
quarter into `ProductionSequence` playback and during `GuiRegression` cached
playback; a later stage never replaces it. Build the templates once per machine outside the repository with
a host Python:

```powershell
python composite/MtoULiveLink/maya/MtoULiveLink/tests/maya_prefs_template.py --maya <Maya2024>/bin/maya.exe --kind minimal --out <scratch>/maya-prefs-minimal
```

`minimal` (only `modelingToolkit`) is the comparable baseline; `maya-default`
(the list a first Maya start saves) measures a default Maya install. A template
holds `pluginPrefs.mel` and a `userPrefs.mel` with only Maya's one-time
plug-in load markers; without those markers Maya loads its default set
whatever `pluginPrefs.mel` says. Compare throughput only between runs with the
same SHA-256; with `maya-default` the loaded count varies between runs because
Maya is still autoloading while it plays.

`maya/MtoULiveLink/tests/maya_gui_install_check.py`, run with a host Python,
checks the packaged `install.mel` in real Maya GUI sessions: upgrade from the
0.9.0 single file with a saved shelf button, Chinese and ASCII install paths,
reinstall, an update while the tool is open, a broken and a partial package,
missing parts, rollback and a clean install. Pass
`--old-entry` the 0.9.0 `MtoULiveLink.py` (`git show 799fb07^:composite/MtoULiveLink/maya/MtoULiveLink/scripts/MtoULiveLink.py`).

`maya/MtoULiveLink/tests/maya_gui_window_check.py`, also run with a host
Python, closes the panel and the scene-reference window from their title bars
(`WM_CLOSE`), repeats close and reopen, switches workflow and scenes, and
quits Maya in each state; a scenario passes only when Maya exits with code 0,
in time and without a crash log. Run it after changing window or panel
lifecycle. Pass `--app-dir-template` a Maya 2024 preference directory a person
has started Maya with once and whose first-start dialogs they answered; the
check copies it for every process and never answers those dialogs. Without
such a template, report it as a missing prerequisite instead of starting Maya
with an empty preference directory.

Before 2026-10-19 these tests used the `MtoULiveLink.` prefix with the same
remaining path; acceptance records use the old names.

`prototypes/` is frozen verification scaffolding. Its Unreal plugins are
disabled in `unreal/ToolsLab.uproject`, and product work does not run its tests.

mayapy has no idle loop: the tool queues its main-thread work in batch mode,
and a host loop (the `maya_*_peer.py` scripts) must call
`mayaapi.drain_deferred()` on its own thread.

## Iteration

- After a failure, rerun only the failing Unreal test (for example
  `Automation RunTests MtoULiveLink.Linkage.Session`) or Python module. Run each
  affected default suite once more after the change stabilizes.
- Inspect new Unreal warnings. A warning a test triggers on purpose is declared
  with `AddExpectedMessagePlain` so that only unexpected warnings appear in the
  report.
- Wait on the reply or state a test needs, not on a fixed read timeout per
  step; only a protocol window that is itself under test may set the duration.

## Versions

The product version (Maya `__version__`, Unreal `VersionName` and `Version`)
names the next release and changes only in the release change, for both
components together; PRs leave it alone. It is 0.10.0 until that release.
The protocol version both hosts negotiate still changes with the wire
contract; the PR that changes it adds an `Upgrade notes` entry under
`Unreleased` instead of bumping the product version.

## Changelog

Write `CHANGELOG.md` for users upgrading. Every PR with a user-visible change
adds its entry under `## Unreleased`; the release change only renames that
heading to the version and date. Group each version's entries under
`### Upgrade notes`, `### Added`, `### Changed`, `### Fixed` and `### Removed`,
in that order, omitting empty groups. Upgrade notes say what the user must do:
update both hosts together, reinstall, redo a setting or replace a removed
control. Each entry is one or two lines in the panels' own terms and ends with
its issue:

```markdown
- Long root and outfit names no longer widen the Maya panel; the full name is in the tooltip. (#71)
```

Leave out implementation, review finding IDs, tests, tooling and prototypes;
the issue, PR and project history hold them. Rewriting a released version
in this format keeps what it shipped and adds nothing it did not ship.
