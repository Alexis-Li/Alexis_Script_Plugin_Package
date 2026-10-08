# Scene reference transfer contract (Issue 53 verification)

The contract between the Unreal exporter and the Maya importer. Both hosts
implement it; neither may read the other host's source or configuration.

This is verification scaffolding. It does not change the MtoULiveLink product,
its protocol (v9), its packages, or its supported versions. Geometry goes
`Unreal -> file -> Maya`; nothing here uses the product's streaming socket.

## Scope selection semantics

A transfer is described by a **scope**, not by the editor selection:

- `level`: the package name of the loaded level whose world owns the transfer.
  The persistent level is always part of the scope.
- `sublevels`: the package names of the streaming sublevels the caller asks for.

Rules the exporter must implement and report:

1. The scope is resolved against the level that is **currently loaded** in the
   editor world. A requested sublevel that the loaded world holds contributes its
   actors.
2. A requested sublevel that the loaded world does **not** hold contributes
   nothing and is reported in `scope.unloaded_sublevels` with its state
   (`not_in_world`). An editor world loads every streaming level it owns
   (`ULevelStreaming::DetermineTargetState` returns `LoadedNotVisible` for a
   non-game world), so in the editor the unloaded case is a level package that is
   absent from the loaded world — including World Partition cells that are not
   streamed in. The export never loads anything on demand and never silently
   drops a requested level.
3. A sublevel that exists in the world but was not requested is reported in
   `scope.excluded_sublevels` and contributes nothing.
4. Unloaded World Partition cells are outside the loaded actor set for the same
   reason as rule 2, and a `world_partition` flag alone does not establish
   coverage. The resolver therefore reads the partition's actor descriptors
   (read only: no content is loaded), and the manifest carries the result as
   `scope.coverage`, `scope.completeness` and `scope.world_partition`:
   `actor_descriptors`, `loaded_actor_descriptors`, `unloaded_actor_count`
   (authored actors the loaded world has not spawned), `unloaded_hlod_count`
   (generated HLOD proxies, which carry merged copies of authored content
   rather than content of their own), `unloaded_actors` (their paths, capped at
   `inventory_limit`) and `inventory_truncated`. `completeness` is `confirmed`
   when every authored descriptor is spawned in the loaded world and
   `not_confirmed` otherwise; a Maya run reports the unconfirmed case as a
   `SCOPE_LOADED_ONLY` warning instead of treating an empty
   `unloaded_sublevels` list as completeness.
5. The scope never narrows to "the selected actors" and never widens to "every
   loaded level".
6. The scope is **static reference geometry**: for every actor in scope the
   exporter turns the engine's FBX level export into a static-mesh-only transfer
   by suppressing the components that would otherwise become nodes (skeletal
   mesh, camera, light and child-actor components) for the duration of the
   export, and reports every suppressed component in `filter.suppressed_components`
   with its class and reason. A node-layout prediction (how many nodes an actor
   gets and what they are named) counts only the components that survive the
   filter.

## Output directory

The exporter writes every produced file into one directory it owns:

```
<out_dir>/<scope_name>.fbx            official FBX level export (the geometry handoff)
<out_dir>/<scope_name>.manifest.json  the measurement record described below
<out_dir>/<scope_name>.obj            only when the OBJ probe is requested
```

`<scope_name>` is derived from the level package. The directory is recreated per
run, so a repeat run cannot mix with the previous one.

## Geometry handoff

The `.fbx` file is produced by the engine's own level exporter
(`ULevelExporterFBX` through `UAssetExportTask`), driven by the editor
selection the exporter sets for the duration of the run and restores
afterwards. The prototype does not serialize meshes itself.

Options used, and why:

