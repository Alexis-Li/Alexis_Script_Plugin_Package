# Issue #53: selected-level reference geometry in Maya

Date: 2026-09-28, extended 2026-09-29. A bounded two-host prototype was built and
measured on Unreal 5.7.4 and Maya 2024. Unreal resolves an explicit level scope,
exports it with the engine's own FBX level exporter, suppresses the components
that are not static reference geometry, and writes a manifest of the world data it
evaluated; Maya stages the file in its own namespace, keeps the material
assignment it carries, shows the geometry in one uniform gray, places it in the
handoff's world or in the camera route's world by one explicit conversion,
compares what it holds against that manifest, and only then replaces the previous
reference. Issue #53 stays open for the product decisions listed at the end;
nothing here is a commitment to a scene-reference product, and no product code,
protocol, or package changed.

The 2026-09-29 work followed the evaluation recorded on the issue and closed its
five follow-ups: the media rule, the world contract, the mixed-Blueprint
component boundary, update failure recovery, and real-level acceptance.

## What was delivered

| Area | Result |
| --- | --- |
| Official capability review | Unreal 5.7 exports a level through `ULevelExporterFBX`, `ULevelExporterOBJ`, STL or T3D; only the FBX path keeps hierarchy, instancing and actor labels, and only the FBX path can be kept free of image data. All findings are source-referenced in the prototype's [official capability review](../../../composite/MtoULiveLink/prototypes/scene-reference/official-capabilities.md). |
| Prototype | An editor-only Unreal module (scope resolution, static-geometry filter, fixture, export, produced-file inspection, manifest) and a Maya importer/verifier (media check, staged update, two worlds, comparison, report), with the contract in [transfer.md](../../../composite/MtoULiveLink/prototypes/scene-reference/transfer.md). |
| Sample | A generated fixture of seven levels under `/Game/MtoUSceneRefFixture`: off-origin, rotated, non-uniformly scaled and instanced static meshes, a two-component Blueprint actor, a mixed Blueprint actor (mesh, light, camera, child actor, skeletal mesh), a Nanite-enabled mesh, a texture-driven material, one held and one unheld sublevel, a landscape, a level instance and a World Partition level. |
| Cross-host session | One real handoff of twelve objects, measured end to end on both hosts, with the axis convention and the node frame factor fitted from the imported geometry rather than assumed. |
| Real level | The user's own character level (`/Game/Untitled` in the local test project) exported and verified on both hosts, in both worlds. |

## Acceptance criteria

