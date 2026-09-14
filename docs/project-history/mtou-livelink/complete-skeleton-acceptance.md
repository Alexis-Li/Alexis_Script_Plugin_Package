# Complete-skeleton connection and preview acceptance (Issue #44)

Date: 2026-09-14. Follow-up to the review of `d03fc31` and the reported
`BIND_POSE_INVALID` at bone 587.

## Correction

The saved Maya sample publishes 1472 nodes, including both pupil joints with
non-zero scales near `1e-12`. UE's absolute determinant tolerance rejected these
valid transforms. Checked finite inversion now replaces that test and bypasses
the engine's tiny-axis identity fallback. Explicit output scale extraction also
preserves tiny axes that `FTransform(Matrix)` would otherwise collapse. Zero
scale, failed inverses and non-finite results still fail closed. No skeleton
matching rule, protocol version, source bone name or rig asset was changed.

The existing socket test's zero/one/two-actor fixture now collects prior pending
worlds before it starts. Running CacheSessionReconnect immediately before
SocketFlow previously left two global actor entries and returned
`MULTIPLE_BINDING_ACTORS`; this was test isolation, not a reason to relax the
production one-actor contract. The first handshake reply is retained in the
Automation log for future diagnosis.

## Independent Maya transform oracle

`maya_host_tests.py` reconstructs matrices directly from the transmitted T/R/S,
reverses the UE X/Z/Y basis, and multiplies them by published parent indices.
The oracle does not call `_sample_matrix` or `convert_transform`. It compares
all matrix elements against `cmds.xform(..., worldSpace=True, matrix=True)` for
multiple non-unit structural parents, then changes the pose and independently
checks the frozen bind worlds and current animated worlds. Units are centimeters;
the maximum permitted element difference is `1e-6`. Maya 2022.4: 22/22 host
checks passed; pure Maya tests: 138/138 passed. Maya 2024 also passed 22/22 in the
preceding run. This is a TRS-chain guarantee, not arbitrary local shear support.

## Actual asset and host run

The user-designated Backups project contains:

- Binding `/Game/Character/C01_111/DA_C01_MtoUBinding`.
- Driver `/Game/Animation/C01/Rigging/MH/SK_C01_Clothes_09_All`.
- Preview `/Game/Character/C01_111/Clothes_09/Mesh/SM_C01_Clothes_09`.

The first actual project run used its rebuilt plugin and Maya 2022.4 mayapy.
After the user opened their interactive Maya/UE sessions, subsequent tests used
an isolated temporary project mounting the very same Backups Content directory
and the repository plugin. No asset was copied, reimported or saved. This avoids
replacing loaded DLLs or disturbing unsaved user scenes. Both used stock UE
5.7.4; the character test renders an editor viewport, not NullRHI.

`MtoULiveLink.Editor.Preview.CharacterAcceptance` starts a disposable Maya process
with `maya_character_peer.py`. The peer loads the supplied MB without saving,
uses the production CharacterScene/controller/sender, and responds to explicit
commands. Only warning/error dialog presentation is suppressed. Pose edits are
fixture data, applied to the disposable scene; no character names are embedded
in the test implementation. Maya opens the newer sample with `ignoreVersion`;
missing optional embeddedRL4/ngSkinTools plug-ins do not constitute validation
of DNA/RigLogic or those plug-ins.

The sequence is:

1. Animation handshake, baseline, body (`FKShoulder_L`), head (`FKHead_M`), and
   accessory (`XZ_R_joint1_Con4`) poses.
2. Public Refresh builds the actual Preview and terminates the Maya session.
   The old Live Link subject is absent and the listener is ready before reconnect.
3. Explicit Model handshake, then the same four poses with BlendShape
   transmission enabled and the Generated Preview selected.
4. Another public Refresh replaces the generated mesh and ends Model streaming.
5. Explicit Animation reconnect publishes the baseline again with a fresh
   description; the peer closes and exits cleanly.

Every phase checks the entire 1472-bone publication. The real handshake also
asserts exactly these five remaps:

| Source parent/name | UE published name |
| --- | --- |
| joints_grp/spine_04 | spine_041 |
| spine_05/clavicle_l | clavicle_l1 |
| spine_05/clavicle_r | clavicle_r1 |
| clavicle_l/upperarm_l | upperarm_l1 |
| clavicle_r/upperarm_r | upperarm_r1 |

