# Model Preview Full-Character Recalibration

Issue #22 revalidated the Model preview quality metrics and failure boundaries
against the full-character Driver workflow on 2026-08-27, using the stock
Unreal Engine 5.7.4 Win64 editor (build 5.7.4-51494982). The Issue #13 record
remains the garment-only baseline; its tables and thresholds were not modified.

## Metric basis after Issue #19/#22

Quality measurement, skin-weight transfer, and Morph projection run against the
resolved Driver garment surface only. Distances normalize by the resolved
garment scale (the Preview or resolved-surface bounding diagonal), never by the
complete-character bounds:

* Isolated-character evidence: pairing one Preview with an isolated
  full-character Driver whose non-garment parts sit far outside the agreement
  radius and with the legacy garment-only Driver resolves exactly the same
  pieces and yields bit-identical normalized averages (0.001-diagonal shift:
  0.000500001457 average / 0.000707108842 RMS in both pairings).
* Synthetic anchor: SameTopology through the full-character fixture measures
  average/max 0.000000; applying the same 0.001-diagonal shift to the full and
  legacy pairings yields bit-identical 0.000400001166 average /
  0.000632457375 RMS, confirming character bounds never enter normalization.

## Resolution failure boundaries

Automatic resolution keeps nearest-ownership selection, uses matching imported
slot names or assigned material assets to narrow geometric candidates only when
that evidence covers the whole Preview, and applies two structural boundaries
(constants live beside `MtoUResolveDriverGarmentSurface` in the private
`MtoUDriverGarmentSurface` module, which owns all source-selection and
geometry-validation rules):

