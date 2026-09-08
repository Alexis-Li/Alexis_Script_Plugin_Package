# Cached Playback reconnect acceptance

Date: 2026-09-08. Follow-up to Issue #37's independent review of `48ca965`.
Starting tree: `1581543ccbadbe22cdaa0b5d4f2af57b143e01e0` on `main`.
The follow-up commit containing this record adds acceptance coverage; the
production session-reset behavior and protocol v6 are unchanged.

## Delayed session delivery

`MtoULiveLink.Source.CacheSessionReconnect` keeps one listener/source through
three real socket negotiations. It retains the original fresh upload/play=1,
transport-loss cleanup, and same-session stale IDs after clear checks.

A development-only friend seam now saves A's transport identity and injects
synthetic delayed A envelopes into the actual command and outgoing queues:
frame, end, play, clear, cache_ready, progress, completion, and an error that
would close the connection if delivered. A and B intentionally reuse operation
IDs. The progress/completion sentinel is 777 applied frames.

Delivery occurs after B negotiates, after B's upload reaches Ready, and after
B's first playback frame is observed. The test observes socket silence and no
published pose before B plays, then B's own two poses and identity-matched
completion without A's progress/completion/error. The injected inputs exercise
the production queue-consumer and worker send gates; they do not pretend to be
a naturally reproduced OS scheduling race or assign private identity counters.

## Real Maya to Unreal run

Hosts: Maya 2022.4 (`apiVersion=20220400`, bundled Python 3.7) and stock Unreal
Editor 5.7.4 / CL 51494982. Unreal used ToolsLab, NullRHI and the memory DDC.
`MtoULiveLink.Source.MayaCacheReconnect` launches the supplied `mayapy` with
`maya_reconnect_peer.py` and uses a transient SkeletalCube Binding Actor.

The peer creates an unsaved, skinned, two-joint Maya character with four
animated frames at 30 fps. It uses the production `_CharacterScene`,
`_Controller.connect`, capture/replay actions, `_StreamingSession`, sender,
cache file and retention code. Only error-dialog presentation is overridden.
Because mayapy has no UI idle loop, the test pumps the real ready, capture and
cache-poll callbacks on Maya's main thread. This is a real two-host integration
check, not a Maya UI/viewport or production-character visual acceptance claim.

1. A connects and captures frames 1–4 once. Upload 1 and play 1 complete with
   four accepted publications and four evaluated Live Link poses.
2. The peer shuts down A's actual socket. The cached poller observes transport
   loss; the real controller detaches and retains the completed compatible
   cache. No synthetic terminal event is supplied.
3. B explicitly reconnects to the same UE source. A new Cached Playback owner
   consumes the retention. Cache path, summary and SHA-256 remain identical.
4. Maya's current frame-4 animation is changed to X=999. B's replay action
   uploads the retained file using fresh upload 1 and play 1. UE still applies
   the original four poses, proving it did not recapture the changed animation.
5. Controller close removes the temporary cache. No scene, level or asset is
   saved. UE-generated configuration residue is removed after testing;
   no generated files enter the commit.

| Frame index | A session | B session | Accepted root X | Evaluated root X in both |
| --- | --- | --- | --- | --- |
| 0 | 1 | 2 | 10 | 10 |
| 1 | 1 | 2 | 20 | 20 |
| 2 | 1 | 2 | 30 | 30 |
| 3 | 1 | 2 | 40 | 40 |

Both completions report four frames. The observer records only frames for which
the real source publication succeeds, and evaluates the resulting subject after
each source update. Session ownership, exact frame count and A/B pose equality
are asserted independently of Maya's completion state.

The accepted run's cache SHA-256 was
`51c29812dc1e6a943a76063f14e945abab839aebbcc8f10fb3878f19d5bebb4a`.
The unchanged Maya runtime SHA-256 was
`0c9a4fcf839c6af81ae989a5501fa5fd137e8f6c9095c09c23a8ecdf6102c6db`.

## Reproduction

Set `$EngineRoot`, `$Mayapy`, `$RepoRoot` and `$Evidence` to local absolute
paths; create `$Evidence` first. The evidence directory is external to the
repository. The peer stays beside its owning Maya implementation; the Unreal
component does not acquire a runtime dependency on Maya.

```powershell
& "$EngineRoot/Engine/Build/BatchFiles/Build.bat" UnrealEditor Win64 Development "$RepoRoot/unreal/ToolsLab.uproject" -WaitMutex -NoHotReloadFromIDE
& "$EngineRoot/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "$RepoRoot/unreal/ToolsLab.uproject" -unattended -nop4 -nosplash -NullRHI -DDC-ForceMemoryCache '-ExecCmds=Automation RunTests MtoULiveLink.Source.MayaCacheReconnect+MtoULiveLink.Source.CacheSessionReconnect' '-TestExit=Automation Test Queue Empty' "-MtoUMayapy=$Mayapy" "-MtoUMayaPeer=$RepoRoot/composite/MtoULiveLink/maya/MtoULiveLink/tests/maya_reconnect_peer.py" "-MtoUEvidence=$Evidence" "-ReportExportPath=$Evidence/report" "-abslog=$Evidence/editor.log"
```

Inspect `report/index.json`, its test entries (including every applied pose),
and `maya-result.json`; process exit code alone is insufficient. Without
`MtoUMayapy`, the ordinary suite reports that the opt-in host check was not
requested; that entry is not host acceptance evidence.

## Verification results

| Check | Result |
| --- | --- |
| Stock UE Development Editor build, affected Runtime module | Pass; no new compiler/linker warnings |
| Full `Automation RunTests MtoULiveLink`, with Maya opt-in arguments | 56/56 successful: 50 without warnings, 6 with expected/bounded warnings; 0 failed/notRun |
| Final restored build and two reconnect tests | 2/2 successful, including the real Maya peer |
| Maya pure suite | 121/121 passed |
| Maya 2022.4 `maya_host_tests.py` | 19/19 passed |
| Scoped Ruff, Python 3.7 grammar, peer `--help` | Passed |
| Protocol corpus `--check` | Passed |
| Unreal/Maya package dry-runs | Passed, 32/1 runtime package files respectively |
| Final diff whitespace and generated-file review | Passed |

The full suite ran before tightening the sentinel assertion to match its exact
JSON field (avoiding an accidental match in elapsed-time decimals); the final
affected tests were rerun after that assertion change and all negative-control
source edits were restored. Source implementation bytes match the starting
production source. The only source-header change is a development-automation
friend declaration, following the existing actor test-seam convention.

Three temporary negative controls were compiled and executed independently:

| Removed protection | Expected failure actually observed |
| --- | --- |
| Cache command transport-session gate | Delayed A commands produce B replies and prevent B's own upload/play completion |
| Outgoing reply transport-session gate | A replies appear on B; delayed error breaks B's connection and subsequent operations |
| New-session identity reset at both boundaries, replaced by `ResetToIdle` | Real Maya A succeeds, B upload 1 is rejected as stale (`last seen 1`); only A's four publications/evaluated poses exist |

These are controlled mutations of the current implementation, not claims that
an exact historical checkout was rebuilt. The original pre-fix regression
failure remains documented in Issue #37's earlier implementation comment.
All mutations were restored before the final successful build and replay.

The test project is ToolsLab and the fixture is deliberately small and
transient. The supplied external Backups project and production animation
assets were not modified. This record establishes the requested real-host
reconnect/cache recovery and delayed-envelope isolation. It does not extend
production-character visual/performance or third-party-engine acceptance.