| Option | Value | Reason |
| --- | --- | --- |
| `BakeMaterialInputs` | `Disabled` | No material input is rendered into a new image. This is an export setting, not evidence: the media rule below inspects the produced file. |
| `bASCII` | `true` | Both hosts' own media check reads the file, so the verification artifact stays inspectable. Binary output is a packaging choice for a product, not a prototype one. |
| `LevelOfDetail` | `0` | One LOD per mesh keeps the comparison unambiguous. |
| `Collision` | `false` | Reference geometry only. |
| `bExportSourceMesh` | `false` | Render geometry, not the source mesh description. |
| `bExportMorphTargets`, `bExportPreviewMesh` | `false` | Static reference geometry has neither. |
| `bForceFrontXAxis` | `false` | Keep the engine's own axis convention so the measured conversion is the engine's, not a prototype invention. This is the option the node frame factor below was measured with; `frontx` exists for the comparison only. |

## Media rule

The material assignment a model carries is part of the reference and is kept:
a material ball, a material slot and a texture **record** are normal, and a
recorded path is a reference, not a delivered image. What the handoff must not
deliver is image **data**. Therefore:

1. The exporter lists every file it produced and reports any image suffix
   (`.png`, `.bmp`, `.tga`, `.jpg`, `.jpeg`, `.exr`, `.hdr`, `.dds`, `.fbm`
   directory) it finds in its own directory. A non-empty list is a failure of the
   media rule and is reported, never deleted silently.
2. The exporter scans the produced FBX text and reports the records it holds:
   `output.texture_records` (every `Texture:` record), `output.texture_references`
   (records that name a file), `output.video_references`, `output.content_records`
   (every `Content:` record) and `output.embedded_media_records` (records whose
   `Content:` line carries media data). Only the last two, and the image file
   list, are media data; the texture counts are information the report keeps for
   the record.
3. The Maya side re-scans the same file with its own detector before importing,
   lists the image files in the handoff's own directory itself, and refuses to
   import a handoff that delivers image data unless `--allow-image-data` is
   passed. It reports the recorded texture paths and whether they resolve to a
   file on this machine (`media.image_paths_present`), which is a reference that
   resolves, not an image the handoff delivered.
4. The Maya side reports the `file` texture nodes the import created
   (`counts.file_texture_nodes`), how many of them name an image that exists on
   disk (`counts.image_nodes_loaded`), and the material assignment each imported
   shape holds (the shading groups the file assigned, `materials.preserved_assignment`).
   A run never claims "no texture was loaded" from the material it finally shows.
5. The reference display is separate from the media question: `--shading display`
   (the default) leaves the imported materials in place and colors the viewport
   with a uniform gray override, `--shading material` assigns a gray lambert the
   importer creates (`MtoU_UE_SceneRef_Gray`), and `--shading keep` changes
   nothing. The choice never removes a `file` node or a texture record from the
   handoff and is always reported.

### Shared record rule

Both hosts walk the FBX text with the same rule, so a disagreement in the counts
is a finding rather than a formatting difference:

- a line whose trimmed text starts with `Texture:` opens a texture record,
  `Video:` opens a video record, and the record ends only when the brace depth
  returns to zero (the SDK writes a nested `Properties70` block before the name
  and `Content` lines, so a nested closing brace ends nothing);
- inside a record, `FileName:`, `Filename:` and `RelativeFilename:` record the
  last quoted token on the line;
- inside a record, a `Content:` line is an embedded media record when it carries
  data on the same line or when the next non-empty line starts with a quoted
  payload (measured on this host's `fbxmaya` 2020.3.4: `Content: ,` followed by
  the base64 payload on the next line, and no `Content:` line at all for media
  that is not embedded);
- a `NodeAttribute:` line whose quoted tokens include the class `Camera` or
  `Light` is a camera or light node record. The SDK does **not** write `Camera:`
  or `Light:` record heads, so a detector that counts those finds nothing.

## Worlds and the node matrix

The transfer preserves the level's own world space; the geometry is never
rescaled and never moved to the origin. Two Maya worlds exist in this project,
and the handoff relates to them explicitly.

### The point maps

Measured, not assumed (Maya 2024 + `fbxmaya` 2020.3.4 reading an ASCII FBX from
Unreal 5.7.4), over the fixture's off-origin, rotated and non-uniformly scaled
objects:

