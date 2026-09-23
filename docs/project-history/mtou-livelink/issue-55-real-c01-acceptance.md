# Issue #55: real C01 Maya-to-Unreal acceptance

Status: real C01 host acceptance completed on 2026-09-23. [Issue #55](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/55) remains open for owner review.

## Asset and isolation boundary

- Maya 2024 opened the rigged C01 scene `111_MH_Backups.0002.ma`, rooted at `|Group|root`.
- Stock UE 5.7.4 loaded the Backups C01 assets: Primary `SK_C01_CombineBody_Clothes_12`, enabled `SK_C01_Head` and `SK_C01_Hair_01`, and the existing `DA_C01_MtoUBinding`. The acceptance fixture applies this combination to a transient Binding duplicate.
- A disposable UE project reads Backups Content and spawns the actor in an unsaved `/Temp` world. The Model workflow uses `SM_C01_Clothes_12.fbx` imported into the disposable project's own content-only plugin. Acceptance logs show no package-save operation on Backups assets.
- The isolated Static Mesh has geometry only. Model screenshots therefore show a gray garment; garment material appearance was outside this acceptance. Private source assets and raw screenshots remain outside the repository.

## Results

| Gate | Real C01 evidence |
| --- | --- |
| Connection and Animation | The Maya peer published a complete 1,854-node scene snapshot. One UE character subject negotiated 872 required bones for the Primary, Head, and Hair. Baseline, 18-degree head rotation, and 22-degree left shoulder rotation passed source-to-published and displayed-component pose checks. Maximum displayed world-position error was `3.82e-05 cm`. |
| Rendered composition | Buffer Visualization **Base Color** screenshots show the complete body, head, hair, arms, hands, and feet in the baseline, head, and shoulder poses. |
| Model and Refresh | Both Refresh calls built a usable transient Generated Preview from the Clothes_12 Static Mesh. Model poses displayed the garment with Head and Hair and passed the same 872-bone pose checks. Refresh replaced the generated description, ended the old subject, and a fresh Animation connection passed. Preview weight transfer's low-confidence ratio was `0.2716`, inside the configured Warning band. |
| Cached Playback | The production Maya controller captured 12 frames at 30 fps; UE applied 12 frames in `0.375 s` (`29.4 fps`) and replayed 12 in `0.369 s` (`29.8 fps`). It stopped, cleared the cache, and returned to live preview. All stages compared 872 bones with maximum Primary/part position error about `1.1e-13 cm`. A Head-only Morph value of `0.55` matched the published value on its owning display component in live, cached, replayed, and returned-live stages. |
| Checks | Rendered `CharacterAcceptance`: Success, 0 errors. Cross-host `MayaCharacterPartsHost`: Success, 0 errors. Each UE run reported 122 pre-existing asset warnings about empty saved engine versions. Maya pure tests: 140/140; Maya 2024 host tests: 23/23. |

The test harness holds each pose until its screenshot is written and retains driven Morph values across pose changes. Fixture overrides apply only to a transient Binding duplicate.

The disposable FBX import commandlet exited with a UE Slate assertion. A separate editor run loaded the saved Static Mesh and completed the Model and Refresh checks.

Raw local reports and images are retained under ignored `Saved/issue55/`. The rendered acceptance artifacts include Base Color Animation/Model screenshots and an Automation report; the cache artifacts include `parts-host-report.json`. These machine-local files are evidence for this run.
