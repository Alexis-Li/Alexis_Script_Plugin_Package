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
   reason as rule 2. The prototype reports whether the world is partitioned and
   never claims cell-level coverage.
5. The scope never narrows to "the selected actors" and never widens to "every
   loaded level".

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
| `BakeMaterialInputs` | `Disabled` | No material input is rendered into a new image. This is an export setting, not evidence: the texture check below inspects the produced file. |
| `bASCII` | `true` | The prototype's own texture check reads the file, so the verification artifact stays inspectable. Binary output is a packaging choice for a product, not a prototype one. |
| `LevelOfDetail` | `0` | One LOD per mesh keeps the comparison unambiguous. |
| `Collision` | `false` | Reference geometry only. |
| `bExportSourceMesh` | `false` | Render geometry, not the source mesh description. |
| `bExportMorphTargets`, `bExportPreviewMesh` | `false` | Static reference geometry has neither. |
| `bForceFrontXAxis` | `false` | Keep the engine's own axis convention so the measured conversion is the engine's, not a prototype invention. |

## Texture rule

The handoff must not copy, package, or auto-load textures, and must not rely on
a flag for that claim. Therefore:

1. The exporter lists every file it produced and reports any image suffix
   (`.png`, `.bmp`, `.tga`, `.jpg`, `.jpeg`, `.exr`, `.hdr`, `.dds`, `.fbm`
   directory) it finds. A non-empty list is reported, never deleted silently.
2. The exporter scans the produced FBX text for material texture records and
   reports `output.texture_records` (every `Texture:` record) and
   `output.texture_references` (the records that name a file, which is what an
   import would try to load) with the recorded file names. Zero frames is not
   enough on its own: a record that names no file is still a material input the
   export could not carry, so both counts are reported and the Maya side refuses
   a handoff with either.
3. The Maya side re-scans the same file with its own detector before importing,
   so neither host trusts the other's count, and refuses to import when
   references are found unless `--allow-textures` is passed.
4. The Maya side reports the number of `file` texture nodes and imported
   shading networks that reference an image after import. The reference
   geometry ends up on one gray material created by the importer.
5. The Maya importer assigns that gray material to every imported mesh, and
   reports the material it created.

## Measured axis convention

The transfer preserves the level's own world space; it does not redefine it.
The convention the file round trip actually uses was measured, not assumed
(Maya 2024 + Maya 2024's `fbxmaya` 2020.3.4 reading an ASCII FBX from Unreal
5.7.4), over the fixture's off-origin, rotated and non-uniformly scaled objects:

