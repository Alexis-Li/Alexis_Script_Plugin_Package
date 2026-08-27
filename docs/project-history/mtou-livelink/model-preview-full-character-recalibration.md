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
  average 0.002199 / max 0.031879 while its legacy pairing measures the same
  order, confirming character bounds never enter the normalization.

## Resolution failure boundaries

Automatic resolution keeps nearest-ownership selection and adds two structural
boundaries (constants live beside `ResolveDriverGarmentSurface`):

* Source-mass boundary — selected Driver triangles must stay within 1.70x of
  Preview triangles (`MaxDriverToPreviewTriangleRatio`). Whole duplicated
  garments measure 2.00x+ and production proximity mixing measured 2.31x/4.49x,
  all rejected deterministically with actionable diagnostics naming both
  remedies (remove duplicates/split sections, or the manual Driver Garment Slot
  Override).
* Twin-family boundary — selected regions cluster into families by mutual
  near-face-for-face coincidence (corner-sample share within 2% of Preview scale
  at or above 98% both directions). When two or more families each independently
  cover at least 90% of agreeing vertices, the candidates are indistinguishable
  and resolution fails deterministically. Because nearest-ownership lets an
  exact overlay win every vertex, synthetic near-twin shells resolve stably onto
  the supported family instead; the production hazard equivalent is the mass
  boundary above, which rejected measured mixing at 2.31x. Legitimate garment
  pieces neither coincide nor each cover the whole Preview, so a unique garment
  never forms two qualifying families.
* Deterministic stability: for exact duplicate copies nearest-ownership hands
  every vertex to one family and Refresh resolves stably onto that garment
  family (verified by repeated Prepare calls with identical region/triangle
  counts and distances). Selection can therefore map clothing only onto
  garment-like surfaces, never silently onto the body; a redundant exact twin
  is inert.
* Single-region whole Drivers are the legacy passthrough: the structural gates
  intentionally do not run there, and #13's metric gates alone judge them.
* A fully enclosed body shell without any distinct section shows no ownership
  or mass evidence on the Preview surface and resolves like a valid local-detail
  garment; this accepted ceiling follows from Issue #18's out-of-scope rule
  against reliably separating welded geometry that presents no observable
  evidence, and heavy mixed-section imports are still caught by the mass
  boundary above.

Boundary tests: `MtoULiveLink.Editor.Preview.GarmentFaultLines` (duplicate
stability, divergent overlap stability) and the existing quality/boundary tests
retain values immediately above and below every calibrated Ready, Warning, and
Error boundary of `MtoUEvaluatePreviewQuality`.

## Stock full-character corpus (synthetic fixture)

| Case | Vertices | Inpaint ratio | Distance avg | Distance max | Regions | Resolved tris | Total refresh | Status |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| SameTopology | 48 | 0.66667 | 0.002199 | 0.031879 | 17 | 34/72 | ~4 ms | Warning |
| LocalRetopology | 49 | 0.65306 | 0.002180 | 0.031879 | 17 | 34/72 | ~4 ms | Warning |
| DoubleLayerSeams | 98 | 0.65306 | 0.002636 | 0.018220 | 18 | 36/72 | ~4 ms | Warning |
| MisalignedNegative | rejected | — | — | — | 0 | 0 | ~2 ms | Error |

The diagnostic cube ratios exceed 0.27 only because the toy shells inpaint
half their vertices; they demonstrate the warning band rather than approved
quality.

## External production corpus SK_C01_Clothes_09_All + SM_C01_Clothes_09

Logical identities recorded without committing private assets or machine paths:

| Logical name | Content | SHA-256 |
| --- | --- | --- |
| `SK_C01_Clothes_09_All` | imported full-character Skeletal Mesh (Driver), LOD0 150,772 triangles | `24CCBB57E2C0734569F6D9281197FBAB6AD087457CFEDF4C7197A031AFFFFDC4` |
| `SM_C01_Clothes_09` | imported garment-only Static Mesh (Preview) | `4840BC461952F761B3C7D94619E3239C8A4E374C37D42AC363A9983099ECB34E` |

Automatic resolution rows (harness defaults): Auto selected 64,050 triangles —
2.31x of the Preview — because skin-tight contact lets underlying body surfaces
win nearest-ownership ties across tight-fitting regions. That inflation exceeds
the source-mass boundary, so Auto refuses transactionally instead of letting
body surfaces inherit weights; MisalignedNegative remains refused by coverage.
This is precisely the case the manual Driver Garment Slot Override exists for.

Manual override rows via `MTOU_QUALITY_SLOTS` with the five clothing material
regions `M_C01_Clothes09_ChenShan02`, `M_C01_Clothes09_KuZi`,
`M_C01_Clothes09_MaJia1`, `M_C01_Clothes09_PiDai1`,
`M_C01_Clothes09_Shoes`:

| Case | Vertices | Inpaint ratio | Distance avg | Distance max | Regions | Resolved tris | Morphs (sparse deltas) | Total refresh | Status |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| SameTopology | 14,389 | 0.20988 | 0.000321 | 0.010329 | 45 | 27,630/150,772 | 27 projected (+82 skipped), 5,516 | ~1.70 s | Ready |
| LocalRetopology | 14,390 | 0.20987 | 0.000321 | 0.010329 | 45 | 27,630/150,772 | 27 projected (+82 skipped), 5,516 | ~1.32 s | Ready |
| DoubleLayerSeams | rejected | — | — | — | — | coverage 28,075/28,780 within radius | — | ~0.38 s | Error |
| MisalignedNegative | rejected | — | zero coverage | — | — | — | — | ~0.28 s | Error |

Resolved-source details captured from diagnostics: 45 connected regions across
the five slots, matched Preview coverage 1.0000, body/face/hair regions absent
from the resolved surface. Timings are single-run headless feasibility figures.

## Threshold decision

Measured evidence retains every Issue #13 threshold unchanged:

* Approved production revisions measure inpaint ratio 0.20987–0.20988, inside
  the existing Ready anchor 0.27 (baseline largest approved measurement was
  0.26532); no new band between them is warranted.
* Normalized distance averages under the garment-scale basis measure
  0.000321 on the approved production pairing and remain orders below the
  misalignment bound 0.60; misaligned negatives measure no nearer than the
  coverage bound itself, consistent with the 1.71 anchors of Issue #13.
* The DoubleLayerSeams layered preview variant rejects during manual
  resolution because its shifted inner-layer vertices fall outside the
  agreement radius of the real asset's garment surface; this documents the
  strict whole-garment coverage contract on production assets rather than any
  threshold change. Layered WIP iterations closer to the original surface
  remain covered, as the synthetic rows show.

No constant changed except the new resolution boundaries above (mass 1.70x,
twin proximity/agreement/union shares), which are first-calibrated here and
recorded next to their implementation comments.
