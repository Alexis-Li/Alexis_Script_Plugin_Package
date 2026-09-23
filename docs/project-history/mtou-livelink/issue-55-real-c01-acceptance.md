# Issue #55: real C01 Maya-to-Unreal acceptance

Status: real C01 host checks passed on 2026-09-23, and the owner reported no visual issues. The review's blocking source-mapping defect is fixed and re-verified on the same assets; [Issue #55](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/55) stays open until final owner acceptance.

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

## Source-mapping resolution

An independent source review reproduced a regression in
`MtoUConnectionNegotiator.cpp` introduced by `c6f5580`: subset negotiation
committed the first matching Maya source for a required bone, removed that
target from later candidate searches, and then skipped every competing source
as an unused branch, so which Maya bone drove a target depended on capture
order. It could return `ready` for two same-parent sources of one required bone
and publish whichever came first.

The fix resolves every required target against all Maya sources in its mapped
parent's scope. An exact name outranks an import rename and retires that rename
claim by re-running the mapping with the claim forbidden, so the winner never
depends on capture order. Two equally valid sources are reported as a
`Mapping ambiguities` entry naming the target, its parent, and both complete
Maya paths instead of publishing one of them. Duplicate short names under their
own mapped parents and sources matching no required bone stay unused branches.

Post-fix evidence, all on stock UE 5.7.4:

| Gate | Result |
| --- | --- |
| Reviewer's own reproductions `MtoULiveLink.Review55.SourceAmbiguity`, `Review55.ExactSourcePriority`, `Review55.AmbiguousSocket` | 3/3 Success against the fixed source; the same three failed against `279f1ac` in the reviewer's isolated host |
| `Automation RunTests MtoULiveLink` (NullRHI, full suite) | 76 Success (9 with expected diagnostics), 0 failed, 0 not run |
| New regressions | `Negotiation.RequiredBoneSources` covers same-parent normalized duplicates, an exact name against a suffix candidate in both capture orders, rename-versus-rename, and unrelated duplicates on their own mapped parents. `Workflow.CharacterPartSourceMapping` drives the public socket flow: `ready` with an empty remap list, the exact source moving the displayed part in live and Cached Playback, and `SKELETON_MISMATCH` with `Mapping ambiguities` when a second source matches one required bone. |
| Real C01 Animation | `MtoULiveLink.Editor.Preview.CharacterAcceptance` on the owner's `Backups.uproject`: Success, 0 errors. Maya 2024 opened `111_MH_Backups.0002.ma`, published 1,854 nodes, and the character negotiated and drove **872 required bones** across baseline, 18-degree head, and 22-degree left-shoulder poses plus an explicit reconnect. Maximum world position error `3.9e-06 cm`, maximum displayed component error `3.77e-05 cm`, maximum axis error `5.4e-07`. The handshake reported exactly one bone remap, the existing `joints_grp/spine_04 -> spine_04_5d859dce24654c43b1b653def8d6278f`, so the new uniqueness rule neither rejected nor re-renamed the real character. |
| Maya | Pure tests 140/140 and Maya 2024 host tests 23/23 (unchanged Maya code; re-run green) |
| Repository | `tools/validate_repository.py` and the 22 repository tests pass |

The C01 re-verification used a transient Binding duplicate with Primary
`SK_C01_CombineBody_Clothes_12` plus Head and Hair parts in a disposable
`/Engine/Maps/Entry` world; the owner's project plugin was restored to its
original source and DLL hashes afterwards (verified by checksum). Because the
fixture has no Preview Static Mesh, the run took the Animation-and-reconnect
path: the Model/Refresh and Cached Playback phases recorded in the table above
were not repeated for this fix and remain the evidence for those paths, while
the live/cache projection of the resolved source is covered by
`Workflow.CharacterPartSourceMapping`.