| Export option | Unreal world `(x, y, z)` in centimetres maps to Maya |
| --- | --- |
| `bForceFrontXAxis = false` (engine default, the prototype's default) | `(x, z, y)` |
| `bForceFrontXAxis = true` (console command flag `frontx`) | `(-y, z, x)` |

The camera sync prototype documents `(y, z, -x)` for its own route. All three
maps have determinant `-1`, so each pair differs by a rotation.

### The node matrix, not just the point

A point map is not enough to describe what the file holds. The exporter writes
every node in the file's own local frame, so the node matrix Maya reads back is

```
M_maya = frame . L_ue . map            (measured: every node of the fixture handoff)
```

with `frame = ((1,0,0),(0,-1,0),(0,0,1))` for `bForceFrontXAxis = false`. The
factor was measured on 2026-09-29 over the ten object fixture handoff: every
measured node matrix matches `frame . L_ue . map` to `1.3e-15`, while the naive
`L_ue . map^T` is off by up to `7.7`. It is a mirror in the file's second axis
(the up-axis conversion the Maya reader applies to each node's local space) and
it is why the node matrix has a positive determinant while the point map does
not. A host that compares orientations against `L_ue . map^T` compares against a
matrix the file never contains.

### The two worlds the importer can place the geometry in

| `--target-world` | Result |
| --- | --- |
| `engine` (default) | The geometry stays in the handoff's own world, which is also the world of the product's Maya to Unreal animation route: `MtoULiveLink.py`'s `convert_transform` maps `maya (x, y, z) -> ue (x, z, y)`, its own inverse and the same map as `map` above (checked 2026-09-29). A level reference imported this way agrees with the character and prop animation the artist sends to Unreal. |
| `camera` | Every imported root's world matrix is carried through one explicit map, `conversion = camera_map . engine_map^-1` (determinant `+1`, a 90 degree rotation about Maya's up axis), so the reference lands in the camera sync route's world. The rotation is applied to the root transforms only; the geometry below follows. |

The conversion is one explicit operation, reported with its matrix, its
determinant, its rotation angle and the number of roots it moved. Whichever world
is asked for, the comparison reports:

- `transform_check.candidate` — the point map the positions really arrived in;
- `transform_check.engine_check` (when a conversion was applied) — a full
  comparison measured **before** the conversion, which is what proves the file
  arrived in the handoff's world;
- `transform_check.node_frame_factor` — the frame factor used for the
  orientation comparison, or `null` when the manifest declares no measured
  export axis option;
- `transform_check.camera_contract` — how the camera route's map scores on this
  handoff, so the divergence is on the record.

## Object naming and the container

The Maya importer places every imported root node under one transform called
`MtoU_UE_SceneRef` and inside the namespace of the same name, so a repeat run
replaces only its own container. Verification matches Unreal objects to Maya
nodes by the node name the engine wrote (actor label, or mesh name for a
multi-component actor, plus the instance node the FBX exporter created), with
the namespace prefix removed.

The container group carries an ownership mark: a string attribute
`mtouSceneRefContainer` whose value is `mtou-scene-ref-container/1 <name>`, the
name being the namespace the group was created for. The mark is what makes a
container replaceable — a namespace, group or root-level node of the container's
name that does not carry it is refused (`CONTAINER_NOT_OWNED`, exit 1) instead
of being emptied, and a top-level transform of another type inside an owned
namespace is refused the same way. The staging namespace is the first free name
of the `<container>_Incoming`, `<container>_Incoming_1`, ... family, so a
namespace of that name that belongs to the scene is neither reused nor deleted;
the namespace a takeover moves the previous reference to uses the same rule with
`<container>_Retiring`.

## Updating the reference

An update never leaves a half-replaced reference behind, and it never deletes
the previous reference before the new one has taken its place:

1. The handoff is imported into a **staging namespace** — the first free name of
   the `<container>_Incoming`, `<container>_Incoming_1`, ... family, with a
   group of the same name inside it. The previous reference is untouched while
   the new import is measured, and the staging namespace is what makes a failed
   run harmless. This run owns that namespace from the moment it exists: every
   later failure deletes it again and reports the nodes it removed.
2. The comparison runs in the staging namespace. Any problem — a mismatch, a
   missing node, a refused handoff, a partial import, an exception — deletes the
   staging namespace again and keeps the previous reference exactly as it was
   (`update.mode = "staged_swap_discarded"`, `update.discarded = true`, with the
   discarded node names).
3. Only when the comparison reported no problem is the previous, owned container
   **renamed** into a free `<container>_Retiring`, ... name — renamed, not
   deleted, so the scene keeps the very node it had, with its UUID — and the
   staging namespace takes the container's name with its group renamed to the
   group name the contract promises and its ownership mark rewritten for that
   name.
4. Every recorded path is then re-resolved by short name inside the final
   container (`update.post_swap_paths_checked`, `update.post_swap_paths_missing`).
   Only when none is missing is the retired reference deleted
   (`update.retired_removal`).
5. A failure in step 3 or a missing path in step 4 **rolls the takeover back**:
   the container this run created is deleted, the retired reference is renamed
   back under the container's name, and the report says so
   (`update.rolled_back`, `update.rollback`, `update.rollback_reason`).
6. `--dry-run` stages and verifies without swapping anything.
7. `container.kept_existing` and `container.previous_reference_present_after_run`
   are read from the scene at the end of the run — the previous reference is
   looked up by UUID — so the recovery report states what the scene holds rather
   than what the run intended. `container.after_run` names the group path the
   container has now, any unmarked group or foreign node found under that name,
   and whether a staging namespace is still there.
8. An interrupted run is recovered before the next one proceeds: a marked
   staging namespace is deleted, a reference left in a retiring name is renamed
   back to the container's name (or finished into one when no retired copy is
   waiting), and both are reported in `update.recovery`.