* Shared admission preflight — before any spatial indexing, coverage, or mass
  accounting, both resolution paths reject geometry that is non-empty but
  unusable: zero-triangle LOD0, non-finite coordinates, a scaleless Preview
  bounding box, and zero-area degenerate meshes. Collinear triangles keep a
  positive triangle count, finite coordinates, and a nonzero bounding box, so
  the preflight also rejects a zero (or non-finite overflow) total triangle
  area transactionally, with no partial Generated Preview and an actionable,
  recoverable diagnostic (Issue #32).
* Source-mass boundary — selected Driver triangles must stay within 1.70x of
  Preview triangles (`MaxDriverToPreviewTriangleRatio`), measured only when
  the Preview holds at least `MinTrianglesForMassAccounting` (8) triangles:
  the floor is a Preview TRIANGLE count, so sparse sub-floor Previews stay
  exempt from mass accounting while vertex counts or zero-triangle degenerate
  inputs can never enter or bypass the gate (Issue #32). Whole duplicated
  garments measure 2.00x+ and production proximity mixing measured 2.31x/4.49x,
  all rejected deterministically with actionable diagnostics naming both
  remedies (remove duplicates/split sections, or the manual Driver Garment Slot
  Override).
  This is a conservative topology-density support limit, not proof of excess
  surface area or ambiguity. Issue #40 below measures legitimate reduction
  rejected by the same limit and verifies the manual remedy; no threshold was
  raised and the original #22 anchors remain historical measurements.
* Twin-region boundary — a selected region and an unselected alternative whose
  surfaces mutually agree within 2% of Preview scale for at least 98% of their
  vertices are indistinguishable candidates. Exact and near-shifted duplicate
  fixtures both fail with the same deterministic ambiguity on repeated Prepare
  calls, even when nearest-ownership gives the selected copy every tie. Two
  selected disconnected pieces remain parts of one garment rather than
  competing candidates; duplicated selected mass is still rejected above.
* Single-region whole Drivers are the legacy passthrough: the structural gates
  intentionally do not run there, and #13's metric gates alone judge them.
* A fully enclosed body shell without any distinct section shows no ownership
  or mass evidence on the Preview surface and resolves like a valid local-detail
  garment; this accepted ceiling follows from Issue #18's out-of-scope rule
  against reliably separating welded geometry that presents no observable
  evidence, and heavy mixed-section imports are still caught by the mass
  boundary above.

Boundary tests: `MtoULiveLink.Editor.GarmentSurface.Auto` and
`MtoULiveLink.Editor.GarmentSurface.Failures` (exact and near-shifted duplicate
ambiguity, source-mass rejection) replace the former
`MtoULiveLink.Editor.Preview.GarmentResolution` and
`MtoULiveLink.Editor.Preview.GarmentFaultLines` rule assertions at the private
seam, and the existing quality/boundary tests retain values immediately above
and below every calibrated Ready, Warning, and Error boundary of
`MtoUEvaluatePreviewQuality`.

## Stock full-character corpus (synthetic fixture)

| Case | Vertices | Inpaint ratio | Distance avg | Distance max | Regions | Resolved tris | Total refresh | Status |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| SameTopology | 40 | 0.40000 | 0.000000 | 0.000000 | 2 | 72/216 | ~7 ms | Warning |
| LocalRetopology | 40 | 0.35000 | 0.000000 | 0.000000 | 2 | 72/216 | ~5 ms | Warning |
| DoubleLayerSeams | 82 | 0.26829 | 0.003297 | 0.023378 | 3 | 108/216 | ~8 ms | Ready |
| MisalignedNegative | rejected | — | — | — | 0 | 0 | ~2 ms | Error |

The diagnostic SameTopology and LocalRetopology cube ratios exceed 0.27 only
because the toy shells inpaint a large share of their vertices; they
demonstrate the warning band rather than approved quality. DoubleLayerSeams is
the calibrated Ready-side boundary row.

## External production corpus SK_C01_Clothes_09_All + SM_C01_Clothes_09

Logical identities recorded without committing private assets or machine paths:

| Logical name | Content | SHA-256 |
| --- | --- | --- |
| `SK_C01_Clothes_09_All` | imported full-character Skeletal Mesh (Driver), LOD0 150,772 triangles | `24CCBB57E2C0734569F6D9281197FBAB6AD087457CFEDF4C7197A031AFFFFDC4` |
| `SM_C01_Clothes_09` | imported garment-only Static Mesh (Preview) | `4840BC461952F761B3C7D94619E3239C8A4E374C37D42AC363A9983099ECB34E` |

Automatic rows use the five clothing material regions as supporting evidence —
`M_C01_Clothes09_ChenShan02`, `M_C01_Clothes09_KuZi`,
`M_C01_Clothes09_MaJia1`, `M_C01_Clothes09_PiDai1`,
`M_C01_Clothes09_Shoes` — then validate the selected geometry against the whole
Preview. This excludes the skin-tight body surfaces that previously inflated
nearest-only Auto to 64,050 triangles (2.31x) without making material equality
mandatory; missing or replaced material evidence still falls back to geometry.

| Case | Vertices | Inpaint ratio | Distance avg | Distance max | Regions | Resolved tris | Morphs (sparse deltas) | Total refresh | Status |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| SameTopology | 14,389 | 0.20988 | 0.000321 | 0.010329 | 45 | 27,630/150,772 | 27 projected (+82 skipped), 5,516 | ~3.02 s | Ready |
| LocalRetopology | 14,390 | 0.20987 | 0.000321 | 0.010329 | 45 | 27,630/150,772 | 27 projected (+82 skipped), 5,516 | ~2.81 s | Ready |
| DoubleLayerSeams | rejected | — | — | — | — | coverage 28,147/28,780 within radius | — | ~1.21 s | Error |
| MisalignedNegative | rejected | — | zero coverage | — | — | — | — | ~0.53 s | Error |

Resolved-source details captured from Auto diagnostics: 45 connected regions
across the five slots, matched Preview coverage 1.0000, manual source `false`,
and body/face/hair regions absent from the resolved surface. Timings are
single-run headless feasibility figures.

## Threshold decision

Measured evidence retains every Issue #13 threshold unchanged:

* Approved production revisions measure inpaint ratio 0.20987–0.20988, inside
  the existing Ready anchor 0.27 (baseline largest approved measurement was
  0.26532); no new band between them is warranted.
* Normalized distance averages under the garment-scale basis measure
  0.000321 on the approved production pairing and remain orders below the
  misalignment bound 0.60; misaligned negatives measure no nearer than the
  coverage bound itself, consistent with the 1.71 anchors of Issue #13.
* The DoubleLayerSeams layered preview variant rejects during automatic
  resolution because its shifted inner-layer vertices fall outside the
  agreement radius of the real asset's garment surface; this documents the
  strict whole-garment coverage contract on production assets rather than any
  threshold change. Layered WIP iterations closer to the original surface
  remain covered, as the synthetic rows show.

No constant changed except the new resolution boundaries above (mass 1.70x,
twin proximity/agreement shares); Issue #32 added no constant at all, only the
shared zero-area admission gate and the Preview-triangle-count mass floor,
recorded next to their implementation comments.

## Same-surface topology reduction (Issue #40, 2026-09-08)

Stock UE 5.7.4 Win64 (5.7.4-51494982) reproduced the suspected false rejection
before the runtime diagnostic changed. `Preview.ReducedTopology` extends
`MakeFullCharacterFixtures` through `MtoUPreparePreview` and the public
`RefreshActor` entry point. The full Driver has 216 triangles across six
connected components: body, face, hair, arm, and two garment shells. The
garment contributes 72 triangles (36 per shell) in final slots 3/4/5,
`Garment_Upper_A`, `Garment_Upper_B`, and `Garment_Lower`.

The Preview uses the identical upper/lower transforms and surface positions.
Starting with two welded coarse cubes (24 triangles total), a deterministic
planar triangle poke replaces one triangle with three without changing its
surface. Ten pokes produce 44 triangles; nine produce 42; zero produce 24.
The original 72-triangle fixture is unchanged. Material assignments, shell
count, bounds, reference pose, and full-character Driver stay equivalent;
only Preview triangulation density changes. The 44/42 counts are the nearest
attainable even counts on either side of 72/1.70 for this construction.

| Preview tris / vertices | Selected Driver / Preview ratio | Auto preparation and Refresh | Manual preparation and Refresh | Manual inpaint ratio |
| --- | ---: | --- | --- | ---: |
| 72 / 40 | 1.000000 | Usable Warning, 2 regions / 72 tris | Usable Warning, 2 regions / 72 tris | 0.400000 |
| 44 / 26 | 1.636364 | Usable Warning, 2 regions / 72 tris | Usable Warning, 2 regions / 72 tris | 0.461538 |
| 42 / 25 | 1.714286 | Source-mass Error, no Generated Preview | Usable Warning, 2 regions / 72 tris | 0.400000 |
| 24 / 16 | 3.000000 | Source-mass Error, no Generated Preview | Usable Warning, 2 regions / 72 tris | 0.000000 (closest fallback) |

Every accepted row selects exactly garment slots 3/4/5, excluding body, face,
hair, and arm. Manual whole-Preview coverage is 1.000000 and normalized maximum
surface distance is 0.000000000 in all four rows. Rejected Auto diagnostics
report the 72-triangle candidate count; no candidate is committed as a usable
surface. Public Refresh restores the Driver, returns Error with no stale
Generated Preview, then becomes usable after the explicit garment-slot override.
Warnings remain honest: the first three toy rows exceed the Ready inpaint
ratio; the 24-triangle case uses the existing closest-point fallback after the
inpaint solve fails, so zero reported inpaint ratio is not a Ready claim.

**Decision:** retain the calibrated 1.70 ceiling as an intentional conservative
Auto support limit (more than one Driver connected component, Preview at least
8 triangles; equality is permitted). These rows prove density sensitivity,
not a new discriminator between reduced garments and unsafe mixed sources.
Raising the limit to accommodate the 3.00x example would remove rejection at
the original 2.00x duplicate-mass and 2.31x proximity-mixing anchors. No general
topology matcher or new dependency is justified by these planar fixtures.
The narrow correction is diagnostic: explicitly name legitimate reduction as
a possible cause, state that triangle count does not prove duplicate/foreign
geometry, and explain the verified manual remedy. Both READMEs now describe
the actual limit and require source inspection plus distinct garment slots.
Manual selection retains geometry, coverage, alignment, shared-slot, and
transfer-quality checks; it is not permission to select duplicated garments.

**Retained controls and verification:**

- `GarmentSurface.Failures`: exact/shifted duplicates, source-mass refusal,
  and misalignment; `GarmentSurface.Auto` retains full-character geometry
  isolation from the underlying body and shared-slot/mapping refusal.
- `GarmentSurface.MaterialEvidence`, `Manual`, `GeometryEdgeCases`, and
  `RenamedSlot`: final-slot evidence, fail-closed manual selection, tiny
  Preview floor/twin ambiguity, invalid/collinear/non-finite geometry.
- `Preview.GarmentOverride`, `InvalidGeometry`, `Misalignment`,
  `FullCharacterQualityCorpus`, and `QualityBoundaries` retain their existing
  safety and readiness assertions; their acceptance thresholds are unchanged.
- Initial `Automation RunTests MtoULiveLink.Editor.Preview.ReducedTopology`:
  passed against the original diagnostic and selection policy, establishing
  the measured rejection before the correction.
- Final `Build.bat UnrealEditor Win64 Development`: succeeded, no compiler
  warnings. A disposable copy of the supplied Backups host descriptor loaded
  the independently copied plugin; the existing host project was untouched.
- Final `Automation RunTests MtoULiveLink`: 55/55 passed (49 without warnings,
  6 with diagnostic warnings; 0 failed, 0 not run), including ReducedTopology.
- `python tools/package_unreal_plugin.py MtoULiveLink --engine 5.7 --json`:
  dry-run passed, 32 files. `git diff --check`: passed.

This is deterministic synthetic calibration and headless UE host evidence,
not arbitrary-retopology support or production visual acceptance. No new C01
asset run, Maya host test, or Topia build was performed: this slice changes
only the Unreal selection diagnostic and its corpus/documentation. The #22
external production rows and #13 quality baseline above were not rerun or
replaced. No generated files or machine-specific paths belong in the commit.
