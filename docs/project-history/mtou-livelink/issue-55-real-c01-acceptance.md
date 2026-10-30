# Issue #55: real C01 Maya-to-Unreal acceptance

Status: accepted on 2026-09-24 at `1ef0c1c1e509045afe1c4ca3accd005dac4906f9`, with the owner's report of no visual issues. Independent review confirms the source-mapping blockers are resolved. Real C01 host checks passed on 2026-09-23 and 2026-09-24; Model/Refresh and real C01 Cached Playback visuals were not repeated after the mapping changes. The final conclusion is recorded in [Issue #55](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/55).

## Final independent acceptance

An isolated copy of the accepted commit compiled both modules with stock UE
5.7.4. The unmodified `MtoULiveLink` Automation suite passed all 78 tests
(9 with existing diagnostics), with zero failures or unrun tests. The five
retained independent review reproductions passed, including the previously
failing rename-versus-rename capture-order case and the public socket ambiguity
refusal. An additional independent test enumerated 5,125 small exact/numeric/hash
name combinations and compared acceptance and source projection with exhaustive
assignment enumeration; all agreed. Existing socket tests verified live and
Cached Playback projection in both capture orders.

Running the temporary review tests together with the product suite produced one
`CacheBackpressure` Ready failure. That test passed in a fresh process, and the
complete original suite passed after removing the temporary review tests. The
mixed run is not counted as a clean suite pass; no product change was needed.
The latest C01 raw report was inspected and confirms Animation/reconnect
Success with zero errors and 122 existing asset-version warnings. This review
used NullRHI and retains the earlier rendered evidence and owner visual feedback.

## Hosts and assets

- Maya 2024 opened the rigged C01 scene `111_MH_Backups.0002.ma`, rooted at `|Group|root`.
- Stock UE 5.7.4 ran the acceptance in the owner's `Backups.uproject` with Primary `SK_C01_CombineBody_Clothes_12`, enabled `SK_C01_Head` and `SK_C01_Hair_01`, and the existing `DA_C01_MtoUBinding`. The fixture applied this combination to a transient Binding duplicate and spawned the actor in an unsaved `/Temp` world.
- The Model Preview Static Mesh came from the low garment group in `C01_Clothes12_37.ma`. It was imported without materials or textures into a dedicated temporary Content directory, then removed. The project's original MtoULiveLink plugin was restored after testing; original Content assets were unchanged.
- Rendered automation used Buffer Visualization **Base Color** and disabled Fab on its command line to avoid UE WebBrowser's automation assertion.

## Results

| Gate | Direct `Backups.uproject` evidence |
| --- | --- |
| Animation and composition | Maya published a 1,854-node scene snapshot; the C01 character negotiated 872 required bones. Baseline, 18-degree head rotation, and 22-degree left shoulder rotation passed source and displayed-component pose checks. **Base Color** screenshots show the complete body, head, hair, arms, hands, and feet. Maximum displayed component position error was `4.42e-05 cm`. |
| Model and Refresh | The fixture selected the four distinct Clothes_12 garment slots on its transient Binding. Both Refresh calls resolved 20,186 garment triangles with full Preview coverage and built a usable Generated Preview. The low-confidence weight-transfer ratio was `0.1709`, within the Ready limit of `0.2700`. Three Model poses, ended-subject cleanup, and a fresh Animation connection passed. The geometry-only Preview garment appears gray in screenshots. |
| Cached Playback | The production Maya controller captured 150 frames at 30 fps. UE applied and replayed all 150 at `29.9 fps`, then returned to live preview. Every stage checked 872 bones; maximum Primary/part position error was about `1.1e-13 cm`. A Head-only Morph value of `0.55` matched on its owning display component in live, cached, replayed, and returned-live stages. |
| Host checks | Rendered `CharacterAcceptance` with Animation and Model: Success, 0 errors. Cross-host `MayaCharacterPartsHost` with cache: Success, 0 errors. Both reported 122 pre-existing empty-engine-version asset warnings. A NullRHI Animation/reconnect run also passed. Maya pure tests: 140/140; Maya 2024 host tests: 23/23. |

