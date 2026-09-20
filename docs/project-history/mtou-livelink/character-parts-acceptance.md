# Character-part composition acceptance (Issue #45)

Status: local implementation and C02 asset-level automated acceptance passed;
visual acceptance remains owner-operated. The C02 follow-up below supersedes
the original missing-assets and Maya 2024 availability blockers. Changes have
not been pushed. This record supports the Issue #45 comments.

Date: 2026-09-20.

## Scope

One Binding now describes a character delivered as several Skeletal Meshes. The
existing serialized `SkeletalMesh` field keeps its name and meaning as the
Primary Driver; a labelled Additional Parts list adds each part's display name,
Skeletal Mesh, enabled flag, and internal stable identity. The design rationale
and the composition-versus-Preview-revision classification live in
[the architecture record](architecture.md#character-composition-and-additional-parts)
and [ADR 0002](../../../composite/MtoULiveLink/docs/adr/0002-end-streaming-when-preview-inputs-change.md).

## Verification performed

| Check | Result |
| --- | --- |
| Stock UE 5.7.4 `Build.bat UnrealEditor Win64 Development` on the deployment project | Succeeded; no compiler or linker warnings in the affected modules |
| `Automation RunTests MtoULiveLink` (NullRHI) | 65/65 Success (61 existing + 4 new); 0 failed, 0 not run |
| New `MtoULiveLink.Workflow.CharacterParts` | Ready reply carries the composed library (`target_morph_count` 2, `accepted_morph_count` 2), a Head-only name is absent from `missing_in_unreal`, one subject publishes the composed curve set, the part's displayed bones match the Primary on the streamed pose, a shared Morph name reaches both meshes, and a captured frame plus the return to live preview drive the part after `cache_clear` |
| New `MtoULiveLink.Negotiation.CharacterPartCompatibility` | Five refused configurations return `INVALID_BINDING`/`SKELETON_MISMATCH` with the part, bone, skeleton, or reference-pose reason in the details; a disabled part connects; the repaired part connects again |
| New `MtoULiveLink.Actor.CharacterPartLifecycle` | Rename and reorder keep the same component and the same session; enable/disable/replace/remove rebuild exactly the affected components, destroy unclaimed ones, and end the session on a composition change; a part source rebuild ends the session while keeping its component; duplicate and reload leave exactly one component per enabled part |
| New `MtoULiveLink.Editor.Preview.CharacterParts` | Garment build is unaffected by a part (GarmentFlare present, part-only and non-garment Morphs absent), Model preview displays Generated garment + Primary + part, disabling the part and reimporting a part both keep the ready Generated Preview, and a Primary reimport still invalidates the revision |
| Maya pure `unittest discover`, owning tests directory | 140/140 passed |
| Maya 2022.4 `mayapy .../tests/maya_host_tests.py` | 23/23 passed, including the new separately named Head part publishing its own BlendShape and the existing shared-alias aggregation and conflict rejection |
| `python tools/validate_repository.py` | Passed |
| `python -m unittest discover -s tests` (repository tooling) | Passed |
| `python tools/package_unreal_plugin.py MtoULiveLink --engine 5.7 --json` | Dry-run passed |
| `git diff --check` | Passed |

The composed morph library was additionally checked against the existing Model
workflow coverage: the accepted set is the intersection of the Maya manifest
with the union of the Generated Preview, the Primary Driver, and the enabled
parts, while a zero-name manifest stays a valid bone-driven session and a
non-empty manifest with an empty intersection still blocks.

## Original acceptance limitations (historical)

The real split-asset visual acceptance (Maya 2022.4 against stock UE 5.7.4 with
the production Body + Head assets, facial-controller visual inspection, and
real-time plus cached-playback performance measurement) was not executed:

- The test project owns the full-character Driver
  (`SK_C01_Clothes_09_All`) and its garment Preview, but no separately imported
  Head Skeletal Mesh that shares that Skeleton, so a composed Binding cannot be
  assembled there. Producing one is an asset-pipeline step outside this
  repository, and the cross-host acceptance peer also requires a fixture whose
  poses, root, and Binding describe that composed character.
- The supplied split Maya scene `SK_C02_05MH.ma` requires Maya 2024 plugins
  (`mayaUsdPlugin`, `ngSkinTools2`, MetaHuman for Maya) that the supported Maya
  2022.4 host does not load, so it cannot serve as a Maya 2022.4 acceptance
  input. Under Maya 2024 it does open (489 mesh shapes, 1,960 joints, and
  per-mesh BlendShape libraries on the body, outfit, and hair meshes), which
  reconfirms the multi-mesh character shape this Issue targets, but Maya 2024 is
  outside the product's supported host set and would not be the specified
  2022.4 gate.

The owner's visual confirmation and the performance measurement therefore remain
the final gate for this Issue; the automated evidence above covers the
composition rules, negotiation, display wiring, Morph routing, and lifecycle,
but not rendered facial-preview quality.


## C02 follow-up: irrelevant reference-pose rejection

Date: 2026-09-20. The owner supplied imported C02 resources and moved the
production workflow to Maya 2024, reporting the Maya tool usable. Visual
acceptance is explicitly retained by the owner.

The production Binding uses SK_C02_CombineBody_Clothes_05, SK_C02_Head and
SK_C02_Hair_01. Before the fix, loading this Binding reproduced the reported
rejection and created zero additional display components. Each mesh carries
1,502 reference bones; the Head skinning palettes and their ancestor chains
cover 489 bones, and Hair covers 97. The reported necklace branch (0.434
translation difference), and a hair branch rejected on the Head, do not deform
that part. Comparing every retained reference bone incorrectly rejected the
whole composition.

Reference-pose validation now covers the union of section skinning palettes
from all LODs and their ancestors. Complete bone-name/parent compatibility
remains mandatory; missing usable skinning evidence keeps the conservative
whole-skeleton check. Tolerances are unchanged. Parent matching also compares
names rather than allowing equal indices to conceal different parent bones.

Verification:

- Stock UE 5.7.4 affected modules compiled and were deployed to the owner's
  Backups project's existing plugin. C02 assets were read only, not saved.
- The external asset test reproduces failure before the fix and passes after
  it: the composition resolves, and both enabled parts have registered,
  visible Skeletal Mesh Components. This is not a rendered-image assertion.
- Full `Automation RunTests MtoULiveLink`, with
  `-MtoUCharacterPartsBinding=/Game/Character/C02_112/DA_C02_MtoUBinding`:
  66 Success, zero failures/not-run (57 clean and 9 with warnings).
- Compatibility regression verifies unrelated unweighted branch differences
  are allowed, while palette influences from the last LOD and unweighted
  ancestors of weighted bones remain checked. The final focused compatibility
  rerun also verifies equal parent indices cannot conceal different names.
- Maya 2024 `mayapy -m unittest discover` using the owning test directory:
  `test_mtou_livelink.py` 140/140; `maya_host_tests.py` 23/23.
- Unreal package dry-run and `git diff --check` passed.

Engine asset-load warnings include C02 Head Switch/Mobile minimum LOD overrides
outside the available LOD range and a texture with an empty engine version.
These did not block the Win64 test; this fix does not edit those source assets.

Rendered facial-controller quality, actual C02 cross-host live/cached playback
and its performance measurements are not established by the asset visibility
test. The existing synthetic workflow/cache suite passed. Owner visual review
remains pending; this follow-up does not claim full C02 end-to-end acceptance.
Topia and Maya 2022.4 were not rerun in this follow-up; their prior evidence is
retained above. No protocol or Maya runtime implementation changed.


## Saved-level PostLoad crash follow-up

The owner's next saved-level load exposed a gap in the previous asset test:
spawning an Actor and assigning its Binding never exercises UObject's real
PostLoad routing. Opening the existing C02 `/Game/Untitled` map reproduced
`Cannot call UnrealScript ... BlueprintInitializeAnimation ... while PostLoading`
on the hair asset's Post Process animation blueprint, before the editor could
finish loading.

The Actor's PostLoad now restores only readiness/display intent. Component
reconciliation, mesh assignment, animation initialization and source observation
resume through PostRegisterAllComponents, with routing guards on the display
and composition entry points. Additional components disable Post Process
*evaluation* before registration/mesh assignment. UE 5.7 can still instantiate
and initialize an asset Post Process graph with this flag set; therefore moving
initialization outside PostLoad is essential, and merely moving the disable
call earlier would not fix the lifecycle contract.

The regression extends PostProcessIsolation with an actual routing flag and an
initialization counter, checking no animation initializes during PostLoad and
that registration restores the part afterwards. A separate optional
LoadedCharacterParts test inspects actors deserialized from an actual saved
map, rather than spawning substitutes. It verifies both enabled parts have
registered visible meshes, native Live Link instances, and disabled Post Process
evaluation. The existing asset's Post Process class remains unchanged.

The same saved map that crashed before the fix loads successfully after it
under stock UE 5.7.4 with NullRHI, and the loaded-actor checks pass. This proves
the reported loading path, not rendered visual quality. No level, Binding or
mesh asset is saved by the verification. The repaired binary is deployed to
the owner's existing Backups project plugin. Source edits remain uncommitted.

Final verification: affected modules compile successfully; the full
`MtoULiveLink` suite reports 67/67 Success with no failures, and the optional
LoadedCharacterParts check also passes in its separate real `/Game/Untitled`
load run (`-MtoULoadedCharacterMap=/Game/Untitled`). Package dry-run and diff
whitespace checks pass. The default suite does not request the external loaded
map case; the separate run supplies that evidence. Previous Maya 2024 results
remain applicable because this follow-up changes only Unreal lifecycle code.
