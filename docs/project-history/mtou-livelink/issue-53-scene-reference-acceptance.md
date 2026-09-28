# Issue #53: selected-level reference geometry in Maya

Date: 2026-09-28. A bounded two-host prototype was built and measured on Unreal
5.7.4 and Maya 2024. Unreal resolves an explicit level scope, exports it with the
engine's own FBX level exporter, and writes a manifest of the world transforms
it evaluated; Maya imports the file into its own container as a texture-free
gray reference and compares what it holds against that manifest. Issue #53 stays
open for the product decisions listed below; nothing here is a commitment to a
scene-reference product, and no product code, protocol, or package changed.

## What was delivered

| Area | Result |
| --- | --- |
| Official capability review | Unreal 5.7 exports a level through `ULevelExporterFBX`, `ULevelExporterOBJ`, STL or T3D; only the FBX path keeps hierarchy, instancing and actor labels, and only the FBX path can be made texture-free. All findings are source-referenced in the prototype's [official capability review](../../../composite/MtoULiveLink/prototypes/scene-reference/official-capabilities.md). |
| Prototype | An editor-only Unreal module (scope resolution, fixture, export, produced-file inspection, manifest) and a Maya importer/verifier, with the contract in [transfer.md](../../../composite/MtoULiveLink/prototypes/scene-reference/transfer.md). |
| Sample | A generated fixture of four levels under `/Game/MtoUSceneRefFixture`: off-origin, rotated, non-uniformly scaled and instanced static meshes, a two-component Blueprint actor, a texture-driven material, one sublevel the world holds and one it does not. |
| Cross-host session | One real handoff of ten objects, measured end to end on both hosts, with the axis convention fitted from the imported positions rather than assumed. |

## Acceptance criteria

| Criterion | Outcome |
| --- | --- |
| Check the official level-export capability and deliver a minimal UE → file → Maya sample; do not build a general mesh serializer | Done. The geometry comes from `ULevelExporterFBX` through `UAssetExportTask`; the prototype only resolves the scope, drives the editor selection the exporter reads, inspects what it wrote, and serializes a manifest of transforms. The OBJ level exporter was measured as the alternative and rejected (below). |
| Cover plain static meshes and repeated instances, including off-origin, rotated and non-uniformly scaled samples; compare Maya world position, orientation and size against Unreal, and verify axes and units | Done for all four sample kinds. Ten objects across the persistent level and its sublevel arrive in the container, all ten matched (seven by node name, three instanced children by world position). Position error `0.0 cm`, bounding-box size error `4.5e-13 cm`, pivot-offset error `1.1e-13 cm`, and the surface centroid of the mesh's triangles `9.3e-13 cm`; axis fit residual `9.2e-13 cm`. The centroid is the mirror check: position, size and pivot offset are all blind to a mirrored placement, and the fixture's dedicated sample (`SM_Cone_Asymmetric`, a sample whose surface centroid sits `266 cm` from where a mirror would put it) agrees to `4.7e-13 cm`, so the reference geometry is not mirrored. Units are centimetres on both hosts; the axis convention was measured (below), not assumed. |
| Neither output nor import may copy, package or auto-load textures; use a simple gray material; a disabled bake is not a substitute for the check | Done by inspection, not by flag. The handoff writes one file, no image files and zero texture records; the Maya side repeats the scan with its own detector. The texture probe (one material whose BaseColor is a texture sample) produces exactly one texture record naming the engine texture's original path, so the check demonstrably bites, and the Maya importer then refuses that handoff with `REFUSED TEXTURE_REFERENCES_PRESENT`. The reference geometry ends on one lambert the importer creates, with zero `file` nodes and zero loaded images. |
| State the level/sublevel selection semantics and the handling of unloaded content; list results for Blueprint components, Level Instance, Landscape, Nanite and similar, reporting rather than silently dropping | Done. The scope is the loaded persistent level plus the sublevels the caller names; a requested level the world does not hold is reported (`not_in_world`), an unrequested sublevel is reported as excluded, and the resolution never loads anything. Blueprint components are exported as their own nodes (measured). Level instances are refused by the engine with `"Exporting Level Instances to FBX is not supported."` and are reported with that reason. Landscape is classified and carried as one object per landscape actor (source-verified branch, not placed as a fixture). Nanite source meshes, skeletal meshes, lights, cameras and emitters are reported as skipped, never silently dropped. |
| Reference geometry enters an identifiable container; repeated runs do not overwrite or mix with production objects; large inputs report object/geometry scale first, with time and memory recorded and no performance promise | Done. The container is one namespace plus one group (`MtoU_UE_SceneRef`); a repeat run deletes only those and re-imports, leaving a production object, its material and its keys untouched (host-checked). The exporter reports actors, components, objects, triangles and vertices before exporting, plus scope/export/inspect seconds and used physical memory; the importer reports its own import and verify seconds and process RSS. No budget or promise is attached. |
| Preserve world space; if a reference origin offset is proposed, give one shared conversion contract for camera, character and props; do not depend on the camera prototype to verify the geometry | Done, with a finding. No origin offset is proposed, and the geometry verification is self-contained (it compares against the manifest, not against the camera prototype). The measured handoff convention is **not** the one the camera verification documented, and the Maya report quantifies the difference per handoff. See below. |
| Deliver the sample, measurements, support list, reusable API and a minimal product plan; keep the texture-free scene reference independent of the existing animation export flow | Done, plus the pending decisions below. The prototype touches no product protocol, no animation export, and no package. |

## Evidence

