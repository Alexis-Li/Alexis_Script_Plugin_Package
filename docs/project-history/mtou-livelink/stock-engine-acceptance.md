# MtoU_LiveLink Stock-Engine Acceptance

Date: 2026-08-11
Status: Passed for stock Unreal Editor 5.7.4
Record type: Historical acceptance evidence; current production fixture differs

## Scope

This acceptance used the supplied Maya 2022 binding scene
`SK_C04_Last09.0013.ma` and its normal-workflow export
`SK_C04_Clothes_09.fbx`. The third-party-modified Unreal Engine 5.7 build was
excluded by direction and has no compatibility claim.

## Current Production Fixture

The production fixture selected on 2026-08-20 for the next acceptance run is:

- animation scene: `C01_Body_IdleStand02_ChangeClothes.ma`;
- referenced binding scene: `SK_C01.ma`;
- Unreal import source: `SK_C01_Clothes_09.fbx`.

The animation scene is Maya 2022 ASCII at 30 fps, uses playback frames 0 through
320 inclusive, and references `SK_C01.ma` as `SK_C01RN` in the `SK_C01`
namespace. Acceptance must resolve that reference to the supplied binding scene
without modifying or saving either source scene.

This C01 fixture passed the complete stock-engine acceptance gate on
2026-09-02; the run is recorded in the C01 Production Acceptance section
below. The C04 results and measurements below remain valid historical
evidence for the files actually tested on 2026-08-11; they must not be
reported as C01 results.

The Cached Playback happy path and capture-safety guards are covered by
deterministic Maya-side capture/replay tests, including ordered delivery,
timeline restoration, cancellation cleanup, disk-space rejection, and exact
cache finalization. This is implementation evidence only; it complements, and
does not replace, the stock-engine C01 run across all 321 display frames
recorded below.

## Issue #8 C01 Validation Attempt

Date: 2026-08-21
Status: Blocked by the C01 Real-time Preview performance gate; Issue #8 remains
open.

The unchanged `C01_Body_IdleStand02_ChangeClothes.ma` scene was opened in Maya
2022.4 with its `SK_C01.ma` reference resolved and the `SK_C01_Clothes_09.fbx`
derived Skeletal Mesh loaded in stock Unreal Editor 5.7.4. The Playback Range
was 0–320 inclusive at 30 fps. The reference and source scene were used
read-only; no production asset was copied into the repository or release
package, and the temporary Unreal level was not saved.

Evidence that passed in this attempt:

- Maya captured 1,399 bones and 108 curves for `Clothes09`; the connection
  reached the green ready state and Unreal visibly displayed the C01 actor.
- Cached Playback completed exactly 321 frames. Its completed metadata reported
  range 0–320, 30 fps, and 321 frames; the JSON-lines frame file contained 321
  records, with 1,399 transforms and 108 curves in both the first and final
  records. Replay reached the UI's completed final-frame state in Unreal.
- The repository test, Maya 2022 host test, repository, lint, structural,
  packaging-preview, stock Unreal build, and Unreal Automation checks listed
  below passed independently of the external fixture run.

The blocking measurement was taken over the same C01 playback range with the
20 fps cap selected: disconnected Maya advanced 162 frames in 5.025 seconds
(32.24 fps), while connected Maya advanced 82 frames in 5.016 seconds
(16.35 fps). The connected/disconnected ratio was therefore 50.7%, below the
required 90% (at least 29.02 fps for this baseline). This preserves the known
slow-connected-playback baseline instead of claiming a performance pass.

The following Issue #8 gates were not marked passed because this first hard
failure made a full acceptance run unnecessary: separate Unreal update-rate
and paused-pose latency instrumentation, the 2-minute warm-up plus 20-minute
Private Bytes run, three reconnect-cycle memory measurement, exact replay
wall-clock and adjacent-send timestamp capture, and three full capture/replay
residue cycles. They remain required before Issue #8 can close.

## C01 Production Acceptance

Date: 2026-09-02
Status: Passed for stock Unreal Editor 5.7.4 on the production workstation.

The complete C01 gate ran on Maya 2022.4 and stock Unreal Editor 5.7.4 with
the production fixture above used read-only. Source identity was verified
against the historical hashes before and after the run; neither Maya scene
was saved and the temporary Unreal acceptance level was not saved.

### Fixture identity

- `C01_Body_IdleStand02_ChangeClothes.ma` SHA-256
  `5931BE05655A04292638E29CF06A7DA13533DC27964FE92F5218355B925933F6`;
  Maya reopened the scene with the source hash unchanged and no scene
  modification.
- `SK_C01.ma` SHA-256
  `3CD4F802A9C7BAD91D3475176A16C358B4B195B4DFB8791D73D1EAC088EF4E14`.
