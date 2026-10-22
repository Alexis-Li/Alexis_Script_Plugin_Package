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
| `MtoUOptIn.Editor.Linkage.ProductionSequence` | Linkage in a production Level Sequence copy through a real Maya GUI (`maya_gui_linkage_driver.py`): writer takeover, anchor world/screen/Morph oracle, playback under lag and every exit |
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

`maya/MtoULiveLink/tests/maya_gui_install_check.py`, run with a host Python,
checks a standalone install in real Maya GUI sessions: previous release with
a saved shelf button, upgrade, repeated runs and reopening, missing parts and
rollback.

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