The test harness holds each pose until its screenshot is written and retains driven Morph values across pose changes. Fixture overrides apply only to a transient Binding duplicate.

The FBX import commandlet exited with a UE Slate assertion after saving the temporary Static Mesh. The rendered editor run loaded that mesh and completed the Model and Refresh checks.

## Source mapping

Independent reviews of subset negotiation found three capture-order defects:
two Maya sources could compete for one required bone, two same-parent
same-name sources could be split between an exact target and an unclaimed
import-suffix target, and two sources that both needed an importer rename could
connect or refuse depending on sibling order. The review reproductions and
original socket responses are in [Issue #55](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/55).

The current negotiator resolves each mapped parent scope as one relation. All
exact-name Maya sources reserve their targets first; same-parent, same-name
Maya siblings fail with `SKELETON_MISMATCH` and a `Mapping ambiguities` entry
naming the required target, parent and both published paths even if a suffixed
target is free. The remaining rename candidates are matched against the scope's
required targets, and the connection is accepted only when exactly one
assignment of sources to targets covers them: a contested target, several
possible assignments, and a source whose feasible targets the relation does not
settle all reject with the same category and diagnostics. Different mapped
parents can still share short names, and an unrelated exported branch stays
unused. Accepted live and cached frames use the same frozen source-index
projection.

| Gate | Result on the corrected implementation |
| --- | --- |
| `Negotiation.RequiredBoneSources` | Before the first correction, six added assertions failed. After it, numeric and hash suffix collisions reject, the exact `joint1` reserves its target in both Maya sibling orders, and only `joint -> joint2` is remapped. |
| `Negotiation.RequiredBoneRenameOrder` | The rename chain `joint`/`joint1` -> `joint2`/`joint11` connects with the same mapping in both sibling orders, three duplicated short names keep their only valid mapping in all six permutations, and two indistinguishable sources competing for two targets reject in both orders with one ambiguity entry per contested target. |
| `Workflow.CharacterPartSourceMapping` | Before the first correction, the new two-part socket assertion failed. After it, the handshake rejects indistinguishable sources; exact-source live and Cached Playback poses remain correct. |
| `Workflow.CharacterPartRenameProjection` | Both capture orders hand back `ready` with `Cloth -> Cloth2` and `Cloth1 -> Cloth11`; live and Cached Playback frames drive the matching part bones, and a reversed-order reconnect repeats the same mapping. |
| Stock UE 5.7.4 Automation | Full `MtoULiveLink` suite: 78 tests, all Success; 9 report expected diagnostics. |
| Repository gates | `tools/validate_repository.py` passed; 22 repository tests passed (1 PowerShell-dependent skip). |
| Real C01 Animation/reconnect | Maya 2024 published 1,854 nodes; UE negotiated 872 required bones and accepted baseline, 18° head, 22° shoulder, and reconnect. Maximum world-position error was `3.914e-06 cm`, maximum displayed-component error `3.765e-05 cm`, and maximum normalized axis error `5.378e-07`. The only importer remap remains `joints_grp/spine_04 -> spine_04_5d859dce24654c43b1b653def8d6278f`. The run succeeded with 0 errors and 122 existing empty-engine-version asset warnings. |

The 2026-09-23 C01 re-verification used a transient Binding duplicate with
Primary Clothes_12 plus Head and Hair in a disposable world; the owner's
project plugin was restored to its original source and DLL hashes afterward.
The 2026-09-24 checks instead compiled a standalone plugin copy in a temporary
UE project with the owner's Content directory linked for loading only, and the
rename-resolution re-run compiled the plugin in the owner's project with the
plugin source and DLLs backed up and restored by checksum. No original Content
asset was replaced or saved. Maya still warns that ngSkinTools2 is unavailable
while loading the original scene; the captured rig animation and skeleton
comparison completed. The fixture has no Preview Static Mesh, so these runs
cover Animation and reconnect, not Model/Refresh. The real C01 Cached
Playback/Model evidence remains from the earlier host run; the current source
mapping's live/cache behavior is covered by the socket regression.
