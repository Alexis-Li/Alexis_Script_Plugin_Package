# Issue #53: selected-level reference geometry in Maya

Date: 2026-09-28, extended 2026-09-29 and 2026-10-08. A bounded two-host
prototype was built and measured on Unreal 5.7.4 and Maya 2024. Unreal resolves an
explicit level scope, exports it with the engine's own FBX level exporter,
suppresses the components that are not static reference geometry, and writes a
manifest of the world data it evaluated; Maya stages the file in its own
namespace, keeps the material assignment it carries, shows the geometry in one
uniform gray, places it in the handoff's world or in the camera route's world by
one explicit conversion, compares what it holds against that manifest, and only
then replaces the previous reference — after the new container resolved every
recorded path, with the previous one renamed aside rather than deleted. Issue #53
remains open: the fixed findings still need independent re-verification, and the
support boundaries below (one fixture landscape, descriptor-level World Partition
coverage, injected rather than real process failures) are unproven outside their
tested conditions. Product decisions are separate work owned by #47; no product
code, protocol, or package changed.

Current assessment: the four engineering findings of the 2026-09-29 review of
`0f1367ebceb4d57bb0deb02dfbea0fb6527b9998` (preserved as comment `5888512464` in
[the source archive](issue-53-comment-archive.json)) were fixed in
`489b1c5`; the landscape acceptance gap was closed for the fixture
by measuring the source the engine's landscape branch writes, and the World
Partition gap by stating the scope's completeness from a read-only descriptor
inventory. The fixes are senior-authored development results: they are reported as
"fixed, awaiting re-verification", not as accepted, and no review has yet
reproduced the fault injections against this baseline. The FBX route, media
policy, two explicit worlds and mixed-Blueprint filtering keep their earlier
evidence.

This record was reconciled on 2026-10-08 against a fresh pair of runs: the Unreal
automation suite (10/10, including the real Maya peer) and the Maya pure (110) and
host (274, zero failures) checks, all on this machine, plus one hand-run
cross-host import of the landscape handoff and one of each partitioned handoff.

## Outstanding acceptance

These IDs are local references for the reviewed findings, not claims that the
historical comments already used them.

| ID / type | Evidence and remaining work | Completion criterion / owner |
| --- | --- | --- |
| R-001 / confirmed defect | During namespace rename failure the previous reference was deleted, both namespaces were absent, yet `container.kept_existing` was true. | Fixed in `489b1c5`, awaiting re-verification. A takeover now renames the previous container into a free `<container>_Retiring` name, renames the staged namespace into the container's name, resolves every recorded path, and deletes the retired reference only when none is missing; a failure in any of those steps deletes the container this run created and renames the retired reference back. `container.kept_existing` and `container.previous_reference_present_after_run` are read back from the scene by UUID at the end of the run. Host checks inject a namespace rename failure, a group rename failure and a final path read back that does not resolve: all three keep the previous group's UUID alive, report `update.rolled_back` with `update.rollback.previous_reference_restored`, and leave no staging or retiring namespace. |
| R-002 / confirmed defect | Both staging and final-container cleanup deleted simulated production objects in a colliding namespace without verifying ownership. | Fixed in `489b1c5`, awaiting re-verification. The container group carries the ownership mark `mtouSceneRefContainer` (`mtou-scene-ref-container/1 <namespace>`); a namespace, group or root-level node of the container's name without it — or a foreign top-level transform inside an owned namespace — is refused with `CONTAINER_NOT_OWNED` and exit 1, and nothing is deleted. Staging names come from the free `<container>_Incoming[_N]` family, so a namespace of that name that belongs to the scene is neither reused nor emptied; a marked namespace of that family left behind by an interrupted run is swept and reported. Host checks cover the unowned container collision (production node alive, namespace untouched) and the unowned staging-name collision (the run uses `..._Incoming_1`, the foreign node stays). |
| R-003 / confirmed defect | A partially completed FBX import raised before `self.staged` was set, leaving staging nodes behind (`discarded: false`). | Fixed in `489b1c5`, awaiting re-verification. The run takes ownership of the staging namespace before it creates it, so the partial import now reports `update.discarded: true` with `discarded_nodes` naming the partial nodes, `staging_namespace_removed: true`, and the previous reference intact. Host checks inject the partial import by creating a node and raising inside `cmds.file`. The container identity (previous group, previous UUID) is recorded before the import, so a failure during the import still reports what the scene held. |
| R-004 / acceptance gap | Landscape dry-run failed with size error 512 cm and centroid error 5702.1091 cm over 7938 triangles. The manifest and exported geometry use inconsistent measurement bases; this does not establish deformation. | Fixed for the fixture in `489b1c5`, awaiting re-verification. The resolver now measures a landscape from the source the engine's landscape branch writes (`FLandscapeComponentDataInterface` vertices at `ALandscapeProxy::ExportLOD`, two triangles per quad, the component's relative location and the actor transform applied) instead of the actor's collision/editor bounds; the Automation test checks the counts and shape against the actor's own numbers and the manifest, and the Maya peer run of 2026-10-08 reported size error `0.0 cm`, surface-centroid error `6.4e-13 cm` over `7938` triangles, position `0.0 cm` and offset `0.0 cm`. Boundary: one flat, single-component fixture landscape at export LOD 0, no visibility layer; a production terrain is unproven. Product inclusion remains a #47 decision. |
| R-005 / acceptance gap | Loaded-only traversal does not enumerate unloaded World Partition actors/cells; `unloaded_sublevels: []` does not prove complete coverage. | Fixed in `489b1c5`, awaiting re-verification. The resolver walks the World Partition's actor descriptor containers read only (no content is loaded), counts descriptors the loaded world has spawned, names the unspawned authored actors (capped at 200 paths) and counts generated HLOD proxies apart, and the manifest carries `scope.coverage`, `scope.completeness`, `scope.coverage_note` and `scope.world_partition`. The Maya side reports a manifest whose completeness is not confirmed as a `SCOPE_LOADED_ONLY` warning. Measured on the fixture: `76` descriptors, `9` spawned and `3` authored actors absent with streaming on (`not_confirmed`, warning), `12` spawned and `0` authored absent with streaming off (`confirmed`, no warning), `64` generated HLOD proxies in both states; both partitioned handoffs were imported in Maya with every object matched. Boundary: descriptor counts, not cell geometry; a `confirmed` scope describes the descriptors, not a running game's streaming. |

