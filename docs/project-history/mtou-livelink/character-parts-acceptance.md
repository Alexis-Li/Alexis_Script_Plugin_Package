# Character-part composition acceptance (Issue #45)

Status: implemented and verified on the local `main` branch (not pushed), with
the real split-asset visual acceptance recorded below as not performed. This
record is the durable technical evidence behind the Issue #45 comment.

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

## Not performed

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
