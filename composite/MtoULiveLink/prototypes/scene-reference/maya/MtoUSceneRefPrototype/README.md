# MtoU scene reference prototype: Maya side

Importer and verifier for the bounded Unreal -> Maya static geometry reference
described in `../../transfer.md`. The Unreal side exports the static geometry of
one level scope as an ASCII FBX plus a `mtou-scene-ref-manifest/1` measurement
record; this side imports that file into one container, puts the reference
geometry on a single gray material, and compares what Maya actually holds
against the manifest so neither host is trusted about the result.

`transfer.md` is the frozen contract; this folder implements the Maya half of
its "Maya report schema", "Texture rule", "Object naming" and "Commands"
sections and never changes them. Nothing here imports or depends on the
product's `MtoULiveLink.py`.

## Files

| Path | Contents |
| --- | --- |
| `scripts/mtou_scene_ref_mapping.py` | Pure mapping: the FBX texture detector, the manifest reader, the axis candidates, the world-position fit and the per-object comparison. No Maya imports, standard library only. |
| `scripts/MtoUSceneRefPrototype.py` | Maya importer, gray material assignment, comparison and the `main()` entry point. |
| `tests/test_scene_ref_mapping.py` | Pure unit tests; runs under CPython 3 and under mayapy. |
| `tests/maya_host_scene_ref_tests.py` | mayapy host checks in a disposable scene, with a JSON evidence file. |
| `tests/__init__.py` | Makes the folder importable for unittest discovery. |

## Running

`mayapy` is Maya's standalone Python interpreter. It is not on `PATH` by
default; point the commands at your Maya installation, for example through a
`MAYAPY` environment variable (`"$MAYAPY"` below).

```bash
# 1. pure mapping tests (any CPython 3, and the same under mayapy)
python -m unittest discover \
  -s composite/MtoULiveLink/prototypes/scene-reference/maya/MtoUSceneRefPrototype/tests \
  -t composite/MtoULiveLink/prototypes/scene-reference/maya/MtoUSceneRefPrototype

# 2. mayapy host checks (disposable scene, JSON evidence, nothing saved)
"$MAYAPY" composite/MtoULiveLink/prototypes/scene-reference/maya/MtoUSceneRefPrototype/tests/maya_host_scene_ref_tests.py \
  --result host_result.json
#    ... add --keep-scratch to keep the fixture FBX files and their manifests

# 3. one handoff, as the Unreal side calls it
"$MAYAPY" composite/MtoULiveLink/prototypes/scene-reference/maya/MtoUSceneRefPrototype/scripts/MtoUSceneRefPrototype.py \
  --fbx <scope>.fbx --manifest <scope>.manifest.json --report maya_report.json
```

Exit codes are the contract's: `0` when every check passed, `1` when a check
failed (including a handoff that records a texture and was not given
`--allow-textures`), `2` for usage and contract errors (an unreadable or non-FBX
handoff, a manifest that is not `mtou-scene-ref-manifest/1`, a scene or manifest
that is not in centimetres, a manifest world that is not Z up, an unusable
container name). `--json` prints the whole report to stdout; without it the run
prints a short summary.

## What a run does

1. Reads the manifest and reports the scope, the exported object count and the
   geometry scale **before** anything is imported.
2. Scans the FBX text with its own detector and refuses to import a handoff that
   records a texture unless `--allow-textures` was passed.
3. Creates the container namespace and group, imports the FBX inside that
   namespace, and parents every imported root node under the group with
   `cmds.parent(..., relative=True)` so the world placement is preserved.
4. Creates one gray lambert (`MtoU_UE_SceneRef_Gray`, mid gray `0.5`) and
   assigns it to every mesh shape in the container, reporting how many shading
   groups it replaced and which imported material and texture nodes are left.
5. Matches every exported manifest object to its Maya node, reads the node's
   world matrix and world bounding box, and compares them with the manifest.

## Container naming, and the one host limit

