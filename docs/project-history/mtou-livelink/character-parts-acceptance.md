# Character-part composition acceptance (Issue #45)

Status: accepted and [Issue #45 closed on 2026-09-21](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/45#issuecomment-5755713454).
The owner reported no problem in the visual review. The final review accepted
the C02 cross-host and performance evidence without repeating the measurements.
This is scoped acceptance of character composition, not a complete 0.5.0 release.

## Accepted implementation

The reviewed implementation comprises `907adf0`, `09b096e`, `d3a2cee`, and
`fa6220c`. One Binding keeps its serialized `SkeletalMesh` as the Primary Driver
and adds independently enabled character parts. Compatibility, one-subject pose
and Morph routing, and Primary-only garment preparation are described in the
[architecture record](architecture.md#character-composition-and-additional-parts)
and [ADR 0002](../../../composite/MtoULiveLink/docs/adr/0002-end-streaming-when-preview-inputs-change.md).

The acceptance fixes establish three durable boundaries:

- Reference-pose checks use every LOD's skinning palettes and ancestor chains.
  This historical #45 gate retained strict names/parents, shared Skeleton and
  rejected extra bones, with a full-pose fallback for missing evidence. Issue
  #55 reopens that rule: current composition uses positive influences and
  ancestors, supports required part-only branches and refuses unreadable skin
  data. The #45 measurements below do not certify the expanded #55 behavior.
- Saved-level PostLoad restores intent only. Component registration, mesh
  assignment, and animation initialization resume afterwards. Disabling Post
  Process evaluation alone cannot prevent Blueprint initialization during load.
- Binding Undo/Redo reaches actors through an unnamed property event. Actors
  compare the restored inputs with the applied Primary, Preview Static Mesh,
  and Garment Slot Override. A Preview input change ends the session and
  invalidates readiness; a composition change ends the session but preserves
  valid garment readiness; names and order change neither.

## Verification evidence

| Boundary | Accepted evidence |
| --- | --- |
| Build and owning Automation | Stock UE 5.7.4 affected modules compiled. Final `full_v4/index.json`: 62 Success + 9 Success with warnings, 0 failed, 0 not run (71 total). Earlier failed runs are superseded. |
| Composition, Morphs, and cache | `Workflow.CharacterParts`, `Negotiation.CharacterPartCompatibility`, `Actor.CharacterPartLifecycle`, and `Editor.Preview.CharacterParts` cover compatibility, shared and part-only Morphs, component lifecycle, and Primary-only garment preparation. |
| Real C02 composition | Primary Body/Clothes plus Head and Hair resolve and create registered visible components. The real Binding and saved-map checks were run separately with external asset parameters; default optional-test Success does not replace those runs. |
| Saved-level loading | The original PostLoad assertion was reproduced on the existing saved map before the fix. Afterward the map loads, with native Live Link instances and disabled Post Process evaluation; `PostProcessIsolation` also exercises the actual routing flag. |
| Part transactions | Real editor transactions cover add/remove/disable, name/order, and Primary restoration. `CharacterPartTransactions` detects the original failure on an actor that construction reruns cannot repair; the loaded-map transaction run also passed. |
| Override transactions | `PreviewInputRestore` establishes Ready Preview and an active negotiated session after an Override edit, then tests Undo and Redo. It asserts connection closure, session termination, Preview release, Dirty/Refresh state, and removal of the stale display. Removing the Override comparison fails the four relevant assertions; the repaired version passes. |
| Stale-frame isolation | The Override test proves session termination; the shared termination boundary and existing `SessionTerminationBoundary` and cache regressions supply old-frame/cache isolation coverage. It is not a separate cache-injection test. |
| Maya | Maya 2024 pure tests 140/140 and host tests 23/23, including independent Head BlendShape discovery and shared-alias aggregation/conflict rejection. Maya 2022.4's earlier 140/140 + 23/23 evidence is retained; final changes are Unreal-only. |
| Repository/package checks | Repository validation, repository tooling tests, Unreal package dry-run, and diff whitespace checks passed in the implementation runs. The final review independently repeated the package dry-run and diff check. |

Local raw reports include `mtou_reports/full_v4`, `restore_probe`, `restore1`,
`loaded_fixed`, `parts_final2`, and `parts_base3`; the paired measurement outputs
are `mtou_parts_host/parts-host-report.json` and
`mtou_parts_host_baseline/parts-host-report.json` under the temporary directory.
They are retained review evidence, not runtime dependencies. The linked Issue
comments retain the review conclusions if temporary reports are later removed.

## Real C02 cross-host measurements

Hardware and host: Windows 11, Maya 2024 (`mayapy` 20240200) opening
`SK_C02_05MH.ma` from the character root `|Group|root`, and stock Unreal Editor
5.7.4 with NullRHI on the owner's Backups project with
`/Game/Character/C02_112/DA_C02_MtoUBinding` (Primary
`SK_C02_CombineBody_Clothes_05`, enabled parts `SK_C02_Head` and
`SK_C02_Hair_01`). Maya publishes 1,502 nodes. Both runs use the same scene and
the same capture range (frames 1-12 at 30 fps) and differ only in whether the
two parts are enabled.

Method: `MtoULiveLink.Source.MayaCharacterPartsHost` drives a real Maya peer
through the production controller. Real-time cost is the Game Thread work for
one frame - source update, Live Link publication, and the animation evaluation
of every displayed mesh - sampled 20 times. Cached cost is the local replay
measured where Unreal applies the captured frames. Rendered image quality and
frame rate with a viewport are not part of this measurement.

| Measurement | Primary only | Primary + 2 parts |
| --- | --- | --- |
| Real-time Game Thread frame cost (mean / median / p95) | 0.398 / 0.389 / 0.440 ms | 1.165 / 1.121 / 1.314 ms |
| Derived real-time frame rate | 2,510 fps | 859 fps |
| Cached playback of the same 12-frame capture | 12 frames in 0.368 s (29.9 fps) | 12 frames in 0.380 s (28.9 fps) |
| Replay-again of the retained cache | 12 frames in 0.389 s (28.3 fps) | 12 frames in 0.376 s (29.3 fps) |
| Accepted Morph library | 32 names | 200 names |

Composed-character checks in every stage - real-time, cached playback, cached
replay, cached stop, and the return to live preview - report a maximum
displaced-bone error between the displayed parts and what the stream dictates
for each part's own hierarchy: about `1.30e-13 cm` in the real-time stage and
`1.13e-13 cm` in the cached and returned-live stages. The part-only BlendShape
`Braise_Eblink_INL`, absent from the Primary Driver, was evaluated as `0.55` in
the real-time stage and `0.65` in the cached and returned-live stages, matching
the published value in each case. Cached Playback completed the
full capture, upload, local replay, stop, replay-again, clear and
return-to-live sequence with the peer reporting an intact 12-frame cache.

The C02 Primary Driver and its parts share no Morph name, so the real-asset run
exercises the part-only case; the shared-name case is covered by the synthetic
`MtoULiveLink.Workflow.CharacterParts` test, where one name reaches both meshes
with the same value.

## Acceptance limits and delivery state

- Owner visual feedback supplies the rendered facial/seam/deformation review.
  NullRHI measurements are short-run update/animation and local-cache evidence,
  not a viewport FPS or long-duration performance guarantee. No new P2 run was
  required because the final transaction fix does not alter that path.
- Real C02 uses Maya 2024. The initial missing-Head and Maya-version blockers
  were resolved by the supplied C02 assets and accepted workflow change.
  Topia and Maya 2022.4 were not rerun for the final Unreal transaction fixes;
  their earlier support evidence remains the boundary of the compatibility claim.
- C02 Head Switch/Mobile minimum-LOD overrides and a texture without an engine
  version produced asset-load warnings. They did not block Win64 acceptance;
  source assets were not saved or corrected by these checks.
- As checked on 2026-09-21, local `main` contains `fa6220c`; remote `main` is
  `94a4a06`, so `d3a2cee` and `fa6220c` are local commits awaiting push. Issue
  closure does not imply a PR, merge, formal release, or fresh deployment.
  This documentation closeout did not independently verify the currently loaded
  editor binary or redeploy the plugin.
