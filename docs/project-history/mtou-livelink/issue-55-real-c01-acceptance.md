# Issue #55: real C01 Maya-to-Unreal acceptance

Status: real C01 host checks passed on 2026-09-23, and the owner reported no visual issues. The two source-mapping blockers found in independent reviews are now fixed; synthetic socket and host regressions plus C01 Animation/reconnect passed on the updated implementation. Model/Refresh and real C01 Cached Playback visuals were not repeated after this mapping change. [Issue #55](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/55) stays open for final owner acceptance.

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

Independent reviews of subset negotiation found two capture-order defects:
two Maya sources could compete for one required bone, and two same-parent,
same-name sources could be split between an exact target and an unclaimed
import-suffix target. A rename source listed before an exact sibling could
also see two suffix candidates and reject a valid character. The review
reproductions and original socket responses are in [Issue #55](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/55).

The current negotiator reserves exact-name targets against all Maya sources
in each mapped parent scope before considering numeric or hash import renames.
Same-parent, same-name Maya siblings fail with `SKELETON_MISMATCH` and a
`Mapping ambiguities` entry naming the required target, parent and both
published paths even if a suffixed target is free. Different mapped parents
can still share short names; an unrelated exported branch stays unused.
Accepted live and cached frames use the same frozen source-index projection.

| Gate | Result on the corrected implementation |
| --- | --- |
| `Negotiation.RequiredBoneSources` | Before correction, six added assertions failed. After correction, numeric and hash suffix collisions reject, the exact `joint1` reserves its target in both Maya sibling orders, and only `joint -> joint2` is remapped. |
| `Workflow.CharacterPartSourceMapping` | Before correction, the new two-part socket assertion failed. After correction, the handshake rejects indistinguishable sources; exact-source live and Cached Playback poses remain correct. |
| Stock UE 5.7.4 Automation | Full `MtoULiveLink` suite: 76 tests, including the existing character and lifecycle regressions; 9 report expected diagnostics. |
| Repository gates | `tools/validate_repository.py` passed; 22 repository tests passed (1 PowerShell-dependent skip). |
| Real C01 Animation/reconnect | Maya 2024 published 1,854 nodes; UE negotiated 872 required bones and accepted baseline, 18° head, 22° shoulder, and reconnect. Maximum world-position error was `3.914e-06 cm` and maximum displayed-component error `3.766e-05 cm`. The only importer remap remains `joints_grp/spine_04 -> spine_04_5d859dce24654c43b1b653def8d6278f`. The test succeeded with 0 errors and 122 existing empty-engine-version asset warnings. |

The 2026-09-23 C01 re-verification used a transient Binding duplicate with
Primary Clothes_12 plus Head and Hair in a disposable world. The owner's
project plugin was restored to its original source and DLL hashes afterward.
The 2026-09-24 check instead compiled a standalone plugin copy in a temporary
UE project with the owner's Content directory linked for loading only; no
original plugin DLL or Content asset was replaced or saved. The temporary
project initially lacked the character's KawaiiPhysics dependency and logged
three unknown-structure errors. Adding an isolated copy of that plugin and
rebuilding yielded the successful host run above. Maya still warns that
ngSkinTools2 is unavailable while loading the original scene; the captured
rig animation and skeleton comparison completed. The fixture has no Preview
Static Mesh, so this run covered Animation and reconnect, not Model/Refresh.
The real C01 Cached Playback/Model evidence remains from the earlier host run;
the current source mapping's live/cache behavior is covered by the socket
regression.