| Criterion | Outcome |
| --- | --- |
| Check the official level-export capability and deliver a minimal UE → file → Maya sample; do not build a general mesh serializer | Done. The geometry comes from `ULevelExporterFBX` through `UAssetExportTask`; the prototype only resolves the scope, filters the components, drives the editor selection the exporter reads, inspects what it wrote, and serializes a manifest of transforms. The OBJ level exporter was measured as the alternative and rejected (below). |
| Cover plain static meshes and repeated instances, including off-origin, rotated and non-uniformly scaled samples; compare Maya world position, orientation and size against Unreal, and verify axes and units | Done for all four sample kinds. Twelve objects across the persistent level, its sublevel and the mixed Blueprint arrive in the container, all twelve matched (nine by node name, three instanced children by world position). Position error `0.0 cm`, bounding-box size error `4.5e-13 cm`, pivot-offset error `1.1e-13 cm`, surface centroid error `9.3e-13 cm`, and the worst node axis angle `1.2e-6` degrees over the two objects whose identification size pins their axes. The centroid is the mirror check; the orientation check compares each node matrix against `frame . L_ue . map`, the relation the file really uses (below). |
| Neither output nor import may copy, package or auto-load textures; use a simple gray material; a disabled bake is not a substitute for the check | Done by inspection, not by flag, and with the corrected reading of the requirement: material assignments and recorded texture paths are kept and reported, and only image **data** — image files in the directory the exporter owns, or media embedded in the FBX — refuses a run (`IMAGE_DATA_PRESENT` unless `--allow-image-data`). Measured: the fixture handoff writes one file with no image files, zero texture records and zero embedded media records; a texture-driven material makes the exporter write one texture record, and that handoff now imports, keeps the shape's own shading group, creates the `file` node, reports whether its path resolves to a file on this machine, and warns; an injected `Content:` payload and a copied image file are both refused, and both are imported with the flag while the report keeps the counts. The uniform gray is a display treatment by default (a viewport override, materials preserved), with `--shading material` for the gray lambert and `--shading keep` for nothing. |
| State the level/sublevel selection semantics and the handling of unloaded content; list results for Blueprint components, Level Instance, Landscape, Nanite and similar, reporting rather than silently dropping | Done. The scope is the loaded persistent level plus the sublevels the caller names; a requested level the world does not hold is reported (`not_in_world`), an unrequested sublevel is reported as excluded, and nothing is streamed in. A mixed Blueprint actor's non-static components are suppressed for the duration of the export and listed in `filter.suppressed_components` (measured on `BP_SceneRefMixed`: light, camera, child actor and skeletal mesh components, five records including the standalone light actor's component), with the file holding `camera_records: 0`, `light_records: 0` and no node for the child actor's mesh. Level instances are refused by the engine with `"Exporting Level Instances to FBX is not supported."` and reported with that reason while contributing no node. Landscape is measured: the engine's own branch wrote 7938 polygons for a 64x64 fixture landscape while the scope's landscape record counted no triangles, so the report and the file disagree on the scale numbers and the gap is recorded. Nanite is measured: the duplicate cube reads back `bEnabled=true`, the file carries its LOD 0 render data (48 triangles, 54 vertices), the node arrives in Maya and no warning mentions it. World Partition is measured: as authored with the editor's streaming on and no streaming source, every placed actor of a partitioned fixture level is absent from the loaded world and the scope holds only the template's always-loaded actor; with streaming disabled for the export the three placed actors enter the scope and the manifest reports `world_partition: true`. |
| Reference geometry enters an identifiable container; repeated runs do not overwrite or mix with production objects; large inputs report object/geometry scale first, with time and memory recorded and no performance promise | Done, and the update is now staged: the handoff is imported into `<container>_Incoming`, compared there, and only then is the previous container deleted and the staging namespace renamed into the container's name. A failed check, a refused handoff or `--dry-run` deletes the staging namespace and leaves the previous reference exactly as it was (host-checked by node identity, matrices and node count, with a production cube, its material and its keys untouched). The exporter reports actors, components, objects, triangles and vertices before exporting, plus scope/export/inspect seconds and used physical memory; the importer reports its own staging, verification and total seconds and its process RSS. No budget or promise is attached. |
| Preserve world space; if a reference origin offset is proposed, give one shared conversion contract for camera, character and props; do not depend on the camera prototype to verify the geometry | Done, with the checked contract: the engine handoff's point map is `ue (x, y, z) -> maya (x, z, y)`, and the product's own animation route sends Maya coordinates to Unreal with the same `(x, z, y)` map (`MtoULiveLink.py`'s `convert_transform`, self-inverse), so the reference imported in the handoff's own world already agrees with the character and props the artist animates. The camera route's `(y, z, -x)` is the divergent map, and the reference import now delivers that world explicitly (`--target-world camera`, one conversion `camera_map . engine_map^-1`, determinant `+1`, 90 degrees about the up axis, applied to every root, reported with its matrix, angle and root count). Both worlds were verified on the same non-origin sample and on the real level. No origin offset is proposed. |
| Deliver the sample, measurements, support list, reusable API and a minimal product plan; keep the texture-free scene reference independent of the existing animation export flow | Done, plus the decisions below. The prototype touches no product protocol, no animation export, and no package. |

## Evidence

| Check | Command shape | Result |
| --- | --- | --- |
| Unreal suite | `UnrealEditor-Cmd <ToolsLab.uproject> -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests MtoUSceneRefPrototype" -TestExit="Automation Test Queue Empty"` with `-MtoUSceneRefMayapy=`, `-MtoUSceneRefPeer=` and `-MtoUEvidence=` | 10 of 10 tests pass: `ScopeSemantics`, `ExportMainScope`, `TextureDetector`, `TextureProbe`, `MixedBlueprintFilter`, `NaniteSourceMesh`, `LandscapeScope`, `LevelInstanceRefusal`, `WorldPartitionScope`, `RealMayaPeer` |
| Maya pure tests | `python -m unittest discover` and the same under `mayapy` over the Maya prototype tests | 103 tests pass under CPython and under mayapy 3.10.8 |
| Maya host checks | `mayapy tests/maya_host_scene_ref_tests.py --result <json>` | 251 checks, 0 failures, `ok: true` |
| Cross-host handoff | The `RealMayaPeer` test exports the fixture scope and runs the Maya importer/verifier over the produced file | `ok: true`, no problems, 12/12 objects compared (9 by name, 3 by transform), position `0.0 cm`, size `4.5e-13 cm`, offset `1.1e-13 cm`, centroid `9.3e-13 cm`, orientation `1.2e-6 deg` over 2 pinning objects, `file_texture_nodes: 0`, engine map `maya_x=ue_x, maya_y=ue_z, maya_z=ue_y`, staged swap |
| Camera world | The same handoff with `--target-world camera` | `ok: true`, no problems, the winner is the camera map `maya_x=ue_y, maya_y=ue_z, maya_z=-ue_x`, `objects_converted: 8`, `conversion_determinant: 1.0`, `conversion_rotation_deg: 90.0`, and `world.engine_check` names the engine map with a `0.0 cm` error before the conversion |
| Media rule | A texture-driven material in scope; a copied image file next to a clean handoff; an injected `Content:` payload in a real `Video:` record | Texture record present: imports, keeps the file's shading group, creates the `file` node, warns when the recorded path resolves locally. Image file beside the handoff: exit 1 `IMAGE_DATA_PRESENT` / `IMAGE_FILES_IN_HANDOFF`, nothing imported; with `--allow-image-data` it imports and warns. Embedded payload: exit 1 `IMAGE_DATA_PRESENT` / `EMBEDDED_MEDIA_PRESENT`; with the flag, `embedded_media_records: 1` and the assigned material is still the file's own |
| Update recovery | Repeat run, wrong-value manifest after a good import, and `--dry-run` | Repeat: `update.mode: staged_swap`, `swapped: true`, the replaced group gone by UUID, identical node list. Failure: exit 1, `staged_swap_discarded`, `discarded_node_count: 18`, staging namespace gone, the previous reference unchanged (node identity, matrices, node count) and production untouched. `--dry-run`: exit 0, `update.mode: dry_run`, nothing swapped |
| Object kinds | `NaniteSourceMesh`, `LandscapeScope`, `LevelInstanceRefusal`, `WorldPartitionScope` automation tests | Nanite: render data exported (48 triangles, 54 vertices) and matched in Maya. Landscape: 7938 polygons written by the engine's branch against 0 triangles in the scope's own record. Level instance: refused by the engine, reported, no node. World Partition: nothing streamed in is absent from the scope; with streaming disabled the three placed actors export and `world_partition: true` |
| Real level | The user's test project (`/Game/Untitled`, a character scene) through a temporary project descriptor that adds the prototype plugin directory | 15 actors in scope, 3 exported objects (sky sphere, template floor, one generated box), 12 actors reported skipped with reasons, one BSP brush skipped, `unsupported: []`; handoff 2.16 MB, one file, no image files, zero texture records; scope `0.0002 s`, export `0.053 s`, inspect `0.071 s`, used physical memory `2184.96 MB`; the Maya peer verified 3/3 objects in both worlds (`0.0 cm` position, `2.4e-4 cm` size on an 8000 cm floor, `1.2e-11 cm` centroid, camera world `objects_converted: 3`) with no problem and no warning. No object of that level pins its axes, so the orientation check reported itself unavailable for all three |
| Repository gates | `python tools/validate_repository.py`, `python -m unittest discover -s tests` | Pass |
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
| Landscape | Carried by the engine's landscape branch; the scope's own record reports no geometry for it, so its triangle count is not the file's polygon count (measured: 7938 polygons written, 0 triangles reported) |
| Nanite mesh | LOD 0 render data exported; the Nanite source mesh is not (`bExportSourceMesh` off), and the node matches in Maya (measured) |
| World Partition | Content that is not streamed in is absent from the scope and reported as such; disabling streaming for the export brings spatially loaded actors into the scope (measured) |
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
- The landscape acceptance measures the engine's behaviour, not a supported
  reference path: the scope's geometry scale for a landscape is incomplete.
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
  product decision with a documented map, and the landscape scale gap has to be
  closed before the scale numbers can be trusted for terrain.

## Pending user decisions

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
