# MtoU scene reference prototype: Maya side

Importer and verifier for the bounded Unreal -> Maya static geometry reference
described in `../../transfer.md`. The Unreal side exports the static geometry of
one level scope as an ASCII FBX plus a `mtou-scene-ref-manifest/1` measurement
record; this side stages that file in its own namespace, keeps the material
assignment the file carries while showing the geometry in one uniform gray,
places it in the handoff's world (or converts it into the camera route's world on
request), and compares what Maya actually holds against the manifest so neither
host is trusted about the result. The previous reference is replaced only after
the comparison passed and the new container resolved every recorded path; a
takeover that fails on the way is rolled back, and the report says which state
the scene is in.

`transfer.md` is the frozen contract; this folder implements the Maya half of
its "Maya report schema", "Media rule", "Worlds and the node matrix", "Object
naming and the container", "Updating the reference" and "Commands" sections and
never changes them. Nothing here imports or depends on the product's
`MtoULiveLink.py`.

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
#    ... add --target-world camera to place the reference in the camera route's
#    world, --shading material|keep to change what the geometry is shown with,
#    --allow-image-data to import a handoff that delivers image data anyway, or
#    --dry-run to compare without replacing the previous reference
```

Exit codes are the contract's: `0` when every check passed, `1` when a check
failed (including a handoff that delivers image data and was not given
`--allow-image-data`), `2` for usage and contract errors (an unreadable or non-FBX
handoff, a manifest that is not `mtou-scene-ref-manifest/1`, a scene or manifest
that is not in centimetres, a manifest world that is not Z up, an unusable
container name, an unknown world or shading mode). `--json` prints the whole
report to stdout; without it the run prints a short summary.

## What a run does

1. Reads the manifest and reports the scope, the exported object count and the
   geometry scale **before** anything is imported, and compares the manifest's
   declared axis convention with the one this module measures against.
2. Scans the FBX text with its own detector, lists the image files in the
   handoff's own directory, and refuses to import a handoff that delivers image
   data (image files or embedded media) unless `--allow-image-data` was passed.
   Texture records and recorded paths are reported, never refused.
3. Creates the **staging** namespace `<container>_Incoming` and its group,
   imports the FBX inside that namespace, and parents every imported root node
   under the group with `cmds.parent(..., relative=True)` so the world placement
   is preserved. The previous reference is untouched at this point.
4. Reads the world the file arrived in when the caller asked for another one
   (`world.engine_check`), then carries every root's world matrix through one
   explicit conversion if `--target-world camera` was asked for.
5. Shows the geometry as asked: a uniform gray display override on every mesh
   shape with the imported material assignment left in place (`display`, the
   default), the gray lambert assigned instead (`material`), or nothing (`keep`).
6. Matches every exported manifest object to its Maya node, reads the node's
   world matrix and world bounding box, and compares them with the manifest:
   position, world box size, pivot-to-bounds-centre offset, surface centroid and
   the node matrix's axis angles.
7. Replaces the previous reference only when the comparison reported no
   problem, and never before the new one is in place: the previous, owned
   container is renamed aside into a free `<container>_Retiring` name, the
   staging namespace takes the container's name, every recorded path is
   re-resolved inside it, and only then is the retired reference deleted. A
   failure anywhere on that path rolls the takeover back — the container this run
   created is deleted and the retired reference is renamed back. On any earlier
   problem the staging namespace is deleted again and the previous reference
   stays exactly as it was.

## Container naming, and the one host limit

The container is a namespace called `MtoU_UE_SceneRef` and a transform of the
same name. On this Maya version a namespace and a root level node **cannot share
a name** (`cmds.createNode(..., name="X")` silently renames the node to `X1`
when namespace `X` exists, and `cmds.namespace(add="X")` fails when a node `X`
exists), so the group lives *inside* its own namespace and its full name is
`|MtoU_UE_SceneRef:MtoU_UE_SceneRef`. The report carries both parts plus the
group's full path.

A run works in a **staging namespace**: the first free name of the
`<container>_Incoming`, `<container>_Incoming_1`, ... family, with a group of the
same name inside it, so a namespace of that name that belongs to the scene is
neither emptied nor reused. The group carries this tool's ownership mark (the
string attribute `mtouSceneRefContainer`, value
`mtou-scene-ref-container/1 <namespace>`), and that mark is what makes a
container replaceable: a namespace, group or root-level node of the container's
name without it is refused (`CONTAINER_NOT_OWNED`) instead of deleted, and so is
another top-level transform inside an owned namespace.

The previous reference is untouched while the new import is measured. When the
comparison reports no problem the previous, owned container (namespace + group,
or a legacy root-level group of that name) is **renamed aside** into a free
`<container>_Retiring` name, the staging namespace is **renamed** to the
container's name (`cmds.namespace(rename=...)`), its group is renamed to
`<container>:<container>` and its mark is rewritten for that name. A rename moves
no node, so the measurements stay valid and a level costs three string operations
instead of a per-node swap; every recorded path is then re-resolved by short name
inside the final container, and only when none is missing is the retired
reference deleted. A failure in that sequence — the namespace rename, the group
rename, or a recorded path that does not resolve — deletes the container this run
created and renames the retired reference back, so the previous reference is
never lost, and `update.rolled_back` with `update.rollback` says what happened.

A run that fails earlier, or `--dry-run`, deletes the staging namespace again; a
staging namespace or a retiring namespace an interrupted run left behind is
recovered before the next run proceeds (deleted, or renamed back to the
container's name) and reported in `update.recovery`. `container.kept_existing`
and `container.after_run` are read from the scene at the end of the run — the
previous reference is looked up by UUID — so the report states what the scene
holds rather than what the run intended.

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

## Media rule

The reference keeps the material assignment the model carries: a material ball, a
material slot and a texture record are normal, and a recorded path is a
reference, not a delivered image. Image **data** is what a static reference
handoff must not deliver, and it is checked in two places, on both hosts:

* the files in the directory the exporter owns (`.png`, `.bmp`, `.tga`, `.jpg`,
  `.jpeg`, `.exr`, `.hdr`, `.dds`, `.fbm`), listed by the Maya side itself with
  `mapping.image_files_beside`, not taken from the exporter's word;
* media embedded in the FBX: a `Content:` line inside a `Texture:`/`Video:`
  record that carries a payload, on the same line or on the following quoted
  line.

Either finding refuses the import with `IMAGE_DATA_PRESENT` unless
`--allow-image-data` was passed, in which case it is reported as a warning and
the run continues. A recorded texture path that resolves to a file on this
machine is reported (`media.image_paths_present`) and does not fail the run: it
is a reference that resolves, not an image the handoff delivered.

Both hosts run the same frozen, line based detector so their counts can be
compared; `tests/test_scene_ref_mapping.py` holds the positive and negative
controls (an engine style fragment, a nested property block, a take clip, the
scene's own `Original|FileName` property, a commented out record, the
`Definitions` template, an embedded media record and a camera/light node
attribute):

* a line whose trimmed text starts with `Texture:` opens a texture record, one
  starting with `Video:` opens a video record, and the record ends only when the
  brace depth returns to zero -- the SDK writes a nested `Properties70` block
  before the name and `Content` lines, so a nested closing brace ends nothing;
* inside a record, `FileName:`, `Filename:` and `RelativeFilename:` name a file,
  taking the last quoted token on the line;
* a `NodeAttribute:` line whose quoted tokens include the class `Camera` or
  `Light` is a camera or light node record; the SDK writes no `Camera:` or
  `Light:` record head at all.

The scan reports `texture_records`, `texture_references`, `video_references`,
`content_records`, `embedded_media_records`, `camera_records`, `light_records`
and the distinct `files` in the order found, with the per record evidence. The
importer counts the `file` nodes the import created and how many of them name an
image that exists on disk, so a run never claims "no texture was loaded" from the
material it finally shows.

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
  map on the same handoff and states the divergence.
* By default nothing corrects the imported geometry, because the handoff's own
  world is also the world of the product's Maya to Unreal animation route:
  `MtoULiveLink.py`'s `convert_transform` maps `maya (x, y, z) -> ue (x, z, y)`,
  its own inverse and the same map as the handoff (checked 2026-09-29), so a
  reference imported without conversion agrees with the character and props the
  artist sends to Unreal. `--target-world camera` moves the geometry into the
  camera route's world instead, through one explicit conversion
  (`camera_map . engine_map^-1`, determinant `+1`, 90 degrees about the up axis)
  applied to every imported root's world matrix, with the matrix, the angle and
  the number of roots it moved reported in `world`. When a conversion is applied,
  `world.engine_check` holds a full comparison measured **before** it, which is
  what shows the file really arrived in the handoff's world.
* A point map is not the whole story. The file writes each node in its own local
  frame, so the node matrix Maya reads back is `frame . L_ue . map` with
  `frame = ((1,0,0),(0,-1,0),(0,0,1))` for `bForceFrontXAxis = false`. Measured
  over the ten object fixture handoff on 2026-09-29: every node matrix matches
  that composition to `1.3e-15`, while `L_ue . map^T` is off by up to `7.7`. The
  factor is reported as `transform_check.node_frame_factor`, and the orientation
  comparison is anchored to the handoff's convention rather than to the scored
  candidate, so it answers "did the file write the frames the convention
  promises" and its error is the same for every candidate. A manifest that
  declares no measured export axis option reports the orientation comparison as
  unavailable instead of comparing against a guess.
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
* Orientation is only compared where the mesh has an unambiguous axis: an
  object whose **identification size** -- its shape's local bounding-box extents
  scaled by the node's own axis scale lengths, reported as
  `objects[].identification_size_cm` -- has three distinct extents. A non-uniform
  scale is what separates axes a symmetric mesh would leave ambiguous, so a cube
  scaled `(2, 0.5, 0.25)` is compared while the same cube at uniform scale is
  not. For anything else a symmetry rotation can produce the same geometry with a
  different node matrix, so the object reports `orientation_checked: false`
  instead of a false pass.
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
* The import happens in the run's own staging namespace first, and the previous
  reference is only replaced after the comparison passed and the new container
  resolved every recorded path. A failed or `--dry-run` run deletes the staging
  namespace again and leaves the previous reference exactly as it was; a takeover
  that fails after the rename is rolled back and the retired reference is put
  back. The report says which of those happened in `update` and names the state
  the scene is in, by UUID, in `container`.
* Only nodes carrying this tool's ownership mark, and the staging and retiring
  namespaces this tool created, are created, replaced or deleted. Nothing else is
  renamed, reparented, reassigned or deleted: a same-named namespace this tool
  does not own is refused, and a staging name it does not own is avoided.
* The gray material is only created when `--shading material` asks for it, in the
  root namespace, and reused by later runs. If a node of that name exists and is
  not a lambert, the run refuses instead of renaming or replacing an object it
  did not create.
* A handoff that delivers image data -- image files in its own directory, or
  media embedded in the FBX -- is not imported without `--allow-image-data`.
  Texture records and recorded paths are material information, not image data,
  and never refuse a run.
* `main()` never raises: a failure is written into the report JSON with the
  phase it happened in, and the run exits non-zero.

## Supported by the prototype

* One ASCII FBX level-scope handoff per run, staged in its own namespace and
  swapped into the container only after it verified, on Maya 2024 in centimetres
  and Y-up.
* Static mesh geometry only: the objects the manifest marks `exported: true`,
  matched by name or by world position, compared by position, world bounding box
  size, pivot offset, surface centroid and node orientation.
* Two worlds: the handoff's own (default, the animation route's world) and the
  camera route's world by one explicit conversion.
* Three display choices: a uniform gray viewport override with the imported
  material assignment kept (default), the gray lambert assigned, or nothing.
* Repeat runs that replace only their own container, `--dry-run` runs, and the
  refusal paths for a handoff that delivers image data, an unreadable, missing or
  non-FBX handoff, and a manifest with another schema or another unit system.
* Evidence: the report JSON, the exit code, and `--json` on stdout.

## Not supported (reported, never approximated)

* Binary FBX handoffs as a first class input: the texture scan there is a
  heuristic and the report says so.
* Any scene unit other than centimetres, and a manifest world that is not Z up.
  The prototype compares centimetres and does not rescale.
* Rescaling, mirroring or offsetting a level that does not fit a signed
  permutation: the fit reports the deviation and every object reports its error.
* Animation, skeletal meshes, cameras, lights, and any product protocol,
  package or installation change. Material assignment is carried and reported but
  never translated: the reference is not claimed to shade like Unreal.
* Any origin offset, and any axis conversion the caller did not ask for.
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
`maya`, `manifest`, `scene`, `fbx`, `container`, `update`, `counts`, `media`,
`texture_scan`, `world`, `materials`, `transform_check`, `objects`, `timing`,
`memory`, `problems` and `warnings`. The keys the contract names mean exactly
what it says; these additions carry the evidence behind them, and a reader that
only wants the contract's keys can ignore them: the candidate notes and the whole
fit block inside `transform_check`, the per record `texture_scan` detail,
`objects[].matched_by`, `match_distance_cm`, `match_candidate`,
`orientation_checked`, `local_bounds_size_cm`, `identification_size_cm`,
`offset_error_cm` with its expected and measured vectors, `centroid_error_cm`
with its expected and measured points, `centroid_checked`, `centroid_faces_used`,
`centroid_triangles_used`, `centroid_sampled`, `update.discarded_nodes` and
`update.post_swap_paths_missing`, `container.group_path`, `scene` (the session
state the run captured and restored, including `import_side_effects`), `fbx` (the
flags and mechanism actually used) and `manifest` (the scope, conventions, filter,
scale and output summary read before importing). Inside `transform_check` the
additions are `decided_by`, `winner`, `camera_contract`, `node_frame_factor`,
`world_conversion_matrix`, `orientation_available`,
`max_offset_error_cm`/`max_centroid_error_cm`/`max_orientation_error_deg`.

## Evidence

`tests/maya_host_scene_ref_tests.py` (274 checks) authors its own reference
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
the refusal of a handoff that delivers image data and its allowed import;
refusals for a non-FBX file, a missing file, a manifest with another schema,
malformed manifest JSON, a manifest in metres, a manifest world that is not Z up,
a scene in inches and an unusable container name; the update contract under
injected failures, where a namespace rename, a group rename and a final path read
back that does not resolve are all rolled back with the previous reference
intact, a partial import cleans up its own staging namespace, a namespace of the
container's or the staging name this tool does not own is refused or avoided, and
a namespace an interrupted run left behind is recovered; the contract's report
shape; and the command line entry point in its own process, which writes its own
report and exits 0.

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