Expected component motion is rebuilt from the source bind/current matrices and
the actual Driver reference skeleton. Live Link output and the displayed
SkeletalMeshComponent are checked separately, including normalized world axes.
Position tolerance is `0.1 cm`, normalized-axis distance tolerance is `1e-3`.
Observed errors are near floating-point roundoff (positions below `3e-13 cm`,
normalized axes below `1e-12`). The separate Maya oracle above verifies the
Maya-world-to-wire boundary.

The full-body screenshots show body and head motion in both workflows.
Accessory axes/parent links are drawn from the actual component for branches
not weighted by the current garment; this does not claim that an unweighted
branch should deform that garment. The captured unlit view checks pose and
hierarchy, not material/texture or photorealistic shading quality.

## Preview verdict

The supplied Preview passed geometric validation: 43 Driver garment regions,
27438/180496 Driver triangles, matched Preview coverage `1.0000`. Refresh built
a generated mesh with the full skeleton and transferred 27 matching garment
Morph Targets; 186 without matching Preview surface were skipped. The existing
calibrated Warning is retained: 4620/14389 vertices (`0.3211`) have low-confidence
weight transfer. This lies within the warning band `(0.2700, 0.7500]`, so the
result is usable but does not claim every transferred weight is artist-approved.
Normalized surface-distance max was `0.00449`. Missing Morph names are reported
separately and do not imply a bone mismatch.

## Reproduction and evidence

Run in a fresh disposable editor launched on `/Engine/Maps/Entry`; the opt-in
harness replaces only that engine startup map with an unsaved temporary world.
It refuses an ordinary project map. Do not invoke it in a working user session.
The fixture JSON requires `scene`, `root`, `binding`, and a nonempty `poses` list
with `name` and `edits` (Maya attribute-to-additive-delta mappings). Optional
`expected_remaps` asserts the exact remap set; `debug_bones` displays auxiliary
bone axes. Paths and pose choices belong to the local fixture, not production.

```powershell
& "$EngineRoot/Engine/Build/BatchFiles/Build.bat" UnrealEditor Win64 Development $Project -WaitMutex -NoHotReloadFromIDE
& "$EngineRoot/Engine/Binaries/Win64/UnrealEditor.exe" $Project /Engine/Maps/Entry -unattended -nop4 -nosplash -DDC-ForceMemoryCache '-ExecCmds=Automation RunTests MtoULiveLink.Editor.Preview.CharacterAcceptance' '-TestExit=Automation Test Queue Empty' "-MtoUCharacterFixture=$Fixture" "-MtoUMayapy=$Mayapy" "-MtoUCharacterPeer=$RepoRoot/composite/MtoULiveLink/maya/MtoULiveLink/tests/maya_character_peer.py" "-MtoUEvidence=$Evidence" "-abslog=$Evidence/editor.log"
```

Inspect the Automation result, per-pose JSON and screenshots, refresh diagnostics,
and `final-peer.json`; process exit code alone is insufficient. Local evidence
is in `issue44-acceptance/` under the user-provided test-data directory, outside
the checkout, with the final rendered run in
`verified/` and `verified-rendered.log`. Initial failing fixture/order runs are
retained for diagnosis, not counted as passes. Original MB/FBX/DNA hashes are
recorded locally and unchanged. Original assets and screenshots are not uploaded
or included in the commit.

## Final checks

- UE Runtime/Editor builds succeeded in Backups and the isolated acceptance host.
- Focused UE regressions: 19/19 passed, including pose/protocol conformance,
  strict hierarchy compatibility, Workflow.Negotiation, RefreshEndsSession,
  RefreshBenchmark, and the CacheSessionReconnect → SocketFlow sequence.
  Benchmark execution was a functional check, not a new performance claim.
- Rendered real-character acceptance: passed with all three handshakes, two
  successful Refresh operations, nine evaluated poses and clean Maya exit.
- Maya 2022 host: 22/22; pure Maya: 138/138; repository tests: 21/21.
- Repository validator, Maya/UE package dry-runs and final whitespace check passed.
- UTF-8 JSON is explicit in the new host harness, including localized disconnect
  diagnostics; local fixtures and evidence remain outside tracked source.