| Export option | Unreal world `(x, y, z)` in centimetres maps to Maya |
| --- | --- |
| `bForceFrontXAxis = false` (engine default, the prototype's default) | `(x, z, y)` |
| `bForceFrontXAxis = true` (console command flag `frontx`) | `(-y, z, x)` |

Both were exact over every sample (residual `9.2e-13` cm and `5.7e-14` cm). The
camera verification's documented contract is `(y, z, -x)`
(`../camera-sync/maya/MtoUCameraSyncPrototype/scripts/mtou_camera_sync_mapping.py`),
so the two routes do **not** share a convention: each measured map differs from
it by a rotation, and the Maya report quantifies that difference per handoff
(`transform_check.camera_contract`). A product that wants one world has to apply
one documented conversion on one side; this prototype reports the mapping and
corrects nothing.

The file's own numbers carry the same story: a node at Unreal
`(1500, -800, 250)` with rotation `(roll 10, pitch 0, yaw 35)` is written as
`LclTranslation (1500, 800, 250)`, `LclRotation (10, 0, -35)`, and Maya reads it
back at `(1500, 250, -800)`.

## Object naming

The Maya importer places every imported root node under one transform called
`MtoU_UE_SceneRef` and inside the namespace of the same name, so a repeat run
replaces only its own container. Verification matches Unreal objects to Maya
nodes by the node name the engine wrote (actor label, or mesh name for a
multi-component actor, plus the instance node the FBX exporter created), with
the namespace prefix removed.

## Manifest schema

`mtou-scene-ref-manifest/1`. Written by Unreal; read by Maya.

```json
{
  "schema": "mtou-scene-ref-manifest/1",
  "generated_utc": "2026-09-28T12:00:00Z",
  "engine_version": "5.7.4",
  "world": {"package": "/Game/.../Map", "name": "Map", "up_axis": "Z", "linear_unit": "cm", "world_partition": false},
  "scope": {
    "kind": "level_range",
    "persistent_level": "/Game/.../Map",
    "requested_sublevels": ["/Game/.../Sub"],
    "loaded_sublevels": ["/Game/.../Sub"],
    "unloaded_sublevels": [{"package": "/Game/.../Other", "streaming_state": "not_loaded", "visible": false}],
    "excluded_sublevels": ["/Game/.../Third"]
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
    "texture_references": 0,
    "texture_reference_files": []
  },
  "timing": {"scope_seconds": 0.1, "export_seconds": 1.2, "inspect_seconds": 0.2},
  "memory": {"used_physical_mb": 1234.5}
}
```

`world_matrix` and the bounds are the values the engine evaluated **before**
the export, in Unreal world space (centimetres, Z-up). The Maya side compares
what the file produced against these numbers; no value in the manifest is a
converted or interpreted value.

`world_surface_centroid_cm` is the area-weighted centroid of the object's LOD 0
triangles, in world space: `sum(area x triangle centre) / sum(area)`. It exists
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
  "maya": {"version": "2024", "api": "20240200", "linear_unit": "cm", "up_axis": "y"},
  "container": {"namespace": "MtoU_UE_SceneRef", "group": "MtoU_UE_SceneRef", "replaced": false},
  "counts": {"manifest_objects": 7, "container_nodes": 7, "file_texture_nodes": 0, "image_nodes_loaded": 0},
  "gray_material": {"name": "MtoU_UE_SceneRef_Gray", "type": "lambert", "color": [0.5, 0.5, 0.5]},
  "transform_check": {
    "candidate": "maya_x=ue_y, maya_y=ue_z, maya_z=-ue_x",
    "candidates": [{"name": "...", "matrix": [9 floats], "max_position_error_cm": 0.0, "max_orientation_error_deg": 0.0, "max_size_error_cm": 0.0}],
    "best": "...", "matched": true, "max_position_error_cm": 0.0, "max_size_error_cm": 0.0
  },
  "objects": [
    {"node_name": "...", "path": "...", "matched_id": "...", "world_matrix": [16 floats], "world_bounds_size_cm": {"x": 0.0, "y": 0.0, "z": 0.0}, "position_error_cm": 0.0, "size_error_cm": 0.0, "offset_error_cm": 0.0, "centroid_error_cm": 0.0, "found": true}
  ],
  "texture_scan": {"source": "<file>", "texture_references": 0, "files": []},
  "timing": {"import_seconds": 0.0, "verify_seconds": 0.0},
  "memory": {"process_rss_mb": 0.0},
  "problems": []
}
```

## Commands

Exporter (Unreal, editor command line, console command surface):

```
MtoUSceneRef.Export <scope_name>=<level_package> [sublevel=<package> [sublevel=<package> ...]] [out=<dir>] [obj_probe]
```

Importer (Maya):

```
<mayapy> MtoUSceneRefPrototype.py --fbx <file> --manifest <file> --report <file> [--container <name>] [--allow-textures] [--keep-existing]
```

## Non-goals

- No animation, no skeletal meshes, no camera or light transfer: this reference
  is static geometry only. The camera route keeps its own contract.
- No reference origin offset. The prototype transfers the level's own world
  space; an offset would have to be shared with the camera and the animated
  objects, which is out of scope for this verification.
- No product protocol, package, or installation change.
