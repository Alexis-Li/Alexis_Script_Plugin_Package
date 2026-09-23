# Issue #55: real C01 Maya-to-Unreal acceptance

Status: technical host acceptance completed on 2026-09-23; [Issue #55](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/55) remains open for the owner's review. This record completes the real-asset gate left after `c6f5580`, without changing the protocol or original assets.

## Asset and isolation boundary

- Maya 2024 opened the **rigged** `111_MH_Backups.0002.ma` scene from the owner's C01 Maya project, rooted at `|Group|root`. The separate `C01_Clothes12_37.ma` is a static model and was not used as the animation source.
- Stock UE 5.7.4 loaded the Backups C01 assets: Primary `SK_C01_CombineBody_Clothes_12`, enabled `SK_C01_Head` and `SK_C01_Hair_01`, and the existing `DA_C01_MtoUBinding`. The acceptance fixture applies this combination to a transient Binding duplicate.
- A disposable UE project reads Backups Content and spawns the actor in an unsaved `/Temp` world. `SM_C01_Clothes_12.fbx` was imported into the disposable project's own content-only plugin for the Model workflow. The original Backups assets were neither saved nor reimported; the acceptance logs contain no package-save operation on them. The isolated FBX import commandlet raised a UE Slate assertion **after** saving the temporary Static Mesh; the subsequent editor run loaded that mesh and completed both Refresh operations.
- The FBX was imported without materials or textures. The gray garment in Model screenshots is expected; the acceptance covers geometry, weight transfer, bone motion, and display composition, not garment material fidelity. Private source assets and raw screenshots are not checked in.

## Results

| Gate | Real C01 evidence |
| --- | --- |
| Connection and Animation | The Maya peer published a complete 1,854-node scene snapshot. One UE character subject negotiated 872 required bones for the Primary, Head, and Hair. Baseline, 18-degree head rotation, and 22-degree left shoulder rotation passed source-to-published and displayed-component pose checks. Maximum displayed world-position error was `3.82e-05 cm`. |
| Rendered composition | In Buffer Visualization **Base Color** mode, the baseline, head, and shoulder screenshots show the whole body, head, hair, arms, hands, and feet. The earlier Unlit screenshots with missing regions are superseded by the completed runs; a later A/B run with the original, unmodified Actor and the same assets also showed the complete character. They do not establish a product Bounds defect. Base Color was chosen for skin/material inspection; Unlit's white skin was a viewport-mode effect. |
| Model and Refresh | Both Refresh calls built a usable transient Generated Preview from the separately imported Clothes_12 Static Mesh. Model poses displayed the garment with the Head and Hair and passed the same 872-bone pose checks. Refresh replaced the generated description, ended the old subject, and a fresh Animation connection passed. Preview weight transfer reported `0.2716` low-confidence vertices, inside the configured Warning band; the result was usable, not a quality-pass claim beyond that warning. |
| Cached Playback | The production Maya controller captured 12 frames at 30 fps; UE applied 12 frames in `0.375 s` (`29.4 fps`) and replayed 12 in `0.369 s` (`29.8 fps`). It stopped, cleared the cache, and returned to live preview. All stages compared 872 bones with maximum Primary/part position error about `1.1e-13 cm`. A Head-only Morph value of `0.55` matched the published value on its owning display component in live, cached, replayed, and returned-live stages. |
| Checks | Rendered `CharacterAcceptance`: Success, 0 errors. Cross-host `MayaCharacterPartsHost`: Success, 0 errors. Each UE run reported 122 pre-existing asset warnings about empty saved engine versions. Maya pure tests: 140/140; Maya 2024 host tests: 23/23. |

The first cache-host run exposed a peer-fixture ordering error: changing pose reset a Morph previously driven by the test. The peer now keeps explicitly driven aliases through pose changes; the second run passed. Fixture overrides and screenshot sequencing are part of the test harness and do not alter a saved Binding.

Raw local reports and images are retained under ignored `Saved/issue55/run-c01-ab-original-bounds/` and `Saved/issue55/run-c01-cache2/`. The first directory contains the final Base Color Animation/Model screenshots and Automation report; the second contains the cache report and `parts-host-report.json`. These machine-local artifacts are evidence for this run, not installable project files.