The container is a namespace called `MtoU_UE_SceneRef` and a transform of the
same name. On this Maya version a namespace and a root level node **cannot share
a name** (`cmds.createNode(..., name="X")` silently renames the node to `X1`
when namespace `X` exists, and `cmds.namespace(add="X")` fails when a node `X`
exists), so the group lives *inside* its own namespace and its full name is
`|MtoU_UE_SceneRef:MtoU_UE_SceneRef`. The report carries both parts plus the
group's full path.

A repeat run deletes only that namespace and that group before importing again
(`cmds.namespace(removeNamespace=..., deleteNamespaceContent=True)` and the
group node), so any other scene object, material or key survives untouched.
`--keep-existing` skips the removal; the FBX plugin then merges into the names
that are already there, and the report shows the resulting counts.

What this host's `fbxmaya` plugin actually accepts is recorded in the report:

* `options="v=0;"` is what the contract names, but this plugin ignores the
  options string completely: an empty string and a deliberately bogus
  `bogus=1;` import identically, with no error.
* the `ns=<container>` flag does **not** place imported nodes in the namespace
  on this host. Setting the *current* namespace
  (`cmds.namespace(set=<container>)`) around the import is what puts every
  imported node -- transforms, shapes, materials and file nodes -- inside it, so
  the importer does that instead and reports
  `fbx.namespace_mechanism`.

## Texture rule

Both hosts run the same frozen, line based detector so their counts can be
compared; `tests/test_scene_ref_mapping.py` holds the positive and negative
controls (an engine style fragment, a nested property block, a take clip, the
scene's own `Original|FileName` property, a commented out record and the
`Definitions` template):

* a line whose trimmed text starts with `Texture:` opens a texture record, one
  starting with `Video:` opens a video record, and a line that is exactly `}`
  closes the current record;
* inside a record, `FileName:` and `RelativeFilename:` name a file, taking the
  last quoted token on the line.

The scan reports `texture_records` (every `Texture:` record), `texture_references`
(records that named a file), `video_references`, the distinct `files` in the
order found, and the per record evidence. **The import refusal is based on
`texture_records > 0`**, not only on `texture_references`: the SDK writes a
nested `Properties70` block before the name lines, so a record can carry a
texture and still name no file that this rule sees -- as both of this host's
own exporter forms do. A texture record that names no file is still a material
input that cannot be carried.

`--allow-textures` imports anyway, for inspection. Surviving `file` nodes inside
the container are then reported as warnings and counted in
`counts.file_texture_nodes` and `counts.image_nodes_loaded`; without the flag
the same finding would be a problem. Texture nodes are never deleted silently,
and neither is any other node.

A binary FBX cannot be read this way. The scan falls back to a documented
heuristic over the FBX string table (node names are length prefixed, so a
`FileName` that belongs to a `Texture` node is found and the property template
tokens and take clips are rejected), reports `binary: true` and says in
`texture_scan.note` that a clean binary scan is not proof. The contract's own
handoff is ASCII.

## Comparison rules worth knowing

* Every candidate in `mtou_scene_ref_mapping.AXIS_CANDIDATES` maps Unreal world
  points to Maya world points, and the run reports the maximum position error of
  each one, so the axis convention the handoff really used is measured rather
  than assumed. Candidates: the engine handoff's
  `maya_x=ue_x, maya_y=ue_z, maya_z=ue_y`, the camera sync prototype's
  `maya_x=ue_y, maya_y=ue_z, maya_z=-ue_x`, the classic Z-up to Y-up
  `maya_x=ue_x, maya_y=ue_z, maya_z=-ue_y`, the mirror of the camera map, and an
  identity baseline.
* The engine's FBX level export and the camera route do **not** use the same
  convention. Measured on 2026-09-28 (Unreal 5.7.4 into Maya 2024), an Unreal
  world point `(x, y, z)` in centimetres arrives at `(x, z, y)`: the file writes
  the node translation with Y negated and Maya converts the file's Z-up axis
  system to Y-up. Its determinant is `-1`, so it is a mirror of the camera
  route's `(y, z, -x)` map. `transform_check.camera_contract` scores that camera
  map on the same handoff and states the divergence, and nothing here corrects
  the imported geometry: a product that carries both routes has to reconcile
  them.
