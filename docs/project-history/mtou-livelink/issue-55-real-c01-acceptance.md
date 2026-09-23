# Issue #55: real C01 Maya-to-Unreal acceptance

Status: real C01 host acceptance completed on 2026-09-23. [Issue #55](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/55) remains open for owner review.

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
