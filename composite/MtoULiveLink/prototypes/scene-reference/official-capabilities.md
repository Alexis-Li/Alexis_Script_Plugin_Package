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

**A selected actor exports every qualifying component, not only its meshes.**
`FFbxExporter::ExportActor` (`Editor/UnrealEd/Private/Fbx/FbxMainExport.cpp:2706`)
iterates `Actor->GetComponents()`, keeps `USceneComponent` casts that are not
`bHiddenInGame` (2793-2796), and adds a component when it is a
`UStaticMeshComponent` with a mesh (2803-2805), a `USkeletalMeshComponent` with a
mesh (2807-2809), a `UCameraComponent` (2811-2813), a `ULightComponent`
(2815-2817) or a `UChildActorComponent` with a child actor (2819-2821). The
emission branches build a real `FbxCamera` (2909-2913), a real `FbxLight`
(2915-2919) and recurse into the child actor with all of its own components
(2921-2925). A Blueprint actor that mixes a wall mesh with a lamp and a camera
therefore lands all three in the file.

There is **no** export option or hook that restricts the export to static mesh
components: `UFbxExportOption` carries no component filter
(`UnrealEd/Classes/Exporters/FbxExportOption.h:63-131`), `ExportActor`'s only
switch is the all-or-nothing `bExportComponents` (`FbxExporter.h:292`), and no
export task flag exists. The one caller-side hook that works is the predicate the
loop itself reads: a component with `bHiddenInGame` set is skipped, so the
prototype sets that flag on exactly the components its scope resolution reports
as suppressed, exports, and restores every previous value. `IsSomethingToExport`
(`EditorExporters.cpp:1766-1817`) only decides whether an actor has anything to
export at all; it is not a filter either.

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
| `ALandscapeProxy` | explicit landscape branch (`ExportLandscapeToFbx`), per-component quads in world space | measured (2026-09-29) with a programmatic 64x64, one component landscape: the engine wrote `Vertices: *4096` and `PolygonVertexIndex: *23814` (7938 polygons) for the actor while the scope's own landscape record counted no triangles at all -- the geometry it exports is real, the prototype's scale numbers are not its geometry count |
| `ALevelInstance` | **refused**: `IsSomethingToExport` warns `"Exporting Level Instances to FBX is not supported."` and the export skips it | measured (2026-09-29) with a real level instance over a saved sublevel: the scope reports it as unsupported with that reason, produces no object and selects nothing; an isolated export of only the instance answers success while writing no file, and the refusal message appears exactly once |
| Nanite | no separate actor class; the mesh branch exports the hi-res mesh description when `bExportSourceMesh` and `IsNaniteEnabled()`; the OBJ path always uses LOD 0 | measured (2026-09-29) with a duplicated engine cube whose `NaniteSettings.bEnabled` reads back true (`source_model=valid`, no hi-res mesh description): the file carries the render data (48 triangles, 54 vertices at LOD 0), the node arrives in Maya and matches, and no warning mentions it. With `bExportSourceMesh` off the Nanite-data branch (`FbxMainExport.cpp:5469-5488`) cannot be taken, so what the handoff carries is render geometry |
| Lights, cameras, emitters | exported as nodes, not geometry | reported as skipped non-geometry by the scope resolver |
| World Partition | cells stream like streaming levels; content that is not streamed in is not in the world | measured (2026-09-29) on a fixture level built from the engine's `OpenWorld` template: as authored (streaming enabled in the editor, no streaming source) all three placed actors are absent from the loaded world and the scope holds only the template's always-loaded actor, which is exactly how not-streamed content appears; with `UWorldPartition::SetEnableStreaming(false)` the three placed actors enter the scope and the export writes four objects, and the manifest reports `world_partition: true`. The template's own 2 km landscape (1 landscape + 64 streaming proxies) was deleted by the fixture builder so the scope measured is the one it authored |

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
- The point map is not the whole story: the file writes each node in its own
  local frame, so the node matrix Maya reads back is `frame . L_ue . map` with
  `frame = ((1,0,0),(0,-1,0),(0,0,1))` for `bForceFrontXAxis = false`. Measured
  over the ten object fixture handoff on 2026-09-29: every measured node matrix
  matches that composition to `1.3e-15`, while `L_ue . map^T` is off by up to
  `7.7`. The factor is a mirror, which is why the node matrix has a positive
  determinant although the point map does not; the prototype reports it as
  `transform_check.node_frame_factor` and only runs the orientation comparison
  when the manifest declares an export option whose factor was measured.

## What the produced file actually contains

Measured on ASCII FBX written by this host's `fbxmaya` 2020.3.4 (FBX 2020) and by
the engine's own exporter, because a record rule that is guessed counts the wrong
things:

| Fact | Layout | Consequence for a detector |
| --- | --- | --- |
| Cameras and lights | `NodeAttribute: <id>, "NodeAttribute::<name>", "Camera"|"Light" {` | The SDK writes no `Camera:` or `Light:` record head at all; counting those heads finds nothing. The class token on a `NodeAttribute:` line is the thing to count. |
| Texture media | `Video: <id>, "Video::<name>", "Clip" {` plus `Texture: <id>, "Texture::<name>", "" {`; the Video record uses `Filename:` while the Texture record uses `FileName:` | Both spellings carry a recorded path, so both belong to the file-name heads. |
| Nested blocks | The SDK writes a nested `Properties70: { ... }` block before the name lines | A walk that ends a record at the first `}` line closes it inside `Properties70` and never sees the names or the media that follow. The record ends when the brace depth returns to zero. |
| Embedded media | Inside a `Video:` record, a `Content: ,` line followed by the payload as a quoted base64 string on the next line; a record without embedded media carries no `Content:` line | The presence of a payload is what makes it media data; an empty `Content:` record delivers nothing and only the payload-bearing ones count. |

## What this means for the prototype

The transfer is: official FBX level export over an explicit scope with the
non-geometry components suppressed for the duration of the export, baking
disabled, a manifest of the engine-evaluated world transforms written beside it,
a post-export inspection of the produced file for image data (image files,
embedded media) and for camera or light records, and a Maya importer that stages
the file in its own namespace, keeps the material assignment, shows the geometry
in one uniform gray, places it in the handoff's world or in the camera route's
world by one explicit conversion, compares what Maya holds against the manifest,
and only then replaces the previous reference. Nothing in this route needs a new
dependency, a new host, or a change to the product's protocol.
