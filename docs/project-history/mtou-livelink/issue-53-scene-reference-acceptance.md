# Issue #53: selected-level reference geometry in Maya

Date: 2026-10-09. **Accepted within the agreed prototype scope.** Independent
review-03 verified implementation `e2462d92e63f25f13212f4e2aa9d4e55c17f032b`; the
checkout `ddd8bb9e274261000875bbd3cc935b0354b83aa8` has identical prototype
sources. R-001 and R-002 are reverified closed; R-003 through R-006 retain their
bounded approval. The final current conclusion links the published implementation and acceptance commits. MtoU integration and the joint camera/character/prop contract remain
with #47 after the three prototypes have runnable evidence and stated limits.

The [final current conclusion](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/53#issuecomment-5888512464)
records the issue disposition. The verified implementation SHA above is authoritative; source archive entry
6073878619 retains the original dev-03 report with its superseded baseline typo.

## Final verification

This independent run passed 110 pure tests and 299 maintained Maya 2024 host
checks in isolated preferences and disposable standalone scenes. It reran the
unchanged review-02 probe against the same valid UE main-fixture handoff: both
reproduced flags are false, exit 0. Four additional checks verify refused restore
renames, refused removal of a retired copy beside a completed container, and
successful retries after each fault is removed. No production scene was opened
or saved. The maintained suite includes normal and dry-run mixed-destination
refusals, successful replacement, original failure rollback and cleanup errors.

| Finding | Independent result | Accepted boundary |
| --- | --- | --- |
| R-001 / Spec / reverified closed | Final readback plus rollback-deletion refusal returns exit 1 and keeps the old UUID in Retiring. Retry while deletion is refused returns RECOVERY_REFUSED without a completed mark. Removing the fault permits dry-run to restore that same UUID under the container name, with no retired-leftover deletion. Refused restore rename and refused retired-copy removal also return structured failure and recover on retry. | In-process host fault injection and retry; no claim of actual process-crash certification. |
| R-002 / Spec / reverified closed | Mixed half-swapped target is refused with CONTAINER_NOT_OWNED before import or deletion. The foreign UUID, staging group and retired reference survive both dry-run and normal retry. Mixed Incoming retains foreign nodes; mixed Retiring is refused. | The documented top-level transform ownership model; arbitrary deeply nested foreign content is not classified. |
| R-003 / Spec / reverified closed | Maintained partial-import cleanup checks pass, preserving the prior UUID. | Successful cleanup; cleanup refusal separately covered by R-006. |
| R-004 / Spec acceptance gap / closed | Review-02 independently imported the existing Landscape handoff: 7938 triangles, position/size/offset error 0 cm, centroid approximately 6.4e-13 cm. Reused evidence; UE geometry and measurement sources are unchanged. | Flat single-component LOD0 terrain without visibility holes. |
| R-005 / Spec acceptance gap / closed | Review-02 independently imported two WP handoffs: streaming-on reports 76 descriptors, 9 spawned, 3 unspawned authored actors, 64 HLOD proxies and SCOPE_LOADED_ONLY; the comparison confirms descriptor coverage. Reused evidence; scope code unchanged. | Descriptor inventory, path list capped at 200; not runtime streaming or arbitrary nested worlds. |
| R-006 / Standards / reverified closed | Prior independent partial-import plus removal refusal returned stable exit 1 with FBX_IMPORT_FAILED, STAGING_CLEANUP_FAILED and residual resources; this run's maintained suite passes that path and its retry. | Unmarked leftovers require operator judgment; no arbitrary deletion to hide cleanup failure. |

No issue-level blocker remains. Production scale, complex Nanite, multi-component
terrain, visibility layers and real process crashes remain unverified extensions,
not additional prototype acceptance gates. This run did not rebuild UE, rerun UE
automation, regenerate its handoffs or reopen C01. The dev-02 UE 10/10 and earlier
real-level measurements remain explicitly historical evidence; this acceptance
does not describe them as newly executed independent UE tests.

## Reproduction and evidence index

| Retained record | Purpose |
| --- | --- |
| [Final evidence](issue-53-acceptance-evidence.json) | Exact baselines, 110/299 checks, independent failure/retry observations and maintained R-001/R-002 results; machine path prefixes in tracebacks are redacted. |
| [Source archive](issue-53-comment-archive.json) | Exact archived comment bodies and timestamps, including previous_versions for revised comments. Dev-03 has two machine-path prefix redactions, explicitly documented with its original-body SHA-256; the remaining text and timestamps are preserved. The archive also contains the formal final review-03. |
| Maintained `maya_host_scene_ref_tests.py` | Both former review-02 failures now have formal regression coverage, so the separate probe script and old failing JSON are no longer kept in the current history tree. They remain recoverable at commit `ddd8bb9e274261000875bbd3cc935b0354b83aa8` under their original paths. |

Earlier archive files and the pre-acceptance record are recoverable at that same
immutable commit. Their unique comment bodies have been merged into the one
current source archive. The final current conclusion is the sole retained agent
report after remotely verifying the archive and removing five superseded reports.
The six live source snapshots comprise that prior current conclusion plus those
five reports; three older removed reports and prior revisions remain archived too.
No human discussion or unresolved dissent is removed. Publication and final
comment counts are verified by the closing operation, not inferred from a local
archive check.

```text
<mayapy> -m unittest discover -s <prototype>/tests -t <prototype>
<mayapy> <prototype>/tests/maya_host_scene_ref_tests.py --result <scratch>/host.json
```

`<prototype>` is `composite/MtoULiveLink/prototypes/scene-reference/maya/MtoUSceneRefPrototype`.
Use an isolated MAYA_APP_DIR and regenerate handoffs using the prototype README.
The final evidence's `adjacent_probe_source` preserves the four independent
fault-injection inputs with their reproduction conditions; the main recovery
failures have maintained regression coverage. Disposable logs, fixtures and
publication helpers are not reproduction dependencies. The final summary links
to the published archive and evidence. Archival checks do not rerun host acceptance.

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
| Reference geometry enters an identifiable container; repeated runs do not overwrite or mix with production objects; large inputs report scale/time/memory without a performance promise | Independently accepted on the final implementation baseline: normal replacement, the maintained swap-failure cases, partial-import cleanup and both review-02 recovery defects (R-001, R-002) now pass; R-006 is independently closed. Geometry/time/memory reporting exists; no production-scale performance claim is made. |
| Preserve world space; if a reference origin offset is proposed, give one shared conversion contract for camera, character and props; do not depend on the camera prototype to verify the geometry | Done, with the checked contract: the engine handoff's point map is `ue (x, y, z) -> maya (x, z, y)`, and the product's own animation route sends Maya coordinates to Unreal with the same `(x, z, y)` map (`MtoULiveLink.py`'s `convert_transform`, self-inverse), so the reference imported in the handoff's own world already agrees with the character and props the artist animates. The camera route's `(y, z, -x)` is the divergent map, and the reference import now delivers that world explicitly (`--target-world camera`, one conversion `camera_map . engine_map^-1`, determinant `+1`, 90 degrees about the up axis, applied to every root, reported with its matrix, angle and root count). Both worlds were verified on the same non-origin sample and on the real level. No origin offset is proposed. |
| Deliver the sample, measurements, support list, reusable API and a minimal product proposal | Runnable prototype and measured samples exist; all six findings are closed within the stated prototype scope. Product candidates below belong to #47. |

## Evidence

The table below preserves the applicable development-run measurements. The final
independent 2026-10-09 results and their provenance are recorded above. Historical
counts are not the final review's counts, and old failure narratives remain in
the source archive rather than defining the current result.
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

#53 has completed its bounded verification. Once #52, #53 and #54 have their
agreed prototype evidence and support boundaries, #47 owns a combined integration
plan. The options below do
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