* The least-squares fit of `maya = M . ue` over the matched positions is
  reported with its determinant, its deviation from a signed permutation and its
  residual. A determined fit needs at least four non-coplanar matched objects;
  with fewer the run says so and falls back to the named candidates alone.
* The comparison decides with a named candidate that places every object inside
  tolerance, so `best` names the measured convention; only when no named
  candidate does does the fitted signed-permutation map decide, and then `best`
  is `fitted signed permutation` with the fit block carrying the numbers.
  `transform_check.decided_by` says which decided and `transform_check.winner`
  carries the matrix the per-object comparison used. The per-object comparison
  and the transform matching below both use that winner, so a handoff that is
  consistent under one map reports no position or size problem even when that
  map is not one of the named candidates, and no correction is applied to the
  geometry.
* World matrices are read and reported in the order both hosts store them: the
  first three rows are the images of the object's local `X`, `Y` and `Z` axes
  and the fourth row is the translation (Unreal's `FMatrix` and Maya's
  `xform -m` both write this). Positions, sizes and axis images are all carried
  through the same candidate, so a comparison is one consistent story.
* Per object: position error, world bounding box size error, pivot to
  bounding-box-centre offset error, world vertex centroid error and orientation
  error, each against an explicit tolerance (1 cm, 1 cm, 1 cm, 1 cm and 0.5
  degrees by default, reported in `transform_check.tolerances`). A mismatch
  names the object, the expected value and the measured value.
* Two of those checks exist because a mirrored arrival is otherwise invisible.
  Every sample mesh in the engine's own fixture is symmetric -- a cube, a
  cylinder, a sphere -- and a mirrored placement leaves a symmetric mesh's
  position and axis-aligned box size exactly where they were, so position and
  size cannot see it. The **offset** (`objects[].offset_error_cm`) is the vector
  from the node's world position to the centre of its world bounding box: a
  mirror moves it for any mesh whose box is not centred on its pivot. The
  **surface centroid** (`objects[].centroid_error_cm`) is the area-weighted
  centroid of the mesh's triangles in world space,
  `sum(area * triangle_centre) / sum(area)`: a mirror moves it for any asymmetric
  mesh, including one whose box *is* centred on its pivot, such as a cone. Both
  are compared against the manifest's own values carried through the winning
  map, both are checked even where the orientation check is skipped, and either
  one failing is a problem (`OFFSET_MISMATCH`, `CENTROID_MISMATCH`) that makes
  `transform_check.matched` false.
* The surface centroid is area weighted and triangle based on purpose: the
  engine's exporter writes the mesh it triangulates, and it welds duplicated
  render vertices away, so its triangle count is the one number both hosts
  really share (the engine's cone LOD 0 has 198 render vertices against 144 in
  the exported mesh, with the same triangles). A *vertex* average would compare
  two different vertex sets; the triangle soup is what is comparable. On the
  Maya side every face is fan-triangulated from its first vertex and each
  triangle contributes `area * (a + b + c) / 3`, matching the engine's formula.
* The centroid read is capped: at most 20000 faces per object are accumulated. A
  mesh at or below the cap is read whole; a larger one is sampled evenly and
  reports `centroid_sampled`. Each object also reports `centroid_faces_used` and
  `centroid_triangles_used`, so a sampled reading is visible rather than
  implied, and no handoff can turn this check into a hang. A manifest written
  before this field existed carries no `world_surface_centroid_cm`; the check
  then reports itself unavailable for that object (`centroid_checked: false`)
  with a `CENTROID_UNAVAILABLE` warning instead of failing it.
* Orientation is only compared where the mesh has an unambiguous axis: a mesh
  whose local bounding box has three distinct extents. For a cube or a
  two-by-two symmetric mesh a symmetry rotation can produce the same geometry
  with a different node matrix, so the object reports
  `orientation_checked: false` instead of a false pass.
