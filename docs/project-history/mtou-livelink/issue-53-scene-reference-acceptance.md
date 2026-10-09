# Issue #53: selected-level reference geometry in Maya

Date: 2026-10-09. Independent review-02 of implementation
`1a3cede888722d879ff462d818bda118d44468dd` confirmed two blocking recovery
defects, R-001 and R-002, both since fixed by the implementation and re-probed
with the review's own scripts. R-006 is independently reverified closed.
R-003, R-004 and R-005 retain their bounded approval. The inspected checkout was
`d79f309df8744a19d9be36aa96ec3d9dc6361107`. These commits are local, not pushed.
Issue #53 remains open pending an independent reverification of the two fixes,
and product integration remains with #47 after all three prototypes have runnable
evidence and explicit boundaries.

The [current handoff](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/53#issuecomment-5888512464)
owns the next action. [Development dev-02](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/53#issuecomment-6056055116)
is the reviewed delivery. Exact source reports before this review are preserved
in [the 2026-10-09 archive](issue-53-comment-archive-20261009.json); earlier versions
remain in the existing 2026-10-08 and original archives. The new archive is committed
locally; remote publication is pending. No source comments are deleted.

This review ran 110 pure tests and 291 Maya 2024 host checks (all passing),
repeated the prior review's fault probes, and imported the development run's
Landscape and two World Partition handoffs in fresh Maya scenes. All three
handoffs passed within their fixture scope. Two additional recovery probes below
reproduced data loss despite the passing maintained suite. UE code is unchanged;
this review did not rebuild UE, rerun its automation, re-export the production
level or open the C01 rig. The development UE 10/10 remains historical development
evidence, not this review's independently executed result.

The implementation has since answered both findings (dev-03, this checkout): the
recovery refuses rather than promotes, and the destination applies the ownership
rule. The review's own minimal probe now reports `R-001_reproduced` and
`R-002_reproduced` as false, and the two paths are maintained host checks (299
checks, 0 failures, Maya 2024 mayapy). The fix round is recorded under
"R-001/R-002 fix round" below; its acceptance awaits independent reverification.

## Outstanding acceptance

Finding IDs are retained because these are unresolved paths of the same recovery
and ownership requirements, not new requirements or production-scale extensions.

| ID / type / status | Current evidence | Completion criterion / owner |
| --- | --- | --- |
| R-001 / confirmed defect / fixed, P1, awaiting independent reverification | Review-02 baseline: when `_rehome_paths` raises and deletion of the destination container is refused, the old UUID remained in Retiring, a following dry-run called `_finish_interrupted_container`, wrote the completed ownership mark and returned success, and the next dry-run deleted the old UUID as `RETIRED_LEFTOVER_REMOVED`. Fixed: the half-swapped namespace is deleted only while it holds nothing foreign; a refused deletion, a refused rename back and a retired copy of a finished container that cannot be deleted all refuse the run with `RECOVERY_REFUSED` (exit 1); the completion mark is written only when no retiring copy exists and the namespace is unambiguously this tool's. The review's probe now reports `R-001_reproduced: false`; the three-step sequence (exception + refused rollback, retry with deletion refused, retry after the fault is gone) is a maintained host check and restores the old UUID under the container's name. | Retain the incomplete marker and the old UUID through consecutive retries; never promote a destination while a retiring reference remains unresolved. Criterion met on the fix baseline; independent reverification pending. Owner: #53 implementation delivered, independent reviewer to reverify. |
| R-002 / confirmed defect / fixed, P1, awaiting independent reverification | Review-02 baseline: a half-swapped destination containing a staging-marked group and an unmarked top-level ProductionObject was deleted wholesale while a valid Retiring reference existed; dry-run returned success, restored the old reference and the foreign UUID was gone. Fixed: the destination namespace is checked for foreign nodes before any deletion and a mixed one is refused with `CONTAINER_NOT_OWNED` (exit 1), so the foreign UUID, the staged group and the retired reference all survive; the probe now reports `R-002_reproduced: false`, and a maintained host check covers the same state. | Apply the same ownership/conflict rules to the destination as to Incoming and Retiring; preserve the foreign UUID and the old reference and cover retry/dry-run in maintained tests. Criterion met on the fix baseline; independent reverification pending. Owner: #53 implementation delivered, independent reviewer to reverify. |
| R-003 / confirmed defect / reverified closed | Partial import with successful cleanup preserves the old reference and removes staging; maintained suite rerun passes. | Closed for successful cleanup, not arbitrary host failure. |
| R-004 / acceptance gap / reverified closed, fixture only | Fresh Maya import of the existing UE handoff passes: 7938 triangles, zero position/size/offset errors, centroid error approximately 6.4e-13 cm. | One flat single-component LOD0 terrain without visibility holes. |
| R-005 / acceptance gap / reverified closed, descriptor scope | Both existing partition handoffs pass in fresh Maya. Streaming-on reports 76 total descriptors, 9 spawned, 3 unspawned authored actors and 64 HLOD proxies with SCOPE_LOADED_ONLY; the comparison handoff has confirmed descriptor coverage and no such warning. | Descriptor scope only, not runtime streaming or arbitrary nested worlds. |
| R-006 / Standards confirmed defect / reverified closed, P2 | Independent partial-import plus namespace-removal refusal returns exit 1 without escaping; both FBX_IMPORT_FAILED and STAGING_CLEANUP_FAILED remain in problems, discard errors/residual nodes are recorded and old UUID survives. Maintained suite also verifies subsequent recovery retaining unmarked leftovers. | Closed for the reported cleanup-exception contract. |

## Review-02 reproduction and retained evidence

The minimal [probe script](issue-53-review-02-probes.py) covers the two failing
paths absent from the maintained suite. The [compact evidence](issue-53-review-02-evidence.json)
preserves the exact tested baselines, handoff hashes, counts and structured
observations. These files are retained to reproduce and substantiate the blockers;
full logs and exploratory reports remain scratch evidence, not durable attachments.

```text
<mayapy> docs/project-history/mtou-livelink/issue-53-review-02-probes.py --fbx <main-fixture>.fbx --manifest <main-fixture>.manifest.json --result <scratch>/recovery.json
```

On the reviewed baseline both `R-001_reproduced` and `R-002_reproduced` were
true and the probe exited 1. This is intentional diagnostic failure; successful
script execution does not establish product acceptance. On the fix baseline the
same command prints `{"R-001_reproduced": false, "R-002_reproduced": false}` and
exits 0. Use isolated MAYA_APP_DIR and disposable standalone scenes. Generate the
main handoff through the maintained prototype's Unreal fixture/RealMayaPeer
route. The review reused the 2026-10-08 development handoff rather than
regenerating it, and the fix round did the same. Scratch evidence is
repository-adjacent `.tmp/issue53-acceptance-20261009/` (`pure.log`, `host.json`,
`host.log`, `reverify.py/json/log`, `recovery.json/log`) and
`.tmp/issue53-fix-20261009/` (`host.json`, `recovery.json`). No actual host crash
was induced.

## R-001/R-002 fix round

The recovery of `MtoUSceneRefPrototype.py` was changed so an unresolved state is
refused instead of interpreted:

- A half-swapped destination namespace is deleted only while its top level holds
  nothing the tool does not own. A mixed one is refused with
  `CONTAINER_NOT_OWNED` (exit 1) and nothing is deleted (R-002).
- A destination deletion the host refuses, a retiring reference that cannot be
  renamed back, and a retired copy of a finished container that cannot be deleted
  all refuse the run with the new `RECOVERY_REFUSED` category (exit 1). The
  previous reference stays in its retiring name and its UUID survives (R-001).
- The container's completion mark is written only when no retiring copy exists
  and the namespace is unambiguously this tool's staged group, so an unverified
  takeover is never promoted to a finished one (R-001).
- The takeover loop no longer stops after the first retiring name, and the
  retiring family is re-read after every restore, so several leftovers are each
  handled by the rule that matches their state.

Both review-02 reproductions became maintained host checks in the same style as
the earlier probes: the three-run sequence (final read back exception with the
destination deletion refused, a dry run with the deletion still refused, a dry
run after the fault is gone) and the mixed half-swapped destination in a dry run
and in a normal run. The review's own probe script was rerun unchanged on the
same 2026-10-08 main fixture and now reports both defects unreproduced. Maya pure
tests stay at 110; the host suite is 299 checks with 0 failures;
`python tools/validate_repository.py` and `python -m unittest discover -s tests`
pass. The UE side is untouched by this round and was not rebuilt or rerun; the
dev-02 UE 10/10 remains historical development evidence.

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
| State the level/sublevel selection semantics and report support/omissions for each object type | Accepted within the reported fixture boundaries, independently rechecked 2026-10-09. Explicit loaded-level scope, mixed-Blueprint filtering and Level Instance refusal keep their evidence. Nanite still has only a flagged cube sample. Landscape now passes Maya verification for the fixture (R-004); a partitioned scope states its own completeness from a read-only descriptor inventory and the Maya side warns when it is not confirmed (R-005). R-004 and R-005 independently pass within these fixture-level conditions; see the current review table. |
| Reference geometry enters an identifiable container; repeated runs do not overwrite or mix with production objects; large inputs report scale/time/memory without a performance promise | Accepted on the fix baseline, awaiting the reviewer's reverification: normal replacement, the maintained swap-failure cases, partial-import cleanup and both review-02 recovery defects (R-001, R-002) now pass; R-006 is independently closed. Geometry/time/memory reporting exists; no production-scale performance claim is made. |
| Preserve world space; if a reference origin offset is proposed, give one shared conversion contract for camera, character and props; do not depend on the camera prototype to verify the geometry | Done, with the checked contract: the engine handoff's point map is `ue (x, y, z) -> maya (x, z, y)`, and the product's own animation route sends Maya coordinates to Unreal with the same `(x, z, y)` map (`MtoULiveLink.py`'s `convert_transform`, self-inverse), so the reference imported in the handoff's own world already agrees with the character and props the artist animates. The camera route's `(y, z, -x)` is the divergent map, and the reference import now delivers that world explicitly (`--target-world camera`, one conversion `camera_map . engine_map^-1`, determinant `+1`, 90 degrees about the up axis, applied to every root, reported with its matrix, angle and root count). Both worlds were verified on the same non-origin sample and on the real level. No origin offset is proposed. |
| Deliver the sample, measurements, support list, reusable API and a minimal product proposal | Runnable prototype and measured samples exist; the review-02 recovery defects are fixed on the fix baseline and await independent reverification. Product candidates below belong to #47. |

## Evidence

The table below preserves development-run observations; the current independent 2026-10-09 results and provenance are recorded above. The final 2026-09-29
review reran Maya pure tests (103/103) and host checks (251, zero failures), then
ran the failure probes and Landscape dry-run described above. It inspected the
existing UE reports but did not rebuild UE, rerun UE Automation or re-export the
real level. A passing suite total does not cover the newly confirmed failures.
`RealMayaPeer` can return success without launching Maya when its mayapy argument
is absent; actual peer reports are required for a cross-host claim.

| Check | Command shape | Result |
| --- | --- | --- |
| Unreal suite | `UnrealEditor-Cmd <ToolsLab.uproject> -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests MtoUSceneRefPrototype" -TestExit="Automation Test Queue Empty"` with `-MtoUSceneRefMayapy=`, `-MtoUSceneRefPeer=` and `-MtoUEvidence=` | 10 of 10 tests pass on 2026-10-08: `ScopeSemantics`, `ExportMainScope`, `TextureDetector`, `TextureProbe`, `MixedBlueprintFilter`, `NaniteSourceMesh`, `LandscapeScope`, `LevelInstanceRefusal`, `WorldPartitionScope`, `RealMayaPeer` |
| Maya pure tests | `python -m unittest discover` and the same under `mayapy` over the Maya prototype tests | 110 tests pass on 2026-10-08 under CPython and under mayapy 3.10.8 (103 before the previous round; the added class covers the scope coverage statement) |
| Maya host checks | `mayapy tests/maya_host_scene_ref_tests.py --result <json>` | 291 checks, 0 failures, `ok: true` on 2026-10-08 after the review-01 fixes (274 under the reviewed baseline; the three added probes are the review's R-001, R-002 and R-006 reproductions). The R-001/R-002 fix round raised the suite to 299 checks, 0 failures, `ok: true` on the fix baseline (Maya 2024 mayapy 3.10.8); the new probes are the review-02 continuous recovery sequence and the mixed half-swapped destination in both dry-run and normal runs. |
| Cross-host handoff | The `RealMayaPeer` test exports the fixture scope and runs the Maya importer/verifier over the produced file | `ok: true`, no problems, 12/12 objects compared (9 by name, 3 by transform), position `0.0 cm`, size `4.5e-13 cm`, offset `1.1e-13 cm`, centroid `9.3e-13 cm`, orientation `1.2e-6 deg` over 2 pinning objects, `file_texture_nodes: 0`, engine map `maya_x=ue_x, maya_y=ue_z, maya_z=ue_y`, staged swap |
| Camera world | The same handoff with `--target-world camera` | `ok: true`, no problems, the winner is the camera map `maya_x=ue_y, maya_y=ue_z, maya_z=-ue_x`, `objects_converted: 8`, `conversion_determinant: 1.0`, `conversion_rotation_deg: 90.0`, and `world.engine_check` names the engine map with a `0.0 cm` error before the conversion |
| Media rule | A texture-driven material in scope; a copied image file next to a clean handoff; an injected `Content:` payload in a real `Video:` record | Texture record present: imports, keeps the file's shading group, creates the `file` node, warns when the recorded path resolves locally. Image file beside the handoff: exit 1 `IMAGE_DATA_PRESENT` / `IMAGE_FILES_IN_HANDOFF`, nothing imported; with `--allow-image-data` it imports and warns. Embedded payload: exit 1 `IMAGE_DATA_PRESENT` / `EMBEDDED_MEDIA_PRESENT`; with the flag, `embedded_media_records: 1` and the assigned material is still the file's own |
| Update recovery | Repeat run, wrong-value manifest after a good import, and `--dry-run` | Repeat: `update.mode: staged_swap`, `swapped: true`, the replaced group gone by UUID, identical node list. Failure: exit 1, `staged_swap_discarded`, `discarded_node_count: 18`, staging namespace gone, the previous reference unchanged (node identity, matrices, node count) and production untouched. `--dry-run`: exit 0, `update.mode: dry_run`, nothing swapped |
| Review-01 fix probes (2026-10-08, after the fixes) | `mayapy tests/maya_host_scene_ref_tests.py` phase `ownership`, the three probes below | R-001: `_rehome_paths` replaced by a raising function returns exit 1 / `refused` / `STAGED_SWAP_FAILED` with `rolled_back: true`, `rollback.previous_reference_restored: true`, `rollback_error` set and the failing step named; the previous UUID is alive under `|MtoU_UE_SceneRef:MtoU_UE_SceneRef`, no staging or retiring namespace is left, and a following `--dry-run` keeps it. R-002: a marked group next to an unmarked `ProductionObject` in `<container>_Incoming` leaves the foreign UUID alive while only the marked group is deleted and reported (`partial_cleanups`), and a retiring namespace in that state is refused with `CONTAINER_NOT_OWNED` and nothing deleted. R-006: a refused `removeNamespace` no longer escapes — exit 1 with the original `FBX_IMPORT_FAILED`, `discarded: false`, `discard_attempted: true`, `discard_errors` and `residual` present, `STAGING_CLEANUP_FAILED` in `problems`, the previous reference and the production object intact, and the next run sweeping the marked group while reporting the unmarked leftover |
| Update failure injection (2026-10-08, reviewed baseline) | `mayapy tests/maya_host_scene_ref_tests.py` phase `ownership`: namespace rename failure, group rename failure, forced missing path after the swap, partial FBX import, unowned container namespace, unowned staging namespace, stale marked staging namespace, takeover interrupted after the retire | Namespace rename and group rename failures: exit 1 `STAGED_SWAP_FAILED`, `rolled_back: true`, `rollback.previous_reference_restored: true`, the previous group's UUID alive, no staging or retiring namespace left. Missing path: `rolled_back: true` with `rollback_reason` naming the unresolved path, previous reference back. Partial import: exit 1 `FBX_IMPORT_FAILED`, `discarded: true`, the partial node listed in `discarded_nodes` and gone, previous reference intact. Unowned container namespace: exit 1 `CONTAINER_NOT_OWNED`, the foreign node alive, nothing imported. Unowned staging namespace: exit 0, the run used `MtoU_UE_SceneRef_Incoming_1`, the foreign node untouched. Stale marked staging namespace: swept, `stale_staging_removed: true`, the unmarked one kept. Interrupted takeover: the retired reference is renamed back and reported before the next run proceeds |
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
- Incoming recovery deletes only the marked group (and the nodes under it), and
  the same rule now covers the half-swapped destination: a destination namespace
  that also holds an unmarked top-level node is refused with
  `CONTAINER_NOT_OWNED` rather than emptied, and the retiring copy is neither
  restored nor deleted. An unmarked top-level node in an Incoming namespace —
  which is how a partially imported FBX failure looks, and equally how an object
  the scene put there looks — is left in place and reported
  (`update.recovery.partial_cleanups`), so the operator decides. A production
  object that shares a name with the container is refused, not interpreted.
- The fix probes inject faults in process (`_rehome_paths`, `cmds.namespace`,
  `cmds.rename`, `cmds.file`, `delete`); they do not kill the host, so crash
  recovery across processes and a Maya session that dies mid-takeover remain
  unproven, and the same probe set has not been run under a real artist session.
- A cleanup the host refuses leaves a `STAGING_CLEANUP_FAILED` problem and the
  residual names; the recovery that follows is a best effort over provable
  resources, so a scene can keep an unmarked node from a failed import until a
  human removes it.
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
not supersede this issue's current explicit-level scope or image-data policy.
The R-001/R-002 recovery fixes belong to this issue and do not change the
integration options.

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
