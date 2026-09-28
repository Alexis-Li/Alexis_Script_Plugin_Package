# Official level export capability for a texture-free Maya reference

Reviewed 2026-09-28 against the installed Unreal Engine 5.7.4 source tree and
the Maya 2024 installation on this machine. Every claim below is either a quote
from, or a line reference into, one of those two installations. No plugin was
installed and no third-party code was used.

## What Unreal actually ships

| Path | Class | Writes | Selected actors only |
| --- | --- | --- | --- |
| File > Export > Export All / Export Selected | `UExporter::RunAssetExportTask` over the world | whichever exporter matches the extension | yes, from the editor selection |
| FBX level export | `ULevelExporterFBX` (`Editor/UnrealEd/Private/EditorExporters.cpp`) | named nodes with hierarchy, instancing, world transforms on the nodes | yes, per actor: `Actor->IsSelected()` |
| OBJ level export | `ULevelExporterOBJ` (same file) | flattened geometry with world transforms baked into the vertices, `g` group per chunk | yes, through an export context |
| STL level export | `ULevelExporterSTL` | triangles, no materials | yes, text exporter |
| T3D level export | `ULevelExporterT3D` | actor properties, no geometry | yes |
| Static mesh OBJ asset export | `UStaticMeshExporterOBJ` | one mesh in asset space, no materials | n/a (asset) |

There is no official whole-level export that writes geometry without materials:
`ULevelExporterFBX` always builds an FBX material for every used material, and
`ULevelExporterOBJ` asks its material question with an answer that a programmatic
run cannot change (below).

**Entry point the prototype uses.** `UExporter::RunAssetExportTask(UAssetExportTask*)`
(`Runtime/Engine/Private/UnrealExporter.cpp`) with `bSelected = true`, driven by
the editor selection the prototype sets and restores. The FBX path never looks at
an explicit actor array: `FSelectedActorExportObjectInnerContext`
(`Runtime/Engine/Public/UnrealExporter.h`) is consulted by the *text* exporters
only, and the FBX path calls `Actor->IsSelected()` directly
(`FbxMainExport.cpp`). Both official paths therefore need the editor selection,
which is what the prototype manipulates — it does not build a second selection
mechanism.

## Texture behaviour of the two candidate paths

**OBJ is not usable as a texture-free handoff.** `ULevelExporterOBJ::ExportText`
asks `"Would you like to export the materials as images (slower)?"`, and outside
that dialog the reply is forced to *yes*:

```cpp
int32 YesNoCancelReply = EAppReturnType::Yes;
if (!(GIsAutomationTesting || FApp::IsUnattended() || ExportTask->bAutomated))
{
    YesNoCancelReply = FMessageDialog::Open(...);
}
```

An automated or unattended run therefore bakes every material to `.bmp` images
and writes them next to the `.obj` with a `.mtl` (`ExportMaterialPropertyTexture`
calls `IMaterialBakingModule::BakeMaterials`). That is a rendering dependency in
a headless run and image files in the output directory; there is no switch to
skip it. The OBJ probe in this prototype exists to record exactly that, and it
was run twice on the fixture scope:

| Run | Result |
| --- | --- |
| `UnrealEditor-Cmd -NullRHI -unattended`, the same way the automation suite runs | `Fatal error! Unhandled Exception: EXCEPTION_ACCESS_VIOLATION reading address 0x0000000000000588` with the callstack inside `UnrealEditor-MaterialBaking.dll`, after the exporter logged `Executing OBJ automated export, materials will be exported as images by default.` and after a 30-byte `.mtl` containing only `newmtl BasicShapeMaterial` had been written |
| `UnrealEditor-Cmd -unattended` with the renderer available | `MtoUSceneRef_Host.obj` (187 KB, 1443 vertices, 2176 faces), `MtoUSceneRef_Host.mtl` naming `map_Kd`/`map_Ks`/`bump`, three 1×1 `.bmp` files (58 bytes each) that the exporter wrote for those maps, and `RunAssetExportTask` still returned failure |

Its geometry is also the wrong shape for a reference: the file holds seven
`g <name>` groups named after the actors' *object* names (`g StaticMeshActor_0`),
not their labels, so the artist-visible names are lost, and the axes are written
in the same `(x, z, y)` order the FBX round trip measured, so it buys nothing in
return for a material dependency and a crash risk.