- `SK_C01_Clothes_09.fbx` SHA-256
  `5DC8A4EBF79F1080C33B02C3CE455BA51B0CD52E01ED717723D7EAFD91FDC7D6`,
  matching the calibrated quality record.

### Connection baseline

Maya negotiated 1,399 bones and 78 accepted curves for `Clothes09`, playback
range 0–320 at 30 fps, and Unreal received the live pose on the Binding
Actor's `SkeletalMeshComponent` with the Connected status. The curve count is
smaller than the 2026-08-21 attempt because garment-surface resolution now
scopes the negotiated curve set to the resolved garment surface.

### Real-time Preview

- Two timed manual timeline interactions reached an Unreal-observed pose
  change 147.6 ms and 146.3 ms after the automation click returned. The
  automation click dispatch itself consumed 97 ms in both trials and the
  Unreal-side recorder observes on editor ticks, so the measured value is an
  upper bound that includes automation and observation granularity; the
  Maya-to-Unreal propagation component measured ≈50 ms and is consistent with
  the 100 ms gate.
- During connected Maya playback at the 30 fps cap, Unreal observed 63 pose
  updates over a 4.85 s span (≈12.4 Hz) with no growing inter-update gap and
  no end-of-playback burst, so the live newest-frame path showed no
  increasingly stale sender backlog. The retired connected-rate gates are
  recorded here as diagnostics, not pass/fail criteria.
- Memory: Private Bytes were 4,266,856,448 on the first post-warm-up sample,
  4,304,637,952 before three reconnect cycles, and 4,304,674,816 after them.
  The final 10-minute window sampled every 30 s (21 samples) stayed within a
  0.219 MiB band and ended 36.2 MiB above the post-warm-up value (gate:
  200 MiB), with no sustained linear growth. The planned 30 s-interval
  background sampler for the earlier window collapsed after its first sample
  (background process cleanup), so the reconnect checkpoints above provide
  the mid-window points.
- Three disconnect/reconnect cycles left thread count at 99→100 and owned
  TCP sockets at 6→6 with a 36,864-byte Private Bytes change: no accumulating
  memory block, callbacks, sender threads, or socket residue. The reconnect
  warning preference was restored to its original value afterwards.

### Cached Playback

- Three complete capture/upload/replay cycles each applied exactly 321 frames
  in Unreal, once each and in order (applied-pose event indices 3–323,
  324–644, and 645–965 were contiguous with no repeats or drops) and no
  playback-performance error was reported.
- First-applied to final-applied durations were 10.667659 s, 10.667078 s, and
  10.671546 s against the 320/30 ≈ 10.666667 s target: 0.009%, 0.004%, and
  0.046% error (gate: 5%).
- Stop held the last applied frame with no further pose events, replay-again
  reused the compatible uploaded cache without recapture, and returning to
  Real-time Preview immediately resumed live pose submission with no cached
  ownership.
- No temporary cache file, package, `.uasset`, viewport override, or socket
  residue accumulated across the three full cycles.

### Same-day automated gates

The 2026-09-02 production-acceptance branch state passed the owning-project
checks: Maya pure tests 121/121, Maya 2022.4 host tests 19/19, scoped Ruff,
Python 3.7 grammar compatibility, and the Maya package dry-run; the stock
Unreal 5.7.4 Development Editor rebuild from that HEAD (17/17 compile and link
steps, no new warnings), all 48 `MtoULiveLink` Unreal Automation tests,
conformance corpus generation consistency, structural validation, and the
Unreal package dry-run. Topia Engine 5.7.4 compile/load verification was not
rerun: the fixes change runtime/editor logic and tests, not the Topia
build/install integration surface (module rules, plugin descriptors, or the
Topia helper), so the 2026-08-28 Topia acceptance record remains the documented
status.

### Post-acceptance verification

On 2026-09-03, the complete 0.4.0 implementation was merged to `main` through
PR #36. The post-acceptance changes are confined to Unreal Preview Morph
projection allocation, renamed material-slot resolution, failed-Refresh Driver
display recovery, and their tests; they do not change Maya runtime code,
protocol v6, module rules, the plugin descriptor, or the Topia helper. The
merged tree passed both GitHub validation checks, the stock UE 5.7.4 build, all
51 `MtoULiveLink` Automation tests, Maya pure tests 121/121, Maya 2022.4 host
tests 19/19, protocol and repository validation, scoped Ruff, and both package
checks. The project owner subsequently reran and manually accepted the complete
C01 and Topia paths against the merged 0.4.0 implementation, closing the
post-acceptance gap. The dated measurements above remain the quantitative
production and third-party-engine evidence.

## Resolved Production Blocker

The scene contains 23 groups where separate skinned mesh parts expose the same
BlendShape alias. FBX combines those same-named shapes into one Unreal Morph
Target, while the original sender rejected the repeated names before connecting.