* Node matching is by short name first -- the namespace prefix removed, Maya's
  numeric deduplication suffix tolerated -- and reports `matched_by: "name"`.
  An object whose name did not survive is matched by world position under the
  comparison's winning map and reports `matched_by: "transform"` with its
  distance. This is the instanced case: the engine names an instanced mesh's
  children `0`, `1`, `2`, and this host's FBX plugin escapes such a purely
  numeric name to `FBXASC048`, `FBXASC049`, `FBXASC050`, so the name can never
  match and only the position can. A node that matches neither way is a reported
  problem, never a skipped comparison.
* The exporter's own counts are cross-checked, never trusted: a disagreement
  about `texture_records`, `texture_references` or `video_references`, image
  files the exporter listed, or manifest node names missing from
  `output.node_names` are reported as warnings.

## Scene safety

* The scene is never saved, no key is created, and the playback range, the frame
  rate, the linear unit and the current time are never moved of the importer's
  own accord. The only writes to them are the restores described next.
* `cmds.file` importing an FBX **does** move Maya's current time to the imported
  take on this host. The importer captures the session state before the import,
  restores the frame rate, playback range and current time afterwards, and
  records every difference it had to undo in `scene.import_side_effects`.
* Only the container namespace and its group are created, replaced or deleted.
  Nothing outside the container is renamed, reparented, reassigned or deleted.
* The gray material is created once in the root namespace and reused by later
  runs. If a node of that name exists and is not a lambert, the run refuses
  instead of renaming or replacing an object it did not create.
* A handoff that records a texture is not imported without `--allow-textures`.
* `main()` never raises: a failure is written into the report JSON with the
  phase it happened in, and the run exits non-zero.

## Supported by the prototype

* One ASCII FBX level-scope handoff per run, imported into a fresh container, on
  Maya 2024 in centimetres and Y-up.
* Static mesh geometry only: the objects the manifest marks `exported: true`,
  matched by name or by world position, compared by position, world bounding box
  size and orientation.
* Repeat runs that replace only their own container, `--keep-existing` runs, and
  the refusal paths for a textured, unreadable, missing or non-FBX handoff and
  for a manifest with another schema or another unit system.
* Evidence: the report JSON, the exit code, and `--json` on stdout.

## Not supported (reported, never approximated)

* Binary FBX handoffs as a first class input: the texture scan there is a
  heuristic and the report says so.
* Any scene unit other than centimetres, and a manifest world that is not Z up.
  The prototype compares centimetres and does not rescale.
* Rescaling, mirroring or offsetting a level that does not fit a signed
  permutation: the fit reports the deviation and every object reports its error.
* Animation, skeletal meshes, cameras, lights, materials other than the gray
  reference material, and any product protocol, package or installation change.
* Repairing the handoff: a mismatched or missing object is reported, never
  moved, renamed or substituted.

## Environment notes from this machine

Verified with Maya 2024 (mayapy 3.10.8) and the bundled `fbxmaya` plugin
2020.3.4.

* The `ns` flag and the `options` string of `cmds.file` are both ineffective for
  FBX import here (see "Container naming").
* `cmds.file(..., i=True, type="FBX")` accepts a file that is not FBX **without
  raising** and imports nothing, so the importer checks the file's own header
  (the ASCII `; FBX` or binary `Kaydara FBX Binary` magic) before importing.
  `tests/maya_host_scene_ref_tests.py` covers both the refusal and the fact that
  a plugin-visible no-op would otherwise pass unnoticed.
* A purely numeric node name cannot be addressed by name through `cmds` (for
  example `cmds.ls("0")` is a parse error), which is why a node that cannot be
  matched by name may only be reached through a namespace listing or a wildcard
  and is matched by position.
* `cmds.xform(shape, query=True, worldSpace=True, matrix=True)` only works on
  transforms; `cmds.exactWorldBoundingBox` works on both, and a shape's
  `boundingBoxMin/Max` attributes are in object space, which is what the
  orientation check uses.
* `maya.standalone.initialize()` injects a `cmds` module into the globals of its
  caller, which is why the importer's accessor is named `commands()` and not
  `cmds()`.