9. Only nodes carrying this tool's ownership mark, and the staging and retiring
   namespaces this tool created, are ever deleted. Nothing else in the scene is
   renamed, reparented, reassigned or deleted.

## Manifest schema

`mtou-scene-ref-manifest/1`. Written by Unreal; read by Maya.

```json
{
  "schema": "mtou-scene-ref-manifest/1",
  "generated_utc": "2026-09-29T12:00:00Z",
  "engine_version": "5.7.4",
  "world": {"package": "/Game/.../Map", "name": "Map", "up_axis": "Z", "linear_unit": "cm", "world_partition": false},
  "conventions": {
    "handoff": "engine_fbx_level_export",
    "export_axis_option": "bForceFrontXAxis=false",
    "engine_to_maya_point_map": "maya_x=ue_x, maya_y=ue_z, maya_z=ue_y",
    "engine_to_maya_determinant": -1
  },
  "filter": {
    "policy": "static_mesh_components_only",
    "suppressed_count": 2,
    "suppressed_components": [
      {"actor": "BP_SceneRefMixed", "actor_class": "BP_SceneRefMixed_C",
       "component": "PointLight", "class": "PointLightComponent",
       "reason": "light component", "level": "/Game/.../Map"}
    ]
  },
  "scope": {
    "kind": "level_range",
    "persistent_level": "/Game/.../Map",
    "coverage": "loaded_levels",
    "completeness": "confirmed",
    "coverage_note": "the world is not partitioned: every requested sublevel is either loaded and traversed or listed as unloaded",
    "requested_sublevels": ["/Game/.../Sub"],
    "loaded_sublevels": ["/Game/.../Sub"],
    "unloaded_sublevels": [{"package": "/Game/.../Other", "streaming_state": "not_in_world", "visible": false}],
    "excluded_sublevels": ["/Game/.../Third"],
    "world_partition": {
      "detected": false,
      "inventory_available": false,
      "containers": 0,
      "actor_descriptors": 0,
      "loaded_actor_descriptors": 0,
      "unloaded_actor_count": 0,
      "unloaded_hlod_count": 0,
      "inventory_limit": 200,
      "inventory_truncated": false,
      "unloaded_actors": [],
      "note": "..."
    }
  },
  "scale": {"actors": 5, "components": 6, "objects": 7, "triangles": 1234, "vertices": 987},
  "objects": [
    {
      "id": "static_mesh_actor:SM_Pillar_Offset:component:StaticMeshComponent0:instance:none",
      "node_name": "SM_Pillar_Offset",
      "actor": "SM_Pillar_Offset",
      "actor_class": "StaticMeshActor",
      "level": "/Game/.../Map",
      "category": "static_mesh",
      "component": "StaticMeshComponent0",
      "mesh": "/Engine/BasicShapes/Cube.Cube",
      "instance_index": null,
      "exported": true,
      "note": "",
      "materials": ["/Engine/BasicShapes/BasicShapeMaterial"],
      "triangles": 12,
      "vertices": 8,
      "world_location_cm": {"x": 0.0, "y": 0.0, "z": 0.0},
      "world_rotation_deg": {"roll": 0.0, "pitch": 0.0, "yaw": 0.0},
      "world_scale": {"x": 1.0, "y": 1.0, "z": 1.0},
      "world_matrix": [16 row-major floats, Unreal world space],
      "world_bounds_cm": {"min": {"x": 0.0, "y": 0.0, "z": 0.0}, "max": {"x": 0.0, "y": 0.0, "z": 0.0}, "size": {"x": 0.0, "y": 0.0, "z": 0.0}},
      "world_surface_centroid_cm": {"x": 0.0, "y": 0.0, "z": 0.0}
    }
  ],
  "unsupported": [{"actor": "LI_House", "class": "LevelInstance", "reason": "..."}],
  "skipped": [{"actor": "SkyLight", "class": "SkyLight", "reason": "not_static_geometry"}],
  "output": {
    "directory": "<handoff>",
    "geometry_file": "Map.fbx",
    "geometry_bytes": 123456,
    "files": [{"name": "Map.fbx", "bytes": 123456}],
    "image_files": [],
    "texture_records": 0,
    "texture_references": 0,
    "video_references": 0,
    "content_records": 0,
    "embedded_media_records": 0,
    "embedded_media": [],
    "camera_records": 0,
    "light_records": 0,
    "texture_reference_files": [],
    "texture_reference_files_present": [],
    "node_names": ["SM_Pillar_Offset"]
  },
  "timing": {"scope_seconds": 0.1, "export_seconds": 1.2, "inspect_seconds": 0.2},
  "memory": {"used_physical_mb": 1234.5},
  "warnings": []
}
```