Successful fixture checks remain valid within their tested conditions and do
not close these findings. Representative dense/complex scenes and repeat-update
evidence are still needed; the real-level sample contains only three reference
objects. Raw review probes were stored in repository-adjacent
`.tmp/issue53-review-20260929-183244/`; that scratch location is not a durable
artifact. The archive preserves the observed results and command shapes; the
fault-injection probes still need promotion into maintained regression tests.

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
| State the level/sublevel selection semantics and report support/omissions for each object type | Partially accepted, re-measured 2026-10-08. Explicit loaded-level scope, mixed-Blueprint filtering and Level Instance refusal keep their evidence. Nanite still has only a flagged cube sample. Landscape now passes Maya verification for the fixture (R-004); a partitioned scope states its own completeness from a read-only descriptor inventory and the Maya side warns when it is not confirmed (R-005). Both remain fixture-level results awaiting independent re-verification. |
| Reference geometry enters an identifiable container; repeated runs do not overwrite or mix with production objects; large inputs report scale/time/memory without a performance promise | Fixed for the tested paths on 2026-10-08, awaiting re-verification: the container group carries an ownership mark, a same-named namespace this tool does not own is refused, the staging name is a free name of its family, the previous reference survives every injected swap failure by UUID, and a partial import cleans up its own staging namespace (R-001–003). Boundaries: the faults were injected in-process rather than by killing the host, the ownership check covers the container's own name and the top-level transforms inside it, and scale/timing numbers still carry no performance promise. Representative large-input evidence (dense instances, complex Nanite, a production landscape) remains pending. |
| Preserve world space; if a reference origin offset is proposed, give one shared conversion contract for camera, character and props; do not depend on the camera prototype to verify the geometry | Done, with the checked contract: the engine handoff's point map is `ue (x, y, z) -> maya (x, z, y)`, and the product's own animation route sends Maya coordinates to Unreal with the same `(x, z, y)` map (`MtoULiveLink.py`'s `convert_transform`, self-inverse), so the reference imported in the handoff's own world already agrees with the character and props the artist animates. The camera route's `(y, z, -x)` is the divergent map, and the reference import now delivers that world explicitly (`--target-world camera`, one conversion `camera_map . engine_map^-1`, determinant `+1`, 90 degrees about the up axis, applied to every root, reported with its matrix, angle and root count). Both worlds were verified on the same non-origin sample and on the real level. No origin offset is proposed. |
| Deliver the sample, measurements, support list, reusable API and a minimal product proposal | Runnable prototype and measured samples exist; support and failure-recovery acceptance remain bounded by R-001–005. Product candidates below belong to #47. |

## Evidence

The table below preserves development-run observations. The final 2026-09-29
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
| Landscape | Geometry is written, and the record now measures the source that branch writes, so Maya verification passes for the fixture (2026-10-08: size error `0.0 cm`, centroid error `6.4e-13 cm`, `7938` triangles). Not verified beyond one flat, single-component terrain at export LOD 0 with no visibility layer; R-004 awaits re-verification. |
| Nanite mesh | LOD 0 render data exported; the Nanite source mesh is not (`bExportSourceMesh` off), and the node matches in Maya (measured) |
| World Partition | Only loaded content is traversed, and the scope now says so: a read-only descriptor inventory reports how many descriptors have spawned, names the unspawned authored actors and counts generated HLOD proxies apart; `scope.completeness` is `confirmed` only when every authored descriptor is spawned, and Maya warns with `SCOPE_LOADED_ONLY` otherwise (2026-10-08, R-005 awaits re-verification). The inventory counts descriptors, not streamed cell geometry, and records at most 200 paths. Disabling streaming remains a comparison experiment, not a recommended product operation. |
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
and do not block fixing R-001–005.

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