| Check | Command shape | Result |
| --- | --- | --- |
| Unreal suite | `UnrealEditor-Cmd <ToolsLab.uproject> -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests MtoUSceneRefPrototype" -TestExit="Automation Test Queue Empty"` with `-MtoUSceneRefMayapy=`, `-MtoUSceneRefPeer=` and `-MtoUEvidence=` | 5 of 5 tests pass: `ScopeSemantics`, `ExportMainScope`, `TextureDetector`, `TextureProbe`, `RealMayaPeer` |
| Maya pure tests | `python -m unittest discover` and the same under `mayapy` over the Maya prototype tests | 85 tests pass under CPython and under mayapy 3.10.8 |
| Maya host checks | `mayapy tests/maya_host_scene_ref_tests.py --result <json>` | 169 checks, 0 failures, `ok: true` |
| Cross-host handoff | The `RealMayaPeer` test exports the fixture scope and runs the Maya importer/verifier over the produced file | `ok: true`, `phase: done`, no problems, 10/10 objects compared, no missing nodes, `file_texture_nodes: 0`, position `0.0 cm`, size `4.5e-13 cm`, offset `1.1e-13 cm`, surface centroid `9.3e-13 cm`, axis map `maya_x=ue_x, maya_y=ue_z, maya_z=ue_y` |
| Texture probe | Export the sublevel whose material samples a texture, then run the Maya importer over it | Exporter: `texture_records: 1`, `image_files: []`; Maya: exit 1 with `REFUSED TEXTURE_REFERENCES_PRESENT` naming the recorded file |
| OBJ probe | `MtoUSceneRef.Export obj out=<dir>`, once with `-NullRHI` and once with the renderer available | Null RHI: `EXCEPTION_ACCESS_VIOLATION` with the callstack in `UnrealEditor-MaterialBaking.dll`. With a renderer: `.obj` + `.mtl` + three baked `.bmp` files and `RunAssetExportTask` still returned failure |
| Repository gates | `python tools/validate_repository.py`, `python -m unittest discover -s tests` | Pass |

## The axis convention

Measured over the fixture's off-origin, rotated and non-uniformly scaled
objects, not assumed:

| Export option | Unreal world `(x, y, z)` in cm arrives in Maya as |
| --- | --- |
| `bForceFrontXAxis = false` (default) | `(x, z, y)` |
| `bForceFrontXAxis = true` | `(-y, z, x)` |

The camera verification documented `(y, z, -x)` for its own route, so the two
paths do not share a world: each measured map differs from it by a rotation, and
the Maya report quantifies it for every handoff (`transform_check.camera_contract`;
on the fixture handoff the camera map is `2404.16 cm` off in position and
`115.09 cm` in size). A product that wants one world has to apply one documented
conversion on one side. The prototype reports the mapping and corrects nothing.

## Support list

| Object kind | Result |
| --- | --- |
| `AStaticMeshActor` | Exported, world transform preserved (measured) |
| Instanced static mesh component | One child node per instance, named by index, instance transform preserved (measured, three instances) |
| Blueprint actor components | One child node per component when there is more than one (measured, two components) |
| Actor with a component that is not its root | Component transform merged into the node (measured) |
| Sublevel held by the loaded world | In scope when requested (measured) |
| Requested level the world does not hold | Reported, contributes nothing (measured). An editor world loads every sublevel it owns, so this is the editor's unloaded case |
| Unrequested sublevel | Reported as excluded, contributes nothing (measured) |
| World Partition cells | Not measured; not streamed-in content is absent from the world exactly like an unheld level |
| Landscape | Classified and carried as one object; the engine's own landscape branch is source-verified, no landscape was placed as a fixture |
| Level instance | Refused by the engine, reported with its reason |
| Nanite source mesh | Not exported (`bExportSourceMesh` off); LOD 0 render geometry is exported |
| Lights, cameras, emitters, brushes, volumes, world settings | Reported as skipped non-geometry |
| Materials, textures, lights, cameras as data | Not transferred. A texture-driven material makes the exporter write a texture record, which the importer refuses |

## Minimal product boundary proposed

- One scope per transfer: the loaded level plus the sublevels the caller names,
  resolved against the loaded world and reported in full.
- Geometry through the engine's own FBX level export with baking disabled, plus
  a manifest of the engine-evaluated world data; the Maya side owns the
  container, the gray material and the comparison.
- The texture rule is a check on the produced file on both hosts, with a refusal
  rather than a silent import.
- Explicitly outside: World Partition coverage, landscapes, Nanite source
  meshes, origin offsets, any correction of the axis convention, and any
  material, texture, light or camera transfer.

## Decisions that need revision

- The roadmap's shared world-space contract now has two measured conventions in
  the project. The scene reference and the camera route must be reconciled by an
  explicit conversion in one product component, or the artist will see two
  different worlds.
- If a scene reference is adopted, the `bForceFrontXAxis` choice becomes a
  product decision with a documented map, not an export detail.

## Pending user decisions

1. Whether the reference transfer should apply a correction so the imported
   level matches the camera route's world, or keep the engine's own convention
   and document it.
2. Which sublevel selection the product exposes: an explicit list, "everything
   the loaded world holds", or the current editor selection.
3. Whether a textured scope should block the transfer (current prototype
   behaviour) or warn and proceed with a gray reference.

## Delivered files

- `composite/MtoULiveLink/prototypes/scene-reference/` — contract, capability
  review, Unreal module (with Automation tests), Maya importer/verifier (with
  pure tests and host checks).
- `unreal/ToolsLab.uproject` — the prototype plugin directory and its enable
  entry, so the documented host project builds it.
- `composite/MtoULiveLink/docs/preview-workflow-roadmap.md` — the verification
  outcome recorded against the scene-reference direction.