`world_matrix` and the bounds are the values the engine evaluated **before**
the export, in Unreal world space (centimetres, Z-up). The Maya side compares
what the file produced against these numbers; no value in the manifest is a
converted or interpreted value.

`world_surface_centroid_cm` is the area-weighted centroid of the object's LOD 0
triangles, in world space: `sum(area x triangle centre) / sum(area)`. A landscape
object is measured from the geometry the engine's landscape branch writes — the
`FLandscapeComponentDataInterface` vertices of each landscape component at
`ALandscapeProxy::ExportLOD`, two triangles per quad, with the component's
relative location and the actor transform applied — so its bounds, centroid,
triangle and vertex counts describe the file rather than the actor's own
collision/editor bounds. It exists
because position, bounds size and bounds offset are all blind to a mirrored
placement, while this vector is not. It is deliberately triangle based: the
engine's FBX exporter does not write the render vertex buffer's vertex set
(measured: 198 positions in the render buffer against 144 in the exported cone
mesh, with the same 284 triangles), so only a quantity derived from the
triangles is comparable on both hosts. The Maya side computes the same number
from the faces it holds, and an older manifest without the field reports the
check as unavailable rather than failing it.

## Maya report schema

`mtou-scene-ref-report/1`. Written by Maya; the only thing the Unreal side reads
back.