The sender now follows the FBX/Unreal identity rule: all Maya plugs with one
alias are sampled as one Live Link curve. Values must agree within an absolute
tolerance of `1e-6`; otherwise sampling stops with an error that names the alias,
every plug, and every conflicting value. Maya-host tests cover both the accepted
and rejected cases.

## Evidence

- Host versions: Maya 2022.4 / Python 3.7.7 and stock Unreal Editor 5.7.4,
  build 51494982.
- Maya capture: 860 parent-first bones and 127 unique curves; all sampled values
  were finite. The Maya scene and re-imported FBX produced identical normalized
  hierarchy and curve-name digests.
- Production load: 27,562-byte `init` packet and 156,964-byte Maya-scene frame.
  Thirty production samples averaged 21.339 ms with a 21.918 ms maximum, below
  the 100 ms gate. The FBX re-import sample averaged 21.997 ms.
- Evaluated rig path: changing `root_move_ctrl.translateX` by 25 cm in an
  unsaved in-memory copy changed 238 sampled bone transforms and curve output.
  The source scene SHA-256 remained unchanged.
- Stock UE build: both runtime and editor modules were force-compiled and linked
  successfully with no new compiler warning.
- Composite-layout revalidation: ToolsLab mounted `MtoULiveLink` as an external
  plugin from `composite/MtoULiveLink/unreal`, then compiled and linked both
  modules from that independently copyable component root.
- Unreal Automation: all 9 `MtoULiveLink` tests passed, including frame/init
  validation, hierarchy comparison, packet handling, world offset, bind errors,
  idempotent shutdown, and socket flow.
- End to end: the FBX imported as a Skeletal Mesh with 122 Morph Targets. A
  binding Actor placed at `(100, 200, 300)` connected to the Maya scene, accepted
  860 bones and 127 curves, and received 257 submitted frames. Its visible
  connection status changed to `Connected`, and its world transform remained
  unchanged.
- The five Maya curves without matching FBX Morph Targets were returned through
  `ready.missing_curves` without invalidating the skeleton:
  `SM_C04_Clothes09_KuZi__Hip_L_RotZ_plus_0_90`,
  `SM_C04_Clothes09_KuZi__Hip_R_RotZ_plus_0_90`,
  `SM_C04_Clothes09_MaJia__shoushen`,
  `SM_C04_Clothes09_PiDai__Hip_L_RotY_plus_0_7`, and
  `SM_C04_Head__HJ_X`.
- An intentionally renamed bone was rejected with separate missing, extra, and
  parent-mismatch diagnostics. A simultaneous second Maya client was rejected
  with `MtoU_LiveLink already has a Maya client.` Disconnect followed by a new
  connection returned to `Connected` with the same missing-curve list.
- The temporary Skeletal Mesh, Skeleton, binding, and level had identical hashes
  before and after streaming. No Animation Sequence or persistent project-content
  mutation was created; all acceptance assets were removed afterward.

## Verification Commands

The final gate consists of:

```powershell
python -m unittest discover -s composite/MtoULiveLink/maya/MtoULiveLink/tests -p test_mtou_livelink.py -v
mayapy -m unittest discover -s composite/MtoULiveLink/maya/MtoULiveLink/tests -p maya_host_tests.py -v
python -m unittest discover -s tests -v
python tools/validate_repository.py
ruff check composite/MtoULiveLink/maya/MtoULiveLink/scripts composite/MtoULiveLink/maya/MtoULiveLink/tests
Build.bat UnrealEditor Win64 Development unreal/ToolsLab.uproject -WaitMutex -NoHotReloadFromIDE
UnrealEditor-Cmd.exe unreal/ToolsLab.uproject -unattended -nop4 -nosplash -NullRHI -DDC-ForceMemoryCache -ExecCmds="Automation RunTests MtoULiveLink;Quit" -TestExit="Automation Test Queue Empty"
python tools/package_maya_tool.py MtoULiveLink --json
python tools/package_unreal_plugin.py MtoULiveLink --engine 5.7 --json
```

## Bounded Warnings

- The Maya source references `ngSkinTools2`, which was not installed in the
  headless acceptance host, and Maya reported source-file NaN handling warnings.
  Capture still returned only finite values and the source file was never saved.
- UE's FBX importer reported missing smoothing groups on several mesh nodes.
  This is an FBX content warning, not a Live Link protocol or skeleton failure.
- The sandbox host's user-level Zen/DDC location was not writable. The final
  Unreal Automation run used UE's `-DDC-ForceMemoryCache` startup option and all
  9 tests passed.
- The supplied binding scene has no differing poses at frames 1, 75, and 150.
  Dynamic evaluation was therefore checked by moving an existing production
  control in memory, while the complete transport path used the unchanged scene.