## Report

The report is the contract's `mtou-scene-ref-report/1` block: `ok`, `phase`,
`maya`, `container`, `counts`, `gray_material`, `transform_check`, `objects`,
`texture_scan`, `timing`, `memory` and `problems`. The keys the contract names
mean exactly what it says; these additions carry the evidence behind them, and a
reader that only wants the contract's keys can ignore them: `warnings` (the
cross-host disagreements above, allowed textures, transform matches), the
candidate notes and the whole fit block inside `transform_check`, the per record
`texture_scan` detail, `objects[].matched_by`, `match_distance_cm`,
`match_candidate`, `orientation_checked`, `local_bounds_size_cm`,
`offset_error_cm` with its expected and measured vectors, `centroid_error_cm`
with its expected and measured points, `centroid_checked`,
`centroid_faces_used`, `centroid_triangles_used`, `centroid_sampled`,
`container.group_path` and
`container_removal`, `scene` (the session state the run captured and restored,
including `import_side_effects`), `fbx` (the flags and mechanism actually used)
and `manifest` (the scope, scale and output summary read before importing).
Inside `transform_check` the additions are `decided_by`, `winner`,
`camera_contract` and `max_offset_error_cm`/`max_centroid_error_cm`.

## Evidence

`tests/maya_host_scene_ref_tests.py` (165 checks) authors its own reference
geometry and handoffs, synthesizes the manifest from the measured scene values
through the documented axis map, and asserts on a disposable scene: the clean
handoff imports with every object matched by name and every error inside
tolerance; the container namespace, group and gray material; zero `file` nodes
and zero loaded images; a repeat run that replaces only its container and leaves
a production cube, its material and its keys -- and the scene's playback range,
frame rate, current time and root objects -- exactly as they were; the
least-squares fit, its reflection determinant and its permutation; a manifest
value that is 25 cm wrong, one that names a node the scene does not hold, and
one whose node name was renamed so it can only be matched by world position;
the refusal of a textured handoff and its allowed import; refusals for a
non-FBX file, a missing file, a manifest with another schema, malformed manifest
JSON, a manifest in metres, a manifest world that is not Z up, a scene in inches
and an unusable container name; the contract's report shape; and the command
line entry point in its own process, which writes its own report and exits 0.

The self authored fixtures also pin the round trip: with four reference objects
placed off the origin and three of them rotated, the imported Maya world
positions are the authored ones to machine precision, and the fit recovers the
map the fixture was built with (determinant `-1`) with a residual below `1e-11`
cm. The other candidates are rejected on the record, each with its own size and
orientation errors.

A second fixture reproduces the real cross-host case: the reference geometry is
carried through the engine's map into the manifest, one child is named after the
engine's instance index (`0` inside the FBX, which this host imports as
`FBXASC048`), and the run has to report `maya_x=ue_x, maya_y=ue_z, maya_z=ue_y`
as the deciding candidate, match that child by world position, report the fit as
a signed permutation with a residual at machine precision, and report the camera
route's map as divergent by more than a thousand centimetres.

The host checks also cover the mirror signals: a hand-authored asymmetric mesh
whose vertices sit off its pivot passes on the clean path with an offset and a
surface centroid error at machine precision, a manifest whose offset is mirrored
reports `OFFSET_MISMATCH`, a manifest whose surface centroid is mirrored reports
`CENTROID_MISMATCH` while its position, size and offset stay clean, and a
manifest without a centroid reports the check unavailable without failing. The
fixture's own centroid comes from an independent reader (`polyInfo` plus
`xform`), so the value the manifest carries is not produced by the code it
checks.

The engine's own handoff was verified against this importer on 2026-09-28
(Unreal 5.7.4, seven named objects plus three instanced children): `ok: true`,
no problems, all nine objects matched -- six by name and three by world position
-- `best` `maya_x=ue_x, maya_y=ue_z, maya_z=ue_y`, maximum position error
`0.0` cm, maximum size error `4.5e-13` cm, and the camera route's map off by
`2404.2` cm in position and `115.1` cm in size.
