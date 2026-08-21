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

This C01 fixture has not yet passed the complete stock-engine acceptance gate.
The C04 results and measurements below remain valid historical evidence for the
files actually tested on 2026-08-11; they must not be reported as C01 results.
The Cached Playback happy path and capture-safety guards are covered by
deterministic Maya-side capture/replay tests, including ordered delivery,
timeline restoration, cancellation cleanup, disk-space rejection, and exact
cache finalization. This is implementation evidence only; it does not replace
the pending stock-engine C01 run across all 321 display frames.

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
