# Issue #53: selected-level reference geometry in Maya

Date: 2026-10-08. Current review result: **changes required** on local implementation
`29a1bb5cd515df1cbca6a60e7352ccbc9facd707` (fix
`489b1c57bdd1f6814c020639c6927efea6394444`, followed by dead-member removal
`0b304aaa834c0f9550f93f403eacbd4adfdb9e66` and a documentation correction).
The implementation remains local; this review did not push it or integrate the
prototype into MtoU. The [current issue handoff](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/53#issuecomment-5888512464)
owns the next action. Its superseded body and the development delivery are preserved
verbatim in [the 2026-10-08 source archive](issue-53-comment-archive-20261008.json);
earlier revisions remain in [the original archive](issue-53-comment-archive.json).

Independent re-verification ran the Maya 2024 pure suite (110 passing tests), host
suite (274 checks, zero failures), three imports of the development run's UE 5.7.4
handoffs, and additional in-process fault/ownership probes. It did not rebuild or
rerun UE, re-export the production level, or open the C01 production rig. UE source
is unchanged between the fix and reviewed commits. The development report's UE
10/10 result remains development evidence, not an independently repeated UE run.

The official FBX route, explicit loaded-level scope, media policy and isolated
prototype remain viable. R-003, R-004 and R-005 are closed within the conditions
below. R-001 and R-002 remain unresolved; R-006 is a new cleanup-error defect.
Issue #53 cannot receive whole-issue acceptance while those blockers remain.
Large production terrains, complex Nanite assets, process crashes and production
scale are unverified extensions, not silently added prototype acceptance gates.
All three prototypes (#52/#53/#54) must first have runnable evidence and explicit
boundaries; #47 then owns one combined MtoU integration decision.

## Outstanding acceptance

IDs retain the findings first assigned in this record. The current independent
review closes only the stated scope, not every failure mode in the same subsystem.

| ID / type / status | Current evidence | Completion criterion / owner |
| --- | --- | --- |
| R-001 / confirmed defect / unresolved, P1 | Maintained tests now pass namespace/group rename failures and a returned missing-path result. An exception thrown from `_rehome_paths()` instead returns failure with `swapped: true`, `rolled_back: false`; the old UUID remains only in `_Retiring`. A subsequent `--dry-run` deletes that UUID as a retired leftover despite the previous final check never completing. | #53 implementation: protect the complete takeover/final-check sequence with rollback, distinguish a committed container from one merely renamed/marked, and preserve the old UUID through final-check exceptions and the next retry/dry-run. |
| R-002 / confirmed defect / unresolved, P1 | Unowned final/staging-name collision tests pass. Recovery still deletes an entire stale `_Incoming` namespace when any top-level group has the expected mark. A second, unmarked top-level production transform in that namespace is deleted too; the new dry-run returns success. | #53 implementation: apply ownership/conflict checks before recovery deletion as well as normal replacement. A mixed namespace must preserve the foreign UUID and refuse or report selective cleanup; one marked group must not authorize deleting the namespace's other content. |
| R-003 / confirmed defect / reverified closed | The maintained partial-import probe now takes cleanup responsibility before import: exit 1, discarded partial nodes, no staging namespace, old UUID preserved. Independent host suite passed. | Closed for partial-import failure when cleanup succeeds; cleanup failure is separately tracked as R-006. |
| R-004 / acceptance gap / reverified closed, fixture only | Independent Maya dry-run of the new Landscape FBX/manifest passes: 7938 triangles, position/size/offset errors 0 cm, surface-centroid error 6.4311e-13 cm, no problems/warnings. | Closed for one flat, single-component, LOD0 terrain without visibility holes. Multiple components, other LODs and visibility layers remain outside this evidence. |
| R-005 / acceptance gap / reverified closed, descriptor scope | Independent imports both pass. Streaming-on manifest reports 76 descriptors, 9 spawned, 3 unspawned authored actors and 64 unspawned HLOD proxies; Maya emits `SCOPE_LOADED_ONLY`. Streaming-off handoff reports confirmed descriptor coverage, 4 matched objects, no coverage warning. | Closed for the fixture's descriptor-level loaded-content statement, not general runtime cell streaming or arbitrary nested partition layouts. No automatic loading was added. |
| R-006 / confirmed defect / unresolved, P2 | After a simulated partial FBX import, a simulated `namespace(removeNamespace=..., deleteNamespaceContent=True)` refusal escapes `SceneRefImporter.run()` from `_discard_staging()`. The old UUID survives, but staging nodes remain and the API returns neither its normal report nor exit code. | #53 implementation: contain and report cleanup exceptions, retain the original failure, identify residual owned resources and a retry path; repeated cleanup must preserve production and old-reference UUIDs. |

Probe entry: repository-adjacent `.tmp/issue53-reacceptance-20261008/reverify.py`
with the repository root as its sole argument. `reverify.json` and `reverify.log`
hold raw results; `host.json`, `host.log` and `pure.log` hold reruns. These scratch
files are supplemental local evidence, not a published archive. The table above,
the review comment and the source archive preserve the observed outcomes and
reproduction conditions. Maintained checks live in
`prototypes/scene-reference/maya/MtoUSceneRefPrototype/tests/maya_host_scene_ref_tests.py`.

## What was delivered

| Area | Result |
| --- | --- |
| Official capability review | Unreal 5.7 exports a level through `ULevelExporterFBX`, `ULevelExporterOBJ`, STL or T3D; only the FBX path keeps hierarchy, instancing and actor labels, and only the FBX path can be kept free of image data. All findings are source-referenced in the prototype's [official capability review](../../../composite/MtoULiveLink/prototypes/scene-reference/official-capabilities.md). |
| Prototype | An editor-only Unreal module (scope resolution, static-geometry filter, Partition descriptor inventory, fixture, export, produced-file inspection, manifest) and a Maya importer/verifier (media check, owned staged update with rollback, two worlds, comparison, report), with the contract in [transfer.md](../../../composite/MtoULiveLink/prototypes/scene-reference/transfer.md). |
| Sample | A generated fixture of seven levels under `/Game/MtoUSceneRefFixture`: off-origin, rotated, non-uniformly scaled and instanced static meshes, a two-component Blueprint actor, a mixed Blueprint actor (mesh, light, camera, child actor, skeletal mesh), a Nanite-enabled mesh, a texture-driven material, one held and one unheld sublevel, a landscape, a level instance and a World Partition level. |
| Cross-host session | One real handoff of twelve objects, measured end to end on both hosts, with the axis convention and the node frame factor fitted from the imported geometry rather than assumed. |
| Real level | The user's own character level (`/Game/Untitled` in the local test project) exported and verified on both hosts, in both worlds. |

## Acceptance criteria

| Criterion | Outcome |
| --- | --- |
| Check the official level-export capability and deliver a minimal UE → file → Maya sample; do not build a general mesh serializer | Done. The geometry comes from `ULevelExporterFBX` through `UAssetExportTask`; the prototype only resolves the scope, filters the components, drives the editor selection the exporter reads, inspects what it wrote, and serializes a manifest of transforms. The OBJ level exporter was measured as the alternative and rejected (below). |
| Cover plain static meshes and repeated instances, including off-origin, rotated and non-uniformly scaled samples; compare Maya world position, orientation and size against Unreal, and verify axes and units | Done for all four sample kinds. Twelve objects across the persistent level, its sublevel and the mixed Blueprint arrive in the container, all twelve matched (nine by node name, three instanced children by world position). Position error `0.0 cm`, bounding-box size error `4.5e-13 cm`, pivot-offset error `1.1e-13 cm`, surface centroid error `9.3e-13 cm`, and the worst node axis angle `1.2e-6` degrees over the two objects whose identification size pins their axes. The centroid is the mirror check; the orientation check compares each node matrix against `frame . L_ue . map`, the relation the file really uses (below). |
| Do not generate, copy, package or embed image files/data; permit material assignments and external texture paths, and provide gray display | Verified by output inspection. Image files in the handoff directory and embedded FBX media refuse normal import (`IMAGE_DATA_PRESENT`); `--allow-image-data` is a diagnostic bypass. Material/texture records are allowed, path resolution is reported separately, and gray viewport display preserves material assignments by default. This does not promise that Maya will never resolve an existing external image path. |
| State the level/sublevel selection semantics and report support/omissions for each object type | Partially accepted, re-measured 2026-10-08. Explicit loaded-level scope, mixed-Blueprint filtering and Level Instance refusal keep their evidence. Nanite still has only a flagged cube sample. Landscape now passes Maya verification for the fixture (R-004); a partitioned scope states its own completeness from a read-only descriptor inventory and the Maya side warns when it is not confirmed (R-005). R-004 and R-005 independently pass within these fixture-level conditions; see the current review table. |
| Reference geometry enters an identifiable container; repeated runs do not overwrite or mix with production objects; large inputs report scale/time/memory without a performance promise | Not accepted as a whole: normal replacement, the maintained swap-failure cases and partial-import cleanup pass, but R-001/R-002/R-006 remain confirmed blockers. Geometry/time/memory reporting exists; no production-scale performance claim is made. |
| Preserve world space; if a reference origin offset is proposed, give one shared conversion contract for camera, character and props; do not depend on the camera prototype to verify the geometry | Done, with the checked contract: the engine handoff's point map is `ue (x, y, z) -> maya (x, z, y)`, and the product's own animation route sends Maya coordinates to Unreal with the same `(x, z, y)` map (`MtoULiveLink.py`'s `convert_transform`, self-inverse), so the reference imported in the handoff's own world already agrees with the character and props the artist animates. The camera route's `(y, z, -x)` is the divergent map, and the reference import now delivers that world explicitly (`--target-world camera`, one conversion `camera_map . engine_map^-1`, determinant `+1`, 90 degrees about the up axis, applied to every root, reported with its matrix, angle and root count). Both worlds were verified on the same non-origin sample and on the real level. No origin offset is proposed. |
| Deliver the sample, measurements, support list, reusable API and a minimal product proposal | Runnable prototype and measured samples exist; failure-recovery acceptance remains blocked by R-001, R-002 and R-006. Product candidates below belong to #47. |

## Evidence

The table below preserves development-run observations; the independent 2026-10-08 results and provenance are recorded above. The final 2026-09-29
review reran Maya pure tests (103/103) and host checks (251, zero failures), then
ran the failure probes and Landscape dry-run described above. It inspected the
existing UE reports but did not rebuild UE, rerun UE Automation or re-export the
real level. A passing suite total does not cover the newly confirmed failures.
`RealMayaPeer` can return success without launching Maya when its mayapy argument
is absent; actual peer reports are required for a cross-host claim.

| Check | Command shape | Result |
| --- | --- | --- |
| Unreal suite | `UnrealEditor-Cmd <ToolsLab.uproject> -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests MtoUSceneRefPrototype" -TestExit="Automation Test Queue Empty"` with `-MtoUSceneRefMayapy=`, `-MtoUSceneRefPeer=` and `-MtoUEvidence=` | 10 of 10 tests pass on 2026-10-08: `ScopeSemantics`, `ExportMainScope`, `TextureDetector`, `TextureProbe`, `MixedBlueprintFilter`, `NaniteSourceMesh`, `LandscapeScope`, `LevelInstanceRefusal`, `WorldPartitionScope`, `RealMayaPeer` |
| Maya pure tests | `python -m unittest discover` and the same under `mayapy` over the Maya prototype tests | 110 tests pass on 2026-10-08 under CPython and under mayapy 3.10.8 (103 before this round; the added class covers the scope coverage statement) |
| Maya host checks | `mayapy tests/maya_host_scene_ref_tests.py --result <json>` | 274 checks, 0 failures, `ok: true` on 2026-10-08 (251 before this round; the added phase injects the update failures and the collisions) |
| Cross-host handoff | The `RealMayaPeer` test exports the fixture scope and runs the Maya importer/verifier over the produced file | `ok: true`, no problems, 12/12 objects compared (9 by name, 3 by transform), position `0.0 cm`, size `4.5e-13 cm`, offset `1.1e-13 cm`, centroid `9.3e-13 cm`, orientation `1.2e-6 deg` over 2 pinning objects, `file_texture_nodes: 0`, engine map `maya_x=ue_x, maya_y=ue_z, maya_z=ue_y`, staged swap |
| Camera world | The same handoff with `--target-world camera` | `ok: true`, no problems, the winner is the camera map `maya_x=ue_y, maya_y=ue_z, maya_z=-ue_x`, `objects_converted: 8`, `conversion_determinant: 1.0`, `conversion_rotation_deg: 90.0`, and `world.engine_check` names the engine map with a `0.0 cm` error before the conversion |
| Media rule | A texture-driven material in scope; a copied image file next to a clean handoff; an injected `Content:` payload in a real `Video:` record | Texture record present: imports, keeps the file's shading group, creates the `file` node, warns when the recorded path resolves locally. Image file beside the handoff: exit 1 `IMAGE_DATA_PRESENT` / `IMAGE_FILES_IN_HANDOFF`, nothing imported; with `--allow-image-data` it imports and warns. Embedded payload: exit 1 `IMAGE_DATA_PRESENT` / `EMBEDDED_MEDIA_PRESENT`; with the flag, `embedded_media_records: 1` and the assigned material is still the file's own |
| Update recovery | Repeat run, wrong-value manifest after a good import, and `--dry-run` | Repeat: `update.mode: staged_swap`, `swapped: true`, the replaced group gone by UUID, identical node list. Failure: exit 1, `staged_swap_discarded`, `discarded_node_count: 18`, staging namespace gone, the previous reference unchanged (node identity, matrices, node count) and production untouched. `--dry-run`: exit 0, `update.mode: dry_run`, nothing swapped |
| Update failure injection (2026-10-08) | `mayapy tests/maya_host_scene_ref_tests.py` phase `ownership`: namespace rename failure, group rename failure, forced missing path after the swap, partial FBX import, unowned container namespace, unowned staging namespace, stale marked staging namespace, takeover interrupted after the retire | Namespace rename and group rename failures: exit 1 `STAGED_SWAP_FAILED`, `rolled_back: true`, `rollback.previous_reference_restored: true`, the previous group's UUID alive, no staging or retiring namespace left. Missing path: `rolled_back: true` with `rollback_reason` naming the unresolved path, previous reference back. Partial import: exit 1 `FBX_IMPORT_FAILED`, `discarded: true`, the partial node listed in `discarded_nodes` and gone, previous reference intact. Unowned container namespace: exit 1 `CONTAINER_NOT_OWNED`, the foreign node alive, nothing imported. Unowned staging namespace: exit 0, the run used `MtoU_UE_SceneRef_Incoming_1`, the foreign node untouched. Stale marked staging namespace: swept, `stale_staging_removed: true`, the unmarked one kept. Interrupted takeover: the retired reference is renamed back and reported before the next run proceeds |
| Object kinds | `NaniteSourceMesh`, `LandscapeScope`, `LevelInstanceRefusal`, `WorldPartitionScope` automation tests | Nanite: render data exported (48 triangles, 54 vertices) and matched in Maya. Landscape (2026-10-08): the record measures the geometry the engine's branch writes — `7938` triangles, `4096` vertices, a `8064 x 8064 x 0` cm surface where the actor's own bounds are `512` cm thick — and the hand-run Maya import of that handoff reports `ok: true`, size error `0.0 cm`, centroid error `6.4e-13 cm`, position `0.0 cm`, offset `0.0 cm`. Level instance: refused by the engine, reported, no node. World Partition (2026-10-08): `76` descriptors, `9` spawned / `3` unspawned authored / `64` HLOD with streaming on and `12` spawned / `0` unspawned authored / `64` HLOD with it disabled; the as-authored handoff imports in Maya with a `SCOPE_LOADED_ONLY` warning and the loaded one without it, one object and four objects matched respectively |
| Real level | The user's test project (`/Game/Untitled`, a character scene) through a temporary project descriptor that adds the prototype plugin directory | 15 actors in scope, 3 exported objects (sky sphere, template floor, one generated box), 12 actors reported skipped with reasons, one BSP brush skipped, `unsupported: []`; handoff 2.16 MB, one file, no image files, zero texture records; scope `0.0002 s`, export `0.053 s`, inspect `0.071 s`, used physical memory `2184.96 MB`; the Maya peer verified 3/3 objects in both worlds (`0.0 cm` position, `2.4e-4 cm` size on an 8000 cm floor, `1.2e-11 cm` centroid, camera world `objects_converted: 3`) with no problem and no warning. No object of that level pins its axes, so the orientation check reported itself unavailable for all three |
| Repository gates | `python tools/validate_repository.py`, `python -m unittest discover -s tests` | Pass (2026-10-08) |
| Fixture repeatability | Five independent `MtoUSceneRef.BuildFixture` runs across editor sessions | Identical numbers and notes every time (Nanite 48/54, main scope 12 objects / 4476 triangles, landscape 848247 bytes, partitioned 4 objects / 4112 triangles) |

## The world contract, measured

| Export option | Unreal world `(x, y, z)` in cm arrives in Maya as |
| --- | --- |
| `bForceFrontXAxis = false` (default) | `(x, z, y)` |
| `bForceFrontXAxis = true` | `(-y, z, x)` |

The camera verification documented `(y, z, -x)` for its own route. The product's
animation route (`MtoULiveLink.py` `convert_transform`) sends Maya coordinates to
Unreal with `(x, z, y)`, the same self-inverse map as the default handoff, so the
reference in the handoff's own world already matches the artist's character and
props; the camera route is the divergent one.

A point map is not the whole matrix. The file writes each node in its own local
frame, so the node matrix Maya reads back is `frame . L_ue . map` with
`frame = ((1,0,0),(0,-1,0),(0,0,1))` for the default option. Measured on
2026-09-29 over the fixture handoff: every node matrix matches that composition
to `1.3e-15`, while the earlier expectation `L_ue . map^T` is off by up to `7.7`
and therefore never described the file. The prototype reports the factor
(`transform_check.node_frame_factor`) and runs the orientation comparison only
when the manifest declares an export option whose factor was measured.

## Support list

| Object kind | Result |
| --- | --- |
| `AStaticMeshActor` | Exported, world transform preserved (measured) |
| Instanced static mesh component | One child node per instance, named by index, instance transform preserved (measured, three instances) |
| Blueprint actor components | One node per surviving static mesh component; the actor keeps the actor node when it has one, and gets child nodes named after the component when it has several (measured) |
| Mixed Blueprint actor | Light, camera, child actor and skeletal mesh components are suppressed for the export, reported in `filter.suppressed_components`, and absent from the file (measured: `camera_records: 0`, `light_records: 0`, no child mesh node) |
| Actor with a component that is not its root | Component transform merged into the node (measured) |
| Child actor | Its own actors are reported as skipped and never selected; the parent's child-actor component is suppressed, so the recursion the engine would otherwise do is stopped (measured) |
| Sublevel held by the loaded world | In scope when requested (measured) |
| Requested level the world does not hold | Reported, contributes nothing (measured) |
| Unrequested sublevel | Reported as excluded, contributes nothing (measured) |
| Level instance | Refused by the engine with its own message, reported as unsupported, no node (measured) |
| Landscape | Geometry is written, and the record now measures the source that branch writes, so Maya verification passes for the fixture (2026-10-08: size error `0.0 cm`, centroid error `6.4e-13 cm`, `7938` triangles). Not verified beyond one flat, single-component terrain at export LOD 0 with no visibility layer; R-004 independently passed for this fixture. |
| Nanite mesh | LOD 0 render data exported; the Nanite source mesh is not (`bExportSourceMesh` off), and the node matches in Maya (measured) |
| World Partition | Only loaded content is traversed, and the scope now says so: a read-only descriptor inventory reports how many descriptors have spawned, names the unspawned authored actors and counts generated HLOD proxies apart; `scope.completeness` is `confirmed` only when every authored descriptor is spawned, and Maya warns with `SCOPE_LOADED_ONLY` otherwise (2026-10-08, independently reverified for this fixture). The inventory counts descriptors, not streamed cell geometry, and records at most 200 paths. Disabling streaming remains a comparison experiment, not a recommended product operation. |
| Lights, cameras, emitters, brushes, volumes, world settings | Reported as skipped non-geometry; a selected actor's non-mesh components are suppressed (measured) |
| Materials, textures as data | Material assignment and recorded texture paths are kept and reported; no image data is delivered (measured) |

## Known limits

- `MtoUSceneRef.BuildFixture` builds the fixture once per editor session; a
  second build inside one session is refused, because deleting fixture levels the
  session still holds as its world crashes the editor (a pre-existing pattern of
  the prototype's builder).
- The `MtoUSceneRef.Peer` console command cannot take a mayapy path containing
  spaces as a positional argument, because console command arguments are split on
  whitespace; the `-MtoUSceneRefMayapy=` switch and the `MTOU_SCENEREF_MAYAPY`
  environment variable are the supported ways to point it at Maya.
- Landscape verification covers one flat, single-component fixture terrain at
  export LOD 0 with no visibility layer. A landscape that carries a visibility
  layer is reported as such, because the engine's branch omits hidden quads and
  the measured triangle count can then be higher than the file's.
- The update findings were reproduced by in-process fault injection
  (`cmds.namespace`/`cmds.rename`/`cmds.file` replaced for one run), not by
  killing or crashing the host, so cross-process crash recovery and the recovery
  of a Maya session that died mid-swap remain unproven.
- The ownership check covers the container's name and the top-level transforms
  inside it; a foreign node deeper inside an owned container namespace is not
  classified, and the check itself is only as strong as the string attribute it
  reads.
- `container.kept_existing` is a statement about the node the run found, by UUID.
  After a rolled-back takeover the report also names the path it was put back to
  (`container.previous_reference_path_after_run`); a restore the host refuses is
  reported instead of raised, and leaves the reference in its retiring name.
- The World Partition completeness statement is derived from actor descriptors:
  it does not read streamed cell geometry, it caps the recorded path list at 200
  entries, and `confirmed` describes the descriptors being spawned, not what a
  running game would stream.
- The orientation comparison only runs for objects whose identification size has
  three distinct extents; the real level's three objects are all axis-ambiguous,
  so no orientation was checked there.
- Reference origin offsets are not implemented, and the worlds are not unified:
  the prototype delivers both and quantifies the difference.
- No installation package, no host certification, no product protocol or
  animation export change.

## Decisions that need revision

- The roadmap's shared world-space contract still has two measured conventions in
  the project. The reference now agrees with the animation route by default and
  can be moved into the camera route's world by one flag, so the remaining
  decision is which world the product should standardise on, and whether the
  camera route should adopt the other map.
- If a scene reference is adopted, the `bForceFrontXAxis` choice becomes a
  product decision with a documented map. The landscape scale gap this record
  reported is closed for the fixture by measuring the engine's own export source,
  but a production terrain, a visibility layer and another export LOD still need
  their own measurement before terrain numbers are trusted.

## Product candidates for #47

#52, #53 and #54 continue their own bounded verification and fixes. Once all
three have runnable prototypes, reproducible evidence, support boundaries and
remaining issues, #47 owns a combined integration plan. The options below do
not supersede this issue's current explicit-level scope or image-data policy,
and do not replace or block the current R-001/R-002/R-006 fixes.

1. Which world the product standardises on: the handoff and animation route's
   `(x, z, y)` (the current default, consistent with the character and props), or
   the camera route's `(y, z, -x)` through the verified conversion.
2. Which sublevel selection the product exposes: an explicit list, "everything
   the loaded world holds", or the current editor selection.
3. Whether a scope whose records carry image data should be blocked (the current
   behaviour) or warned about and imported with the gray display.

## Delivered files

- `composite/MtoULiveLink/prototypes/scene-reference/` — contract, capability
  review, Unreal module (with Automation tests), Maya importer/verifier (with
  pure tests and host checks).
- `unreal/ToolsLab.uproject` — the prototype plugin directory and its enable
  entry, so the documented host project builds it.
- `composite/MtoULiveLink/docs/preview-workflow-roadmap.md` — the verification
  outcome recorded against the scene-reference direction.