```json
{
  "schema": "mtou-scene-ref-report/1",
  "ok": true,
  "phase": "done",
  "maya": {"version": "2024", "api": "20240200", "linear_unit": "cm", "up_axis": "y", "time_unit": "film", "plugins": {"fbxmaya": "2020.3.4"}},
  "manifest": {"schema": "...", "world": {}, "conventions": {}, "filter": {}, "scope": {}, "scale": {}, "objects_exported": 10, "objects_total": 10, "output": {}},
  "scene": {"linear_unit": "cm", "up_axis": "y", "time_unit": "film", "playback_range": {}, "current_time": 1.0, "namespaces": [], "import_side_effects": {}, "playback_range_unchanged": true, "current_time_unchanged": true},
  "fbx": {"file": "...", "bytes": 0, "format": "ascii", "plugin": "fbxmaya", "import_options": "v=0;", "namespace_flag": "MtoU_UE_SceneRef_Incoming", "root_nodes_parented": []},
  "container": {"namespace": "MtoU_UE_SceneRef", "group": "MtoU_UE_SceneRef", "group_path": "|MtoU_UE_SceneRef:MtoU_UE_SceneRef", "staging_namespace": "MtoU_UE_SceneRef_Incoming", "staging_group_path": "...", "staging_group_uuid": "...", "previous_existed": false, "previous_container_group": null, "previous_reference_uuid": null, "ownership_attribute": "mtouSceneRefContainer", "ownership_token": "mtou-scene-ref-container/1", "swapped": true, "kept_existing": false, "namespace_created": true, "after_run": {"namespace": true, "group_path": "|MtoU_UE_SceneRef:MtoU_UE_SceneRef", "incomplete_groups": [], "foreign_nodes": [], "staging_namespace_present": false}, "previous_reference_present_after_run": null, "previous_reference_path_after_run": null},
  "update": {"mode": "staged_swap", "staging_namespace": "...", "existing_container": false, "swapped": true, "discarded": false, "stale_staging_removed": false, "stale_staging_namespaces": [], "discarded_nodes": [], "swap_seconds": 0.1, "post_swap_paths_checked": true, "post_swap_paths_missing": [], "retired": null, "retired_removal": null, "rolled_back": false, "rollback_reason": null, "recovery": {"stale_staging_namespaces": [], "restored_previous_reference": null, "retired_leftovers_removed": [], "interrupted_container_removed": false, "finished_interrupted_container": null, "events": []}},
  "counts": {"manifest_objects": 10, "manifest_objects_total": 10, "container_nodes": 30, "file_texture_nodes": 0, "image_nodes_loaded": 0, "meshes_assigned": 8, "matched_objects": 10, "matched_by_name": 7, "matched_by_transform_count": 3},
  "media": {"handoff_directory": "...", "image_files_in_handoff_directory": [], "embedded_media_records": 0, "embedded_media": [], "content_records": 0, "texture_records": 0, "texture_references": 0, "camera_records": 0, "light_records": 0, "media_heuristic": false, "file_nodes_created": 0, "image_nodes_loaded": 0, "image_paths_present": [], "allowed": false},
  "texture_scan": {"source": "<file>", "format": "ascii", "texture_records": 0, "texture_references": 0, "video_references": 0, "content_records": 0, "embedded_media_records": 0, "embedded_media": [], "camera_records": 0, "light_records": 0, "files": [], "allowed": false},
  "world": {"target": "engine", "target_map": "maya_x=ue_x, maya_y=ue_z, maya_z=ue_y", "target_note": "...", "conversion_applied": false, "conversion_matrix": null, "conversion_determinant": null, "conversion_rotation_deg": null, "conversion_seconds": null, "objects_converted": 0, "manifest_conventions": {}, "engine_check": null},
  "materials": {"mode": "display", "gray_material": null, "display": {"color": [0.5, 0.5, 0.5], "shapes_overridden": 8, "shapes": [], "note": "..."}, "preserved_assignment": [{"shape": "...", "shading_groups": []}], "imported_material_nodes": {}, "file_nodes": []},
  "transform_check": {
    "candidate": "maya_x=ue_x, maya_y=ue_z, maya_z=ue_y",
    "candidates": [{"name": "...", "matrix": [9 floats], "max_position_error_cm": 0.0, "max_size_error_cm": 0.0, "max_offset_error_cm": 0.0, "max_centroid_error_cm": 0.0, "max_orientation_error_deg": 0.0, "orientation_objects": 2}],
    "best": "...", "matched": true, "decided_by": "candidate", "winner": {},
    "node_frame_factor": [9 floats], "world_conversion_matrix": null,
    "orientation_available": true, "orientation_note": "...",
    "max_position_error_cm": 0.0, "max_size_error_cm": 0.0,
    "max_offset_error_cm": 0.0, "max_centroid_error_cm": 0.0,
    "max_orientation_error_deg": 0.0,
    "fit": {}, "camera_contract": {}, "tolerances": {}, "objects_compared": 10, "objects_missing": 0
  },
  "objects": [
    {"node_name": "...", "path": "...", "matched_id": "...", "category": "static_mesh", "found": true, "matched_by": "name",
     "world_matrix": [16 floats], "world_bounds_size_cm": {}, "local_bounds_size_cm": {},
     "identification_size_cm": {}, "position_error_cm": 0.0, "size_error_cm": 0.0,
     "offset_error_cm": 0.0, "centroid_error_cm": 0.0, "orientation_error_deg": null,
     "orientation_checked": false, "centroid_checked": true, "problems": []}
  ],
  "timing": {"staging_seconds": 0.0, "import_seconds": 0.0, "verify_seconds": 0.0, "total_seconds": 0.0},
  "memory": {"process_rss_mb": 0.0, "process_rss_available": true, "source": "..."},
  "problems": [],
  "warnings": []
}
```

