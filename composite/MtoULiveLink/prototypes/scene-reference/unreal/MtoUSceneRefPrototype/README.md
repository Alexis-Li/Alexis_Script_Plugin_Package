# MtoUSceneRefPrototype (Unreal side)

Editor-only prototype for the Issue 53 verification: it resolves a **level
scope**, exports the selected static geometry with the engine's own FBX level
exporter, and writes a manifest of the world transforms and bounds the engine
evaluated, next to the file the engine produced.

It is verification scaffolding. It does not change the MtoULiveLink product, its
protocol (v9), its packages, or the host project beyond a plugin entry.

## Commands

| Command | Purpose |
| --- | --- |
| `MtoUSceneRef.BuildFixture` | Rebuilds the generated fixture under `/Game/MtoUSceneRefFixture` and loads it: a persistent level with a plain, a rotated, a non-uniformly scaled, an instanced and a two-component Blueprint actor, one loaded sublevel, one sublevel that stays out of the world, and one sublevel whose material samples a texture. |
| `MtoUSceneRef.Export [textured] [obj] [frontx] [out=<dir>]` | Runs one transfer and writes `<scope>.fbx` plus `<scope>.manifest.json`. `textured` uses the texture probe scope, `frontx` sets the exporter's forced front X axis, and `obj` also runs the official OBJ level exporter over the same resolution. **The `obj` probe crashes a null-RHI run** (the OBJ exporter bakes material images through `MaterialBaking`); it exists to record that, and the suite never calls it. |
| `MtoUSceneRef.Peer <mayapy> <peer script> [out=<dir>]` | Runs the Maya importer/verifier over the last export and prints its report. The `MtoUSceneRefPrototype.RealMayaPeer` test does the same with checks. |

The automations are `MtoUSceneRefPrototype.{ScopeSemantics, ExportMainScope,
TextureProbe, TextureDetector, RealMayaPeer}`; `RealMayaPeer` needs
`-MtoUSceneRefMayapy=`, `-MtoUSceneRefPeer=` and `-MtoUEvidence=` and reports
that it was not requested without them.

## What the manifest carries

Per object: world location, rotation and scale, the 16-value world matrix, the
world axis-aligned bounds with their size, and the area-weighted surface
centroid of the object's LOD 0 triangles. The centroid is the only one of those
that a *mirrored* placement moves, and it is area weighted because the exported
mesh does not hold the same vertex set as the render buffer (measured: 198
render positions against 144 exported positions with the same 284 triangles).

## What it does to the editor

- The scope resolution selects the actors the engine will export and restores
  the previous selection afterwards; the export never leaves the selection
  changed.
- It never loads, unloads, creates or renames a level; it never writes to the
  scope's actors; it never saves the scope's levels.
- The fixture it builds is generated content under `/Game/MtoUSceneRefFixture`
  and is deleted and rebuilt on every fixture build.

## Files

| File | Contents |
| --- | --- |
| `MtoUSceneRefTypes.h` | Scope, object, output and measurement records |
| `MtoUSceneRefScope.h/.cpp` | Scope resolution, actor classification, skipped and unsupported reporting |
| `MtoUSceneRefTransfer.h/.cpp` | Export through `UAssetExportTask`/`ULevelExporterFBX`, the OBJ probe, the produced-file inspection and the manifest |
| `MtoUSceneRefFixture.h/.cpp` | The generated sample |
| `MtoUSceneRefPeer.h/.cpp` | Maya process launch and report reading |
| `MtoUSceneRefPrototypeModule.cpp` | Console commands |
| `Tests/MtoUSceneRefPrototypeTests.cpp` | Automation tests |

The contract with the Maya side is `../transfer.md`; the capability review that
justifies these choices is `../official-capabilities.md`.
