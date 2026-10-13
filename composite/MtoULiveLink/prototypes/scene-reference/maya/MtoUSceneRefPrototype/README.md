# MtoU scene reference prototype: Maya side

Import and verify the static geometry exported by the
[Unreal prototype](../../unreal/MtoUSceneRefPrototype/README.md). Maya compares
the imported objects with the supplied manifest and replaces the previous
reference only after verification. The [transfer contract](../../transfer.md)
owns the file format, coordinate conventions and report schema.

## Requirements

Use Maya 2024 with its bundled `fbxmaya` plugin, a centimetre/Y-up Maya scene,
and an ASCII FBX plus `mtou-scene-ref-manifest/1` JSON from the exporter.
The manifest describes a centimetre/Z-up Unreal world. Keep the handoff in its
own directory so its delivered image files can be detected.

The prototype handles static geometry. Animation, skeletal meshes, cameras,
lights, arbitrary origin offsets and automatic rescaling are outside its
import workflow. Binary FBX scanning is heuristic and cannot establish that a
handoff contains no image data.

## Import a handoff

Run from the repository root. Set `$Mayapy` to Maya's standalone interpreter
and `$Fbx`, `$Manifest`, `$Report` to input and output paths.

```powershell
$prototype = 'composite/MtoULiveLink/prototypes/scene-reference/maya/MtoUSceneRefPrototype'
& $Mayapy "$prototype/scripts/MtoUSceneRefPrototype.py" --fbx $Fbx --manifest $Manifest --report $Report
```

This creates the reference in `MtoU_UE_SceneRef`. Inspect `ok`, `problems`,
`warnings`, `world` and `container.after_run` in the JSON report before using
the result. The scene is not saved; the importer restores time, frame rate and
playback range after FBX import.

| Option | Use |
| --- | --- |
| `--container <name>` | Select the owned reference namespace/group; default `MtoU_UE_SceneRef` |
| `--target-world engine` | Keep the default handoff/animation world: UE `(x,y,z)` maps to Maya `(x,z,y)` |
| `--target-world camera` | Explicitly convert into the camera prototype's world, `(y,z,-x)` |
| `--shading display` | Show uniform gray while retaining imported material assignments; default |
| `--shading material` | Assign the reusable gray lambert |
| `--shading keep` | Keep imported display/material settings |
| `--dry-run` | Stage and verify, then discard the new import and keep the previous reference |
| `--allow-image-data` | Allow detected image data with a warning for a diagnostic run |
| `--json` | Print the full report as well as writing `--report` |

Use the default world for the current character animation route. Choose
`camera` only when deliberately working with that separate prototype.
The report identifies the applied conversion; it does not silently correct a
mismatched handoff.

Image files beside the FBX and embedded media cause `IMAGE_DATA_PRESENT`.
Material slots and texture path references are permitted. A path that resolves
on the local machine is reported separately from image data delivered by the
handoff. Inspect `media` and `texture_scan`; a gray viewport alone does not
prove that no image was loaded.

Exit codes: `0` verification passed, `1` verification/import/recovery failed,
`2` invalid usage or input contract. Use the report to identify the failing
phase and any residual resources.

## Updating and recovering a reference

The default group is
`|MtoU_UE_SceneRef:MtoU_UE_SceneRef`, inside its own namespace. The importer
stages a new reference in a free `<container>_Incoming` namespace. After
comparison, it moves the previous owned reference aside to `<container>_Retiring`,
takes the final name and verifies paths before deleting the old reference.
A failed takeover attempts to restore the previous reference.

Keep production/authored nodes outside the owned container hierarchy.
Ownership checks cover the marked container and top-level collisions; arbitrary
foreign descendants are outside the accepted safety boundary. A name collision
with an unowned namespace/group is refused.

| Reported problem | Recovery |
| --- | --- |
| `CONTAINER_NOT_OWNED` | Inspect the named objects. Choose a free container or deliberately relocate your authored nodes; do not delete an unfamiliar namespace. |
| `RECOVERY_REFUSED` | Inspect `update.recovery` and the retained reference UUID. Resolve the reported scene obstruction before retrying; preserve the retiring reference. |
| `STAGING_CLEANUP_FAILED` | Inspect `update.discard_errors` and `update.residual`. Resolve the obstruction and rerun so recovery can clean owned leftovers. |
| `STAGED_SWAP_FAILED` | Check `update.rolled_back`, `update.rollback` and `container.after_run` to confirm which reference remains. |
| Geometry mismatch | Inspect the named object's expected/measured values and the export scope. Correct the source handoff; the importer does not repair it. |

A dry run also performs recovery of leftovers from earlier interrupted runs.
Use a disposable scene when checking refusal and recovery behavior.

## Interpreting verification

The report compares exported manifest objects by name, then by world position
when names changed during FBX import. Default tolerances are 1 cm for position,
box size, pivot offset and surface centroid, and 0.5 degrees for orientation.

- Orientation is measured only when the export convention and the object's
  identification dimensions provide unambiguous axes. An unchecked orientation
  is not evidence of a match.
- Centroids use at most 20,000 faces per object. Larger inputs are sampled and
  marked `centroid_sampled`; older manifests without centroid data report
  `CENTROID_UNAVAILABLE`.
- A fitted coordinate map requires at least four non-coplanar matched objects.
  Named candidates remain available when that fit is underdetermined.
- Scope completeness and media warnings need separate inspection even when
  geometry checks pass. The report's `world`, `objects`, `transform_check`,
  `container` and `update` describe different parts of the result.

Detailed matching, matrix conventions, report fields and measured host
limitations live in the [transfer contract](../../transfer.md),
[official-capability notes](../../official-capabilities.md) and
[accepted scope/evidence](../../../../../../docs/project-history/mtou-livelink/issue-53-scene-reference-acceptance.md).

## Verification commands

From the repository root, with `$prototype` and `$Mayapy` as above and
`$HostReport` pointing to a disposable output location:

```powershell
python -m unittest discover -s "$prototype/tests" -t $prototype
& $Mayapy "$prototype/tests/maya_host_scene_ref_tests.py" --result $HostReport
```

The host checks create disposable scenes and fixtures. Add `--keep-scratch`
only when their generated FBX/manifest inputs are needed for inspection.
Use the [two-host instructions](../../README.md#reproduce) for the Unreal
exporter-to-Maya check.

Implementation entry points are `scripts/MtoUSceneRefPrototype.py` and
`scripts/mtou_scene_ref_mapping.py`. In an initialized Maya process, `run(fbx,
manifest, container=..., target_world=..., shading=..., allow_image_data=False,
dry_run=False)` returns `(report, exit_code)`.