`transform_check.candidate` names the point map the positions arrived in; the
orientation comparison is anchored to the handoff's own convention
(`frame . L_ue . map`, with `node_frame_factor` reported) and is reported as
unavailable for an object whose identification size does not have three distinct
extents, or for a handoff whose export axis option was never measured.

### What the counts mean

`counts.container_nodes` is every node the container namespace holds;
`counts.meshes_assigned` is the number of mesh **shapes** in it (a shape shared
by several instances of one mesh is one shape and is counted once);
`counts.file_texture_nodes` is the `file` nodes the import created and
`counts.image_nodes_loaded` is how many of them name an image that exists on this
machine; `counts.matched_objects`, `counts.matched_by_name` and
`counts.matched_by_transform_count` describe how the manifest objects were
matched. The exporter's `scale` block counts objects, triangles and vertices
*before* the export, so it can differ from what the file holds after the engine
welded duplicated render vertices away.

## Commands

Exporter (Unreal, editor command line, console command surface):

```
MtoUSceneRef.Export [textured] [obj] [frontx] [out=<dir>]
MtoUSceneRef.ExportLevel <level_package> [sublevel=<package> ...] [out=<dir>] [frontx] [obj]
MtoUSceneRef.Peer <mayapy> <peer script> [out=<dir>] [scope=<name>] [world=camera] [shading=material|keep] [dryrun] [allowimagedata]
MtoUSceneRef.BuildFixture
```

Importer (Maya):

```
<mayapy> MtoUSceneRefPrototype.py --fbx <file> --manifest <file> --report <file>
    [--container <name>] [--target-world engine|camera]
    [--shading display|material|keep] [--allow-image-data] [--dry-run] [--json]
```

## Non-goals

- No animation, no skeletal meshes, no camera or light transfer: this reference
  is static geometry only, and the exporter suppresses the components that would
  otherwise carry them. The camera route keeps its own contract.
- No reference origin offset. The prototype transfers the level's own world
  space; an origin offset would have to be shared with the camera and the
  animated objects, which is out of scope for this verification.
- No product protocol, package, or installation change.