**FBX with baking disabled writes no images but can still reference textures.**
`BakeMaterialProperty` returns immediately when
`FbxExportOptions->BakeMaterialInputs == EFbxMaterialBakeMode::Disabled`, so no
image is written. Two other branches still create an `FbxFileTexture` whose file
name is the source asset's import path
(`Texture->AssetImportData->GetFirstFilename()`): a texture sample connected
straight to `BaseColor` (`FFbxExporter::FillFbxTextureProperty`) and a texture
material parameter (`ExportMaterial` parameter loop, `FbxMainExport.cpp`).
A texture-driven material in the scope therefore produces texture *records* in
the file, and Maya's FBX plugin has no option to skip materials or textures —
its import preset (`plug-ins/fbx/plug-ins/FBX/Presets/import/…fbximportpreset`)
lists geometry, animation, cameras, lights and constraints, and no shading
switch. The reference workflow must decide on the record, not on the flag, which
is why the prototype inspects the produced file and refuses to import a textured
handoff.

## Object classes

The FBX mesh the exporter writes is not the render vertex buffer: for the same
cone the render data held 198 positions, the exported `Geometry::Cone` mesh held
144, and both hold the same 284 triangles. Anything that compares geometry
between the hosts (a mirror check, a weight check, a vertex budget) has to be
derived from the triangles, not from the render vertex list.

| Object | Official behaviour | Measured here |
| --- | --- | --- |
| `AStaticMeshActor` | `ExportStaticMesh(actor, component, …)`, node named by the level adapter | yes |
| Blueprint actors | `ExportActor(actor, true, …)`; every qualifying component becomes a child node when there is more than one | yes, with a two-component Blueprint actor |
| `UInstancedStaticMeshComponent` | `ExportInstancedMeshToFbx`: one child node per instance named by its index, instance transform relative to the component | yes, three instances |
| Actor with several mesh components | one child node per component, named after the component | yes |
| `ALandscapeProxy` | explicit landscape branch (`ExportLandscapeToFbx`), per-component quads in world space | classified and reported; not placed as a fixture on this host |
| `ALevelInstance` | **refused**: `IsSomethingToExport` warns `"Exporting Level Instances to FBX is not supported."` and the export skips it | reported by the scope resolver with that reason |
| Nanite | no separate actor class; the mesh branch exports the hi-res mesh description when `bExportSourceMesh` and `IsNaniteEnabled()`; the OBJ path always uses LOD 0 | not measured; `bExportSourceMesh` is off in the prototype |
| Lights, cameras, emitters | exported as nodes, not geometry | reported as skipped non-geometry by the scope resolver |
| World Partition | cells stream like streaming levels; content that is not streamed in is not in the world | reported as absent; no World Partition map was built on this host |

## Editor-world streaming

An **editor world loads every streaming level it owns**: with the level state
`Unloaded`, `ULevelStreaming::DetermineTargetState()` returns `LoadedNotVisible`
for any non-game world. `bShouldBeLoaded` only decides in a game world, and
`bShouldBeVisibleInEditor` only affects PIE. A "requested but not loaded"
sublevel therefore appears in the editor as a level package the loaded world
does not hold at all, and that is the case the prototype tests; World Partition
cells that are not streamed in are the other case, and are reported the same way.

## Maya side

- The FBX plugin ships with Maya 2024 (`fbxmaya`, plugin version 2020.3.4) and
  needs no installation step; Maya's FBX import options have no material or
  texture switch, and units are converted to centimetres by default
  (`DynamicScaleConversion` on, `UnitsSelector` = Centimeters).
- OBJ and STL have no built-in Maya importer; FBX is the only one of the two
  candidate formats Maya can read without a new component.
- Maya's own axis conversion is applied by the plugin, so the world mapping is a
  property to measure, not to assume; the prototype fits the map from the
  imported node positions and reports it next to the conversion the camera
  verification documented (`ue (x, y, z) -> maya (y, z, -x)`).

## What this means for the prototype

The transfer is: official FBX level export over an explicit scope, baking
disabled, a manifest of the engine-evaluated world transforms written beside it,
a post-export inspection of the produced file, and a Maya importer that refuses a
textured handoff, places the geometry in its own container and compares what Maya
holds against the manifest. Nothing in this route needs a new dependency, a new
host, or a change to the product's protocol.
