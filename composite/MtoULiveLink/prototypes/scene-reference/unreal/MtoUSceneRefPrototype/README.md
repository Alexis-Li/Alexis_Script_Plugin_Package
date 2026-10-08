# MtoUSceneRefPrototype (Unreal side)

Editor-only prototype for the Issue 53 verification: it resolves a **level
scope**, exports the selected static geometry with the engine's own FBX level
exporter while suppressing the components that are not static geometry, and
writes a manifest of the world transforms, bounds and records the engine
evaluated, next to the file the engine produced.

It is verification scaffolding. It does not change the MtoULiveLink product, its
protocol (v9), its packages, or the host project beyond a plugin entry.

## Commands

| Command | Purpose |
| --- | --- |
| `MtoUSceneRef.BuildFixture` | Rebuilds the generated fixture under `/Game/MtoUSceneRefFixture` and loads it: a persistent level with a plain, a rotated, a non-uniformly scaled, an instanced, a two-component Blueprint actor and a **mixed** Blueprint actor (a mesh plus a light, a camera, a child actor and a skeletal mesh component), one loaded sublevel, one sublevel that stays out of the world, and one sublevel whose material samples a texture. |
| `MtoUSceneRef.Export [textured] [obj] [frontx] [out=<dir>]` | Runs one transfer over a fixture scope and writes `<scope>.fbx` plus `<scope>.manifest.json`. `textured` uses the texture probe scope, `frontx` sets the exporter's forced front X axis, and `obj` also runs the official OBJ level exporter over the same resolution. **The `obj` probe crashes a null-RHI run** (the OBJ exporter bakes material images through `MaterialBaking`); it exists to record that, and the suite never calls it. |
| `MtoUSceneRef.ExportLevel <level_package> [sublevel=<package> ...] [frontx] [obj] [out=<dir>]` | The same transfer for a level that is already the loaded editor world, so a real project level can be measured without the fixture. It never loads or switches a level. |
| `MtoUSceneRef.Peer <mayapy> <peer script> [out=<dir>] [scope=<name>] [world=camera] [shading=material\|keep] [dryrun] [allowimagedata]` | Runs the Maya importer/verifier over the last export and prints its report, passing the flags through to the Maya CLI verbatim. The `MtoUSceneRefPrototype.RealMayaPeer` test does the same with checks. |

`MtoUSceneRef.Peer` resolves the mayapy it launches in this order: the
`-MtoUSceneRefMayapy=<path>` command line switch, the `MTOU_SCENEREF_MAYAPY`
environment variable, then the first argument. A console command's arguments are
split on whitespace and an 8.3 short path breaks Maya's own plug-in resolution,
so a mayapy installation under a path with spaces needs the switch or the
variable.

The automations are `MtoUSceneRefPrototype.{ScopeSemantics, ExportMainScope,
TextureProbe, TextureDetector, MixedBlueprintFilter, NaniteSourceMesh,
LandscapeScope, LevelInstanceRefusal, WorldPartitionScope, RealMayaPeer}`;
`RealMayaPeer` needs `-MtoUSceneRefMayapy=`, `-MtoUSceneRefPeer=` and
`-MtoUEvidence=` and reports that it was not requested without them.

`LandscapeScope` checks its record against the geometry the engine's landscape
branch writes (`2 x quads^2 x components` triangles, `(quads + 1)^2 x components`
vertices, a surface thinner than the actor's own bounds and a flat fixture's
centroid at the centre of its measured bounds) and against the manifest that the
Maya peer reads. `WorldPartitionScope` exports the partitioned fixture twice —
once as authored, where the loaded-only scope has to report itself
`not_confirmed` with the unspawned authored actors named and carries a
`SCOPE_LOADED_ONLY` warning, and once with streaming disabled, where every
authored descriptor is spawned and the scope reports itself `confirmed`.

`MtoUSceneRef.BuildFixture` can build the fixture once per editor session: a
second build would delete fixture levels the session still holds as its world,
which crashes the editor, so the command refuses with that reason instead. Use a
fresh editor session to rebuild.

## What the manifest carries

Per object: world location, rotation and scale, the 16-value world matrix, the
world axis-aligned bounds with their size, the LOD 0 triangle and vertex counts,
the materials it references, and the area-weighted surface centroid of the
object's LOD 0 triangles. The centroid is the only one of those that a *mirrored*
placement moves, and it is area weighted because the exported mesh does not hold
the same vertex set as the render buffer (measured: 198 render positions against
144 exported positions with the same 284 triangles).

A landscape is measured from the source the engine's landscape branch writes —
`FLandscapeComponentDataInterface` vertices at `ALandscapeProxy::ExportLOD`, two
triangles per quad, the component's relative location and the actor transform
applied — rather than from the actor's own collision/editor bounds, which include
the terrain's thickness (measured: `8064 x 8064 x 512` cm as actor bounds against
`8064 x 8064 x 0` cm as surface on the fixture, and the Maya peer validates the
surface with size error `0.0` cm).

It also carries the facts a static reference handoff is judged on: the
`conventions` block (which axis option the export used and the point map that
follows from it), the `filter` block (the policy and every component the export
suppressed with its class and reason), the `scope` block's coverage statement
(`coverage`, `completeness`, `coverage_note` and, for a partitioned world, the
`world_partition` inventory read from the partition's actor descriptors: the
descriptor, spawned, unspawned authored and generated-HLOD counts, the unspawned
paths up to `inventory_limit`, and whether the list is truncated), and the
`output` block's media records (texture, video, content and embedded media
records, camera and light node records, produced files and image files, and the
node names the file holds).

## What it does to the editor

- The scope resolution selects the actors the engine will export and restores the
  previous selection afterwards; the export never leaves the selection changed.
- For the duration of the export, the components the resolver reported as
  suppressed (skeletal mesh, camera, light, child actor) carry `bHiddenInGame`,
  which is the only caller-side switch the engine's FBX level exporter reads; the
  guard restores every previous value, checks that the number it suppressed
  matches the resolution, and reports a mismatch as a warning instead of hiding
  it.
- It never loads, unloads, creates or renames a level; it never writes to the
  scope's actors; it never saves the scope's levels.
- The fixture it builds is generated content under `/Game/MtoUSceneRefFixture`
  and is deleted and rebuilt on every fixture build.
- Reading a partitioned world's actor descriptors is read only: the resolver
  enumerates them, counts how many have spawned an actor and records the paths of
  the ones that have not. It never loads, streams or activates a cell.

## Files

| File | Contents |
| --- | --- |
| `MtoUSceneRefTypes.h` | Scope, object, suppressed-component, output and measurement records |
| `MtoUSceneRefScope.h/.cpp` | Scope resolution, actor classification, the static-geometry filter, skipped and unsupported reporting |
| `MtoUSceneRefTransfer.h/.cpp` | Export through `UAssetExportTask`/`ULevelExporterFBX`, the component suppression guard, the OBJ probe, the produced-file inspection and the manifest |
| `MtoUSceneRefFixture.h/.cpp` | The generated sample |
| `MtoUSceneRefPeer.h/.cpp` | Maya process launch and report reading |
| `MtoUSceneRefPrototypeModule.cpp` | Console commands |
| `Tests/MtoUSceneRefPrototypeTests.cpp` | Automation tests |

The contract with the Maya side is `../transfer.md`; the capability review that
justifies these choices is `../official-capabilities.md`.
